#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace bassifier;

namespace
{
    constexpr float kGainMuteDb = -60.0f;

    void storeMax (std::atomic<float>& target, float value) noexcept
    {
        float current = target.load (std::memory_order_relaxed);
        while (value > current && ! target.compare_exchange_weak (current, value, std::memory_order_relaxed))
        {
        }
    }

    juce::String dbText (float v, bool sign)
    {
        const auto number = juce::String (std::abs (v) < 0.05f ? 0.0f : v, 1);
        return (sign && v >= 0.05f ? "+" : "") + number + " dB";
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout BassifierProcessor::createLayout()
{
    using Range = juce::NormalisableRange<float>;
    using Float = juce::AudioParameterFloat;
    using Attributes = juce::AudioParameterFloatAttributes;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Freq is logarithmic across its three octaves, so 70 Hz sits a little below the middle.
    Range freqRange (32.0f, 256.0f,
                     [] (float start, float end, float n) { return start * std::pow (end / start, n); },
                     [] (float start, float end, float v) { return std::log (v / start) / std::log (end / start); },
                     [] (float start, float end, float v) { return juce::jlimit (start, end, std::round (v * 10.0f) * 0.1f); });

    layout.add (std::make_unique<Float> (juce::ParameterID { ParamID::freq, 1 }, "Freq", freqRange, 70.0f,
                                         Attributes().withLabel ("Hz").withStringFromValueFunction ([] (float v, int)
                                         {
                                             return (v < 99.95f ? juce::String (v, 1) : juce::String (juce::roundToInt (v))) + " Hz";
                                         })));

    layout.add (std::make_unique<Float> (juce::ParameterID { ParamID::intensity, 1 }, "Intensity",
                                         Range (-24.0f, 24.0f, 0.1f), -24.0f,
                                         Attributes().withLabel ("dB").withStringFromValueFunction ([] (float v, int) { return dbText (v, true); })));

    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { ParamID::originalBass, 1 }, "Original Bass", true,
                                                            juce::AudioParameterBoolAttributes().withStringFromValueFunction (
                                                                [] (bool on, int) { return on ? "In" : "Out"; })));

    layout.add (std::make_unique<Float> (juce::ParameterID { ParamID::gain, 1 }, "Gain",
                                         Range (kGainMuteDb, 0.0f, 0.1f), 0.0f,
                                         Attributes().withLabel ("dB").withStringFromValueFunction ([] (float v, int)
                                         {
                                             return v <= kGainMuteDb + 0.05f ? juce::String ("-inf dB") : dbText (v, false);
                                         })));

    return layout;
}

BassEngine::Params BassifierProcessor::currentParams() const noexcept
{
    BassEngine::Params p;
    p.freqHz = param (ParamID::freq);
    p.intensityDb = param (ParamID::intensity);
    p.originalBass = param (ParamID::originalBass) > 0.5f;
    p.outputGainDb = param (ParamID::gain);
    return p;
}

BassifierProcessor::BassifierProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Bassifier", createLayout()),
      analyserBuffer ((size_t) kAnalyserFifoSize, 0.0f)
{
}

bool BassifierProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo())
           && out == layouts.getMainInputChannelSet();
}

void BassifierProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock);
    engine.setParams (currentParams());
    setLatencySamples (engine.getLatencySamples());
}

void BassifierProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin (getTotalNumInputChannels(), BassEngine::kMaxChannels);

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    engine.setParams (currentParams());
    engine.process (buffer.getArrayOfWritePointers(), numChannels, numSamples);

    const auto meters = engine.getMeters();
    storeMax (peakBass, meters.originalBassPeak);
    storeMax (peakHarmonics, meters.harmonicsPeak);
    storeMax (peakOutput, meters.outputPeak);
    if (meters.clipped)
        clipped.store (true, std::memory_order_relaxed);

    // The output, summed to mono, for the editor's analyser. Never waits: if the editor is not
    // reading, whatever does not fit is dropped.
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    analyserFifo.prepareToWrite (numSamples, start1, size1, start2, size2);

    const float scale = 1.0f / (float) juce::jmax (1, numChannels);
    const auto writeRun = [&] (int start, int size, int offset)
    {
        for (int i = 0; i < size; ++i)
        {
            float sum = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
                sum += buffer.getSample (ch, offset + i);
            analyserBuffer[(size_t) (start + i)] = sum * scale;
        }
    };
    writeRun (start1, size1, 0);
    writeRun (start2, size2, size1);
    analyserFifo.finishedWrite (size1 + size2);
}

BassifierProcessor::MeterPeaks BassifierProcessor::takeMeterPeaks() noexcept
{
    MeterPeaks peaks;
    peaks.originalBass = peakBass.exchange (0.0f, std::memory_order_relaxed);
    peaks.harmonics = peakHarmonics.exchange (0.0f, std::memory_order_relaxed);
    peaks.output = peakOutput.exchange (0.0f, std::memory_order_relaxed);
    peaks.clipped = clipped.exchange (false, std::memory_order_relaxed);
    return peaks;
}

int BassifierProcessor::readAnalyser (float* dest, int maxSamples) noexcept
{
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    analyserFifo.prepareToRead (maxSamples, start1, size1, start2, size2);

    if (size1 > 0)
        std::copy_n (analyserBuffer.data() + start1, size1, dest);
    if (size2 > 0)
        std::copy_n (analyserBuffer.data() + start2, size2, dest + size1);

    analyserFifo.finishedRead (size1 + size2);
    return size1 + size2;
}

void BassifierProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (const auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void BassifierProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* BassifierProcessor::createEditor()
{
    return new BassifierEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BassifierProcessor();
}
