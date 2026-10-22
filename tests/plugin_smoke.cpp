#include "../Source/PluginProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace
{
int failures = 0;

void check (bool condition, const char* message)
{
    std::printf ("  [%s] %s\n", condition ? "PASS" : "FAIL", message);
    if (! condition)
        ++failures;
}

void setParameter (BassifierProcessor& processor, const char* id, float plainValue)
{
    auto* parameter = processor.apvts.getParameter (id);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (plainValue));
}

bool finiteBuffer (const juce::AudioBuffer<float>& buffer)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            if (! std::isfinite (buffer.getSample (channel, sample)))
                return false;
    return true;
}

void checkParameter (BassifierProcessor& processor, const char* id, float min, float max, float defaultValue)
{
    auto* parameter = processor.apvts.getParameter (id);
    check (parameter != nullptr, id);
    if (parameter == nullptr)
        return;

    check (std::abs (parameter->getNormalisableRange().start - min) < 0.001f, "parameter minimum is correct");
    check (std::abs (parameter->getNormalisableRange().end - max) < 0.001f, "parameter maximum is correct");
    check (std::abs (parameter->convertFrom0to1 (parameter->getDefaultValue()) - defaultValue) < 0.001f,
           "parameter default is correct");
}

void renderLayout (BassifierProcessor& processor, const juce::AudioChannelSet& channels)
{
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add (channels);
    layout.outputBuses.add (channels);
    const bool accepted = processor.setBusesLayout (layout);
    check (accepted, channels == juce::AudioChannelSet::mono() ? "mono layout accepted" : "stereo layout accepted");
    if (! accepted)
        return;

    constexpr int blockSize = 256;
    constexpr double sampleRate = 48000.0;
    processor.prepareToPlay (sampleRate, blockSize);
    juce::AudioBuffer<float> audio (channels.size(), blockSize);
    juce::MidiBuffer midi;

    for (int block = 0; block < 24; ++block)
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto n = block * blockSize + sample;
            const float value = 0.35f * std::sin (juce::MathConstants<double>::twoPi * 55.0 * n / sampleRate);
            for (int channel = 0; channel < audio.getNumChannels(); ++channel)
                audio.setSample (channel, sample, value * (channel == 0 ? 1.0f : 0.8f));
        }
        processor.processBlock (audio, midi);
    }

    check (finiteBuffer (audio), "processed audio remains finite");
    const auto meters = processor.takeMeterPeaks();
    check (meters.originalBass > 0.0f, "original-bass meter receives signal");
    check (meters.harmonics > 0.0f, "created-harmonics meter receives signal");
    check (meters.output > 0.0f, "output meter receives signal");

    float analyser[blockSize];
    const int analysed = processor.readAnalyser (analyser, blockSize);
    check (analysed == blockSize, "analyser publishes a complete block");
    check (std::all_of (analyser, analyser + analysed, [] (float sample) { return std::isfinite (sample); }),
           "analyser samples remain finite");
}

void checkClipFlag (BassifierProcessor& processor)
{
    setParameter (processor, ParamID::freq, 256.0f);
    setParameter (processor, ParamID::intensity, 24.0f);
    setParameter (processor, ParamID::originalBass, 1.0f);
    setParameter (processor, ParamID::gain, 0.0f);

    constexpr int blockSize = 256;
    processor.prepareToPlay (48000.0, blockSize);
    juce::AudioBuffer<float> audio (2, blockSize);
    juce::MidiBuffer midi;
    for (int block = 0; block < 24; ++block)
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const float value = ((block * blockSize + sample) / 600) % 2 == 0 ? 1.0f : -1.0f;
            audio.setSample (0, sample, value);
            audio.setSample (1, sample, value);
        }
        processor.processBlock (audio, midi);
    }
    check (processor.takeMeterPeaks().clipped, "output clip flag latches an over-range signal");
}
}

int main()
{
    const juce::ScopedJuceInitialiser_GUI gui;
    BassifierProcessor processor;

    std::printf ("Bassifier JUCE host/state smoke test\n");
    check (processor.getLatencySamples() == 0, "reports zero latency");
    check (processor.getParameters().size() == 4, "publishes exactly four parameters");
    checkParameter (processor, ParamID::freq, 32.0f, 256.0f, 100.0f);
    checkParameter (processor, ParamID::intensity, -24.0f, 24.0f, 0.0f);
    checkParameter (processor, ParamID::originalBass, 0.0f, 1.0f, 1.0f);
    checkParameter (processor, ParamID::gain, -60.0f, 0.0f, 0.0f);

    juce::AudioProcessor::BusesLayout mismatched;
    mismatched.inputBuses.add (juce::AudioChannelSet::mono());
    mismatched.outputBuses.add (juce::AudioChannelSet::stereo());
    check (! processor.isBusesLayoutSupported (mismatched), "rejects mismatched mono/stereo layout");

    setParameter (processor, ParamID::freq, 173.0f);
    setParameter (processor, ParamID::intensity, 7.5f);
    setParameter (processor, ParamID::originalBass, 0.0f);
    setParameter (processor, ParamID::gain, -8.0f);

    juce::MemoryBlock state;
    processor.getStateInformation (state);
    check (! state.isEmpty(), "APVTS serialises state");

    BassifierProcessor restored;
    restored.setStateInformation (state.getData(), (int) state.getSize());
    const auto restoredValue = [&restored] (const char* id) { return restored.apvts.getRawParameterValue (id)->load(); };
    check (std::abs (restoredValue (ParamID::freq) - 173.0f) < 0.11f, "Freq state round-trips");
    check (std::abs (restoredValue (ParamID::intensity) - 7.5f) < 0.11f, "Intensity state round-trips");
    check (restoredValue (ParamID::originalBass) < 0.5f, "Original Bass state round-trips");
    check (std::abs (restoredValue (ParamID::gain) + 8.0f) < 0.11f, "Gain state round-trips");

    renderLayout (restored, juce::AudioChannelSet::mono());
    renderLayout (restored, juce::AudioChannelSet::stereo());
    checkClipFlag (restored);

    std::printf ("%s: %d failure(s)\n", failures == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED", failures);
    return failures == 0 ? 0 : 1;
}
