#pragma once

#include "PluginProcessor.h"

#include <juce_dsp/juce_dsp.h>

#include <array>

/** The embedded typefaces. Shared by all open editors and released with the last one, so
    nothing outlives the host's JUCE shutdown. */
struct EditorFonts
{
    EditorFonts();
    juce::Typeface::Ptr inter, mono;
};

/** Flat drawing for the knobs; everything else paints itself. */
class FlatLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float position,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawCornerResizer (juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;
};

/** A flat disc with a dot, its name above and its value below. Double-click the disc for the
    default; double-click the value to type one. */
class Knob final : public juce::Component
{
public:
    Knob (juce::AudioProcessorValueTreeState&, const char* paramId, const juce::String& name, const juce::String& help,
          int diameter = 64, float sweepDegrees = 270.0f);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    static constexpr int kLabelHeight = 16, kGap = 8, kCellWidth = 84;
    /** Label, disc and value. */
    int preferredHeight() const noexcept { return 2 * (kLabelHeight + kGap) + diameter; }

private:
    void finishEditing (bool apply);

    juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::String name;
    int diameter;
    juce::TextEditor editor;
    juce::SliderParameterAttachment attachment;
};

/** "LABEL  OPTION  OPTION" as plain text; the selected option is lit. Each option carries the
    parameter value it stands for, so the display order need not be the parameter's. */
class OptionRow final : public juce::Component
{
public:
    OptionRow (juce::RangedAudioParameter&, const juce::String& label, juce::StringArray options,
               std::vector<float> values, const juce::String& help);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    /** The label's own width; rows stacked with the meters share the widest plus the label gap. */
    int labelTextWidth() const;
    void setLabelColumn (int width) { labelColumn = width; repaint(); }

private:
    /** Horizontal extent of each option, in component coordinates. */
    std::vector<juce::Range<int>> optionRanges() const;
    int optionAt (int x) const;

    juce::String label;
    juce::StringArray options;
    std::vector<float> values;
    int labelColumn = 0;
    int selected = 0, hovered = -1;
    juce::ParameterAttachment attachment;
};

/** The output's spectrum, 20 Hz to 2 kHz, with the Freq crossover as a draggable line. */
class SpectrumDisplay final : public juce::Component
{
public:
    explicit SpectrumDisplay (juce::RangedAudioParameter& freqParam);

    /** Mono output samples from the processor. Call analyse() once they are in. */
    void push (const float* samples, int numSamples) noexcept;
    void analyse();

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    void setSampleRate (double rate) noexcept { sampleRate = rate; }

    static constexpr int kFftOrder = 12, kFftSize = 1 << kFftOrder;

private:
    juce::Rectangle<float> plotArea() const;
    float xForHz (float hz) const;
    float hzForX (float x) const;
    float yForDb (float db) const;
    float dbAtHz (float hz) const;
    bool isOverFreqLine (juce::Point<float>) const;

    juce::dsp::FFT fft { kFftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) kFftSize, juce::dsp::WindowingFunction<float>::hann, false };
    std::array<float, (size_t) kFftSize> ring {};
    std::array<float, (size_t) kFftSize * 2> fftData {};
    std::array<float, (size_t) kFftSize / 2 + 1> smoothedDb {};
    int writeIndex = 0;
    bool dirty = false;
    double sampleRate = 48000.0;

    juce::RangedAudioParameter& freq;
    float freqHz = 100.0f;
    bool dragging = false;
    juce::ParameterAttachment attachment;
};

/** A horizontal level bar with a falling peak tick and the held peak as a number. The output
    meter also has a clip light. Clicking resets the number and the light. */
class Meter final : public juce::Component
{
public:
    Meter (const juce::String& label, bool hasClipLight, const juce::String& help);

    int labelTextWidth() const;
    void setLabelColumn (int width) { labelColumn = width; repaint(); }

    /** Peak of the last interval, linear. Call once per frame. */
    void push (float peak, bool clipped);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

    static constexpr float kFloorDb = -48.0f;

private:
    juce::String label;
    bool hasClipLight;
    int labelColumn = 0;
    float levelDb = -100.0f, tickDb = -100.0f, heldDb = -100.0f;
    int tickHold = 0;
    bool clipLit = false;
};

/** Everything inside the window, laid out at the design size; the editor scales it. */
class EditorView final : public juce::Component
{
public:
    static constexpr int kWidth = 560, kHeight = 430;

    explicit EditorView (BassifierProcessor&);

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Pulls the analyser and the meters from the processor. */
    void poll();
    /** Shows the help of the control under the mouse (nullptr: the default text). */
    void showHelpFor (juce::Component*);

private:
    BassifierProcessor& plugin;

    SpectrumDisplay display;
    Knob freq, intensity, gain;
    OptionRow originalBass;
    Meter bassMeter, harmonicsMeter, outputMeter;

    std::array<float, 8192> pullBuffer {};
    juce::String helpTitle, helpText;
    juce::Rectangle<int> helpArea;
};

class BassifierEditor final : public juce::AudioProcessorEditor,
                              private juce::Timer
{
public:
    explicit BassifierEditor (BassifierProcessor&);
    ~BassifierEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** For the offline snapshot tool, which has no host message loop: what the timer does. */
    void pollForSnapshot() { view.poll(); }

private:
    void timerCallback() override;

    BassifierProcessor& owner;
    juce::SharedResourcePointer<EditorFonts> fonts;
    FlatLookAndFeel lookAndFeel;
    EditorView view;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BassifierEditor)
};
