#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

// The engine's Ramp compares a float target for equality on purpose; JUCE's warning flags object.
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")
#include "dsp/BassEngine.h"
JUCE_END_IGNORE_WARNINGS_GCC_LIKE

#include <vector>

namespace ParamID
{
    inline constexpr auto freq = "freq", intensity = "intensity", originalBass = "originalBass", gain = "gain";
}

class BassifierProcessor final : public juce::AudioProcessor
{
public:
    /** Peaks since the editor last asked, linear. */
    struct MeterPeaks
    {
        float originalBass = 0.0f, harmonics = 0.0f, output = 0.0f;
        bool clipped = false;
    };

    BassifierProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // UI side
    juce::AudioProcessorValueTreeState apvts;

    /** The peaks since the last call, which are then forgotten. The clip flag is sticky in the same way. */
    MeterPeaks takeMeterPeaks() noexcept;

    /** Copies up to maxSamples of the output (summed to mono) that the audio thread has produced
        since the last read. Returns how many were copied. */
    int readAnalyser (float* dest, int maxSamples) noexcept;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    bassifier::BassEngine::Params currentParams() const noexcept;
    float param (const char* id) const noexcept { return apvts.getRawParameterValue (id)->load(); }

    bassifier::BassEngine engine;

    // Audio thread writes, editor reads. When nobody reads, what does not fit is dropped.
    static constexpr int kAnalyserFifoSize = 32768;
    juce::AbstractFifo analyserFifo { kAnalyserFifoSize };
    std::vector<float> analyserBuffer;

    std::atomic<float> peakBass { 0.0f }, peakHarmonics { 0.0f }, peakOutput { 0.0f };
    std::atomic<bool> clipped { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BassifierProcessor)
};
