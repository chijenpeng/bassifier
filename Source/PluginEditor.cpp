#include "PluginEditor.h"

#include <BinaryData.h>

namespace
{
    // Flat and two-dimensional: a near-black ground, white type, no borders, and knobs as plain
    // pale discs. The blue accent is level: the meter bars and the Freq handle.
    namespace Theme
    {
        const juce::Colour background { 0xff0c1416 }, panel { 0xff17262a }, text { 0xffffffff }, label { 0xff9fb3b3 },
                           accent { 0xff2bd4b4 }, clipping { 0xffff5a5a }, field { 0xff2f4247 };

        // Pale teal, most of the way to white: the things you control (knobs, selected options)
        // and the plug-in's name. Names of controls are in the quieter label grey.
        const juce::Colour pale { 0xffcdf6ee };

        juce::Font make (const juce::Typeface::Ptr& typeface, float height, float tracking)
        {
            return juce::Font (juce::FontOptions (typeface).withHeight (height)).withExtraKerningFactor (tracking);
        }

        /** Labels are upper-case and tracked out; pass upper-case text. */
        juce::Font labelFont (float height = 11.0f)  { return make (juce::SharedResourcePointer<EditorFonts>()->inter, height, 0.09f); }
        juce::Font titleFont (float height)          { return make (juce::SharedResourcePointer<EditorFonts>()->inter, height, 0.18f); }
        juce::Font bodyFont (float height = 12.0f)   { return make (juce::SharedResourcePointer<EditorFonts>()->inter, height, 0.01f); }
        juce::Font valueFont (float height = 12.0f)  { return make (juce::SharedResourcePointer<EditorFonts>()->mono, height, 0.0f); }

        int widthOf (const juce::Font& font, const juce::String& s)
        {
            return juce::roundToInt (std::ceil (juce::GlyphArrangement::getStringWidth (font, s)));
        }
    }

    const juce::Identifier helpId ("help"), helpTitleId ("helpTitle");

    constexpr float kPlotMinHz = 20.0f, kPlotMaxHz = 2000.0f, kPlotFloorDb = -72.0f;
    // Spacing grammar of an option row: sibling options closest, the label further from its
    // first option. The meters' names share the option row's label column.
    constexpr int kOptionGap = 10, kLabelGap = 18, kRowHeight = 18;
    const juce::Identifier editorScaleId ("editorScale");
    constexpr auto kDefaultHelpTitle = "BASSIFIER";
    constexpr auto kDefaultHelp = "Turns the bass below Freq into harmonics the ear hears as the same note, so the low end "
                                  "comes through on speakers that cannot play it. Hover over a control to see what it does; "
                                  "double-click a knob for its default, or its value to type one.";

    float toDb (float linear) noexcept { return 20.0f * std::log10 (std::max (linear, 1.0e-6f)); }

    juce::String meterText (float db)
    {
        return db < -99.0f ? juce::String ("-inf") : juce::String (db, 1);
    }

    void setHelp (juce::Component& c, const juce::String& title, const juce::String& help)
    {
        c.getProperties().set (helpTitleId, title.toUpperCase());
        c.getProperties().set (helpId, help);
    }
}

//==============================================================================
EditorFonts::EditorFonts()
    : inter (juce::Typeface::createSystemTypefaceFor (BinaryData::InterSemiBold_ttf, BinaryData::InterSemiBold_ttfSize)),
      mono (juce::Typeface::createSystemTypefaceFor (BinaryData::JetBrainsMonoMedium_ttf, BinaryData::JetBrainsMonoMedium_ttfSize))
{
}

//==============================================================================
void FlatLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float position,
                                        float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const auto centre = bounds.getCentre();
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f - 1.0f;
    const float angle = startAngle + position * (endAngle - startAngle);
    const float dot = juce::jmax (2.5f, radius * 0.11f);

    g.setColour (slider.isMouseOverOrDragging() ? Theme::pale.brighter (0.1f) : Theme::pale);
    g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre));

    g.setColour (Theme::background);
    g.fillEllipse (juce::Rectangle<float> (dot * 2.0f, dot * 2.0f).withCentre (centre.getPointOnCircumference (radius * 0.72f, angle)));
}

void FlatLookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool isMouseOver, bool isMouseDragging)
{
    // Three short diagonals in the label grey, quiet until touched.
    g.setColour (Theme::label.withAlpha (isMouseOver || isMouseDragging ? 0.8f : 0.3f));
    const float size = (float) juce::jmin (w, h);
    for (float i = 0.3f; i < 1.0f; i += 0.3f)
        g.drawLine ((float) w - size * i, (float) h - 2.0f, (float) w - 2.0f, (float) h - size * i, 1.0f);
}

//==============================================================================
Knob::Knob (juce::AudioProcessorValueTreeState& state, const char* paramId, const juce::String& nameToUse, const juce::String& help,
            int diameterToUse, float sweepDegrees)
    : name (nameToUse.toUpperCase()), diameter (diameterToUse), attachment (*state.getParameter (paramId), slider)
{
    const float halfSweep = juce::degreesToRadians (sweepDegrees * 0.5f), twoPi = juce::MathConstants<float>::twoPi;
    slider.setRotaryParameters (twoPi - halfSweep, twoPi + halfSweep, true);
    slider.setVelocityModeParameters (0.15, 1, 0.0, true, juce::ModifierKeys::shiftModifier);   // Shift: fine
    slider.onValueChange = [this] { repaint(); };
    addAndMakeVisible (slider);

    editor.setJustification (juce::Justification::centred);
    editor.setFont (Theme::valueFont (12.0f));
    editor.setIndents (0, 0);
    editor.setColour (juce::TextEditor::backgroundColourId, Theme::field);
    editor.setColour (juce::TextEditor::textColourId, Theme::pale);
    editor.setColour (juce::TextEditor::highlightColourId, Theme::accent.withAlpha (0.5f));
    editor.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    editor.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    editor.onReturnKey = [this] { finishEditing (true); };
    editor.onEscapeKey = [this] { finishEditing (false); };
    editor.onFocusLost = [this] { finishEditing (true); };
    addChildComponent (editor);

    setHelp (*this, name, help);
}

void Knob::resized()
{
    auto area = getLocalBounds();
    area.removeFromTop (kLabelHeight + kGap - 1);
    slider.setBounds (area.removeFromTop (diameter + 2).withSizeKeepingCentre (diameter + 2, diameter + 2));
    editor.setBounds (getLocalBounds().removeFromBottom (kLabelHeight).withSizeKeepingCentre (64, kLabelHeight));
}

void Knob::paint (juce::Graphics& g)
{
    auto area = getLocalBounds();

    g.setColour (Theme::label);
    g.setFont (Theme::labelFont());
    g.drawText (name, area.removeFromTop (kLabelHeight), juce::Justification::centred, false);

    g.setFont (Theme::valueFont (12.0f));
    g.drawText (slider.getTextFromValue (slider.getValue()), area.removeFromBottom (kLabelHeight), juce::Justification::centred, false);
}

void Knob::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.y < getHeight() - kLabelHeight - 2)
        return;

    editor.setText (slider.getTextFromValue (slider.getValue()), false);
    editor.setVisible (true);
    editor.selectAll();
    editor.grabKeyboardFocus();
}

void Knob::finishEditing (bool apply)
{
    if (! editor.isVisible())
        return;

    editor.setVisible (false);
    const auto text = editor.getText().trim();

    // Only text that names a value: anything else is ignored, never turned into zero.
    if (apply && text.containsAnyOf ("0123456789"))
        slider.setValue (slider.getValueFromText (text), juce::sendNotificationSync);
    repaint();
}

//==============================================================================
OptionRow::OptionRow (juce::RangedAudioParameter& param, const juce::String& labelToUse, juce::StringArray optionsToUse,
                      std::vector<float> valuesToUse, const juce::String& help)
    : label (labelToUse.toUpperCase()), options (std::move (optionsToUse)), values (std::move (valuesToUse)),
      attachment (param, [this] (float v)
      {
          for (int i = 0; i < (int) values.size(); ++i)
              if (std::abs (values[(size_t) i] - v) < 0.5f)
                  selected = i;
          repaint();
      })
{
    jassert (options.size() == (int) values.size());
    labelColumn = labelTextWidth() + kLabelGap;
    attachment.sendInitialUpdate();
    setHelp (*this, label, help);
}

int OptionRow::labelTextWidth() const
{
    return Theme::widthOf (Theme::labelFont(), label);
}

std::vector<juce::Range<int>> OptionRow::optionRanges() const
{
    std::vector<juce::Range<int>> ranges;
    const auto font = Theme::labelFont();
    int x = labelColumn;

    for (const auto& option : options)
    {
        const int width = Theme::widthOf (font, option.toUpperCase());
        ranges.push_back ({ x, x + width });
        x += width + kOptionGap;
    }

    return ranges;
}

int OptionRow::optionAt (int x) const
{
    const auto ranges = optionRanges();
    for (int i = 0; i < (int) ranges.size(); ++i)
        if (ranges[(size_t) i].expanded (kOptionGap / 2).contains (x))
            return i;
    return -1;
}

void OptionRow::paint (juce::Graphics& g)
{
    g.setFont (Theme::labelFont());
    g.setColour (Theme::label);
    g.drawText (label, 0, 0, labelColumn, getHeight(), juce::Justification::centredLeft, false);

    const auto ranges = optionRanges();
    for (int i = 0; i < options.size(); ++i)
    {
        const auto range = ranges[(size_t) i];
        g.setColour (i == selected ? Theme::pale : (i == hovered ? Theme::text : Theme::label.withAlpha (0.7f)));
        g.drawText (options[i].toUpperCase(), range.getStart(), 0, range.getLength() + 4, getHeight(),
                    juce::Justification::centredLeft, false);
    }
}

void OptionRow::mouseDown (const juce::MouseEvent& e)
{
    if (const int option = optionAt (e.x); option >= 0)
        attachment.setValueAsCompleteGesture (values[(size_t) option]);
}

void OptionRow::mouseMove (const juce::MouseEvent& e)   { hovered = optionAt (e.x); repaint(); }
void OptionRow::mouseExit (const juce::MouseEvent&)     { hovered = -1; repaint(); }

//==============================================================================
SpectrumDisplay::SpectrumDisplay (juce::RangedAudioParameter& freqParam)
    : freq (freqParam),
      attachment (freqParam, [this] (float v) { freqHz = v; repaint(); })
{
    smoothedDb.fill (-120.0f);
    attachment.sendInitialUpdate();
    setHelp (*this, "Display", "The spectrum of the output (after Gain, so post-processing), 20 Hz to 2 kHz on a log axis, 0 dB at full scale "
                               "with a line every 12 dB down to -72. The dashed line is Freq: what lies to its left is rebuilt as "
                               "harmonics to its right. Drag the line to set Freq; double-click it to reset.");
}

void SpectrumDisplay::push (const float* samples, int numSamples) noexcept
{
    for (int i = 0; i < numSamples; ++i)
    {
        ring[(size_t) writeIndex] = samples[i];
        writeIndex = (writeIndex + 1) % kFftSize;
    }

    dirty = dirty || numSamples > 0;
}

void SpectrumDisplay::analyse()
{
    if (! dirty)
        return;
    dirty = false;

    for (int i = 0; i < kFftSize; ++i)
        fftData[(size_t) i] = ring[(size_t) ((writeIndex + i) % kFftSize)];
    std::fill (fftData.begin() + kFftSize, fftData.end(), 0.0f);

    window.multiplyWithWindowingTable (fftData.data(), (size_t) kFftSize);
    fft.performFrequencyOnlyForwardTransform (fftData.data(), true);

    // A full-scale sine reads 0 dB: the Hann window's coherent gain is 1/2 and the sine's energy
    // is split between the positive and negative bins.
    const float norm = 4.0f / (float) kFftSize;
    for (size_t k = 0; k < smoothedDb.size(); ++k)
    {
        const float db = toDb (fftData[k] * norm);
        auto& s = smoothedDb[k];
        s = db > s ? s + (db - s) * 0.5f : juce::jmax (db, s - 0.8f);   // quick up, about 24 dB/s down
    }

    repaint();
}

juce::Rectangle<float> SpectrumDisplay::plotArea() const
{
    return getLocalBounds().toFloat();
}

float SpectrumDisplay::xForHz (float hz) const
{
    const auto area = plotArea();
    return area.getX() + area.getWidth() * std::log (hz / kPlotMinHz) / std::log (kPlotMaxHz / kPlotMinHz);
}

float SpectrumDisplay::hzForX (float x) const
{
    const auto area = plotArea();
    return kPlotMinHz * std::pow (kPlotMaxHz / kPlotMinHz, (x - area.getX()) / area.getWidth());
}

float SpectrumDisplay::yForDb (float db) const
{
    const auto area = plotArea();
    return juce::jmap (juce::jlimit (kPlotFloorDb, 0.0f, db), kPlotFloorDb, 0.0f, area.getBottom(), area.getY());
}

float SpectrumDisplay::dbAtHz (float hz) const
{
    const float bin = hz * (float) kFftSize / (float) sampleRate;
    const int lower = juce::jlimit (0, (int) smoothedDb.size() - 2, (int) bin);
    const float t = juce::jlimit (0.0f, 1.0f, bin - (float) lower);
    return smoothedDb[(size_t) lower] + (smoothedDb[(size_t) lower + 1] - smoothedDb[(size_t) lower]) * t;
}

bool SpectrumDisplay::isOverFreqLine (juce::Point<float> p) const
{
    return std::abs (p.x - xForHz (freqHz)) < 7.0f;
}

void SpectrumDisplay::paint (juce::Graphics& g)
{
    const auto area = plotArea();
    g.fillAll (Theme::panel);

    // The output's spectrum, one point per pixel column.
    juce::Path spectrum;
    spectrum.startNewSubPath (area.getX(), area.getBottom());
    for (float x = area.getX(); x <= area.getRight(); x += 1.0f)
        spectrum.lineTo (x, yForDb (dbAtHz (hzForX (x))));
    spectrum.lineTo (area.getRight(), area.getBottom());
    spectrum.closeSubPath();

    g.setColour (Theme::pale.withAlpha (0.28f));
    g.fillPath (spectrum);

    // Below Freq is what becomes harmonics.
    const float lineX = xForHz (freqHz);
    const bool hot = dragging || (isMouseOver() && isOverFreqLine (getMouseXYRelative().toFloat()));
    g.setColour (Theme::accent.withAlpha (hot ? 0.14f : 0.09f));
    g.fillRect (area.withRight (lineX));

    g.setFont (Theme::valueFont (9.0f));
    for (float db = -12.0f; db > kPlotFloorDb; db -= 12.0f)
    {
        const float y = yForDb (db);
        g.setColour (Theme::label.withAlpha (0.14f));
        g.fillRect (area.getX(), y, area.getWidth(), 1.0f);
        g.setColour (Theme::label);
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (area.getRight() - 40.0f, y - 12.0f, 32.0f, 11.0f), juce::Justification::right);
    }

    const std::pair<float, const char*> marks[] { { 50.0f, "50" }, { 100.0f, "100" }, { 200.0f, "200" }, { 500.0f, "500" }, { 1000.0f, "1k" } };
    for (const auto& [hz, text] : marks)
    {
        const float x = xForHz (hz);
        g.setColour (Theme::label.withAlpha (0.14f));
        g.fillRect (x, area.getY(), 1.0f, area.getHeight());
        g.setColour (Theme::label);
        g.drawText (text, juce::Rectangle<float> (x + 4.0f, area.getBottom() - 14.0f, 30.0f, 11.0f), juce::Justification::left);
    }

    // The crossover: a quiet dashed line with an accent handle at the top.
    juce::Path line, dashed;
    line.startNewSubPath (lineX, area.getY());
    line.lineTo (lineX, area.getBottom());
    const float dashes[] { 3.0f, 5.0f };
    juce::PathStrokeType (1.0f).createDashedStroke (dashed, line, dashes, 2);
    g.setColour (Theme::text.withAlpha (hot ? 0.9f : 0.45f));
    g.fillPath (dashed);

    const float handleY = area.getY() + 12.0f;
    g.setColour (Theme::accent);
    g.fillRect (juce::Rectangle<float> (hot ? 9.0f : 7.0f, hot ? 9.0f : 7.0f).withCentre ({ lineX, handleY }));
    g.setColour (hot ? Theme::text : Theme::label);
    g.setFont (Theme::labelFont (9.5f));
    g.drawText ("FREQ", juce::Rectangle<float> (lineX + 9.0f, handleY - 6.0f, 40.0f, 12.0f), juce::Justification::left);
}

void SpectrumDisplay::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (isOverFreqLine (e.position) ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    repaint();
}

void SpectrumDisplay::mouseDown (const juce::MouseEvent& e)
{
    dragging = isOverFreqLine (e.position);
    if (dragging)
        attachment.beginGesture();
}

void SpectrumDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
        return;

    const auto& range = freq.getNormalisableRange();
    attachment.setValueAsPartOfGesture (range.snapToLegalValue (juce::jlimit (range.start, range.end, hzForX (e.position.x))));
}

void SpectrumDisplay::mouseUp (const juce::MouseEvent&)
{
    if (dragging)
        attachment.endGesture();
    dragging = false;
}

void SpectrumDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (isOverFreqLine (e.position))
        attachment.setValueAsCompleteGesture (freq.convertFrom0to1 (freq.getDefaultValue()));
}

//==============================================================================
Meter::Meter (const juce::String& labelToUse, bool clipLight, const juce::String& help)
    : label (labelToUse.toUpperCase()), hasClipLight (clipLight)
{
    labelColumn = labelTextWidth() + kLabelGap;
    setHelp (*this, label, help);
}

int Meter::labelTextWidth() const
{
    return Theme::widthOf (Theme::labelFont(), label);
}

void Meter::push (float peak, bool clipped)
{
    const float db = toDb (peak);

    levelDb = juce::jmax (db, levelDb - 0.7f);   // about 20 dB/s fall

    if (db >= tickDb)
    {
        tickDb = db;
        tickHold = 45;                           // 1.5 s before the tick falls
    }
    else if (tickHold > 0)
        --tickHold;
    else
        tickDb = juce::jmax (db, tickDb - 0.5f);

    heldDb = juce::jmax (heldDb, db);
    clipLit = clipLit || clipped;
    repaint();
}

void Meter::paint (juce::Graphics& g)
{
    auto area = getLocalBounds();

    g.setFont (Theme::labelFont());
    g.setColour (Theme::label);
    g.drawText (label, area.removeFromLeft (labelColumn), juce::Justification::centredLeft, false);

    auto number = area.removeFromRight (40);
    auto light = area.removeFromRight (14).withSizeKeepingCentre (6, 6);
    area.removeFromRight (6);

    const auto bar = area.toFloat().withSizeKeepingCentre ((float) area.getWidth(), 4.0f);
    const auto fraction = [] (float db) { return juce::jlimit (0.0f, 1.0f, (db - kFloorDb) / -kFloorDb); };

    g.setColour (Theme::label.withAlpha (0.25f));
    g.fillRect (bar);

    if (const float f = fraction (levelDb); f > 0.0f)
    {
        g.setColour (Theme::accent);
        g.fillRect (bar.withWidth (bar.getWidth() * f));
    }

    if (const float f = fraction (tickDb); f > 0.0f)
    {
        g.setColour (Theme::pale);
        g.fillRect (juce::Rectangle<float> (2.0f, bar.getHeight()).withPosition (bar.getX() + bar.getWidth() * f - 2.0f, bar.getY()));
    }

    if (hasClipLight)
    {
        g.setColour (clipLit ? Theme::clipping : Theme::label.withAlpha (0.25f));
        g.fillRect (light);
    }

    g.setFont (Theme::valueFont (11.0f));
    g.setColour (Theme::label);
    g.drawText (meterText (heldDb), number, juce::Justification::centredRight);
}

void Meter::mouseDown (const juce::MouseEvent&)
{
    heldDb = -100.0f;
    clipLit = false;
    repaint();
}

//==============================================================================
EditorView::EditorView (BassifierProcessor& p)
    : plugin (p),
      display (*p.apvts.getParameter (ParamID::freq)),
      freq (p.apvts, ParamID::freq, "Freq", "The crossover. Everything below it is taken out of the signal and rebuilt as harmonics above it; "
                                           "it is also the dashed line in the display, which can be dragged. Set it near the bass's fundamental, "
                                           "or at the cutoff of the speakers you are mixing for."),
      intensity (p.apvts, ParamID::intensity, "Intensity", "How much of the generated harmonics goes into the output. A few dB go a long way: "
                                                           "start below 0 dB for a natural result and raise it until the bass comes forward."),
      gain (p.apvts, ParamID::gain, "Gain", "Level of the output. The harmonics add energy, so this is where to bring it back down, "
                                           "or to match the input level for a fair comparison."),
      originalBass (*p.apvts.getParameter (ParamID::originalBass), "Original Bass", { "In", "Out" }, { 1.0f, 0.0f },
                    "Whether the part of the input below Freq is kept. Out leaves only the harmonics, which is what a small speaker "
                    "wants; In keeps the real low end underneath them."),
      bassMeter ("Original Bass", false, "Peak level of the input below Freq: the part being replaced. The number is the "
                                                       "highest peak so far; click to reset it."),
      harmonicsMeter ("Harmonics", false, "Peak level of the harmonics being added, after Intensity. The number is the "
                                                        "highest peak so far; click to reset it."),
      outputMeter ("Output", true, "Peak level of the output after Gain. The light comes on when it goes over full scale "
                                                 "and stays on; click to reset it and the held number.")
{
    for (auto* c : std::initializer_list<juce::Component*> { &display, &freq, &intensity, &gain, &originalBass,
                                                             &bassMeter, &harmonicsMeter, &outputMeter })
        addAndMakeVisible (c);

    // The option row and the meter names share one label column.
    const int column = std::max ({ originalBass.labelTextWidth(), bassMeter.labelTextWidth(), harmonicsMeter.labelTextWidth(),
                                   outputMeter.labelTextWidth() }) + kLabelGap;
    originalBass.setLabelColumn (column);
    for (auto* m : { &bassMeter, &harmonicsMeter, &outputMeter })
        m->setLabelColumn (column);

    showHelpFor (nullptr);
    setSize (kWidth, kHeight);
}

void EditorView::showHelpFor (juce::Component* component)
{
    juce::String title = kDefaultHelpTitle, text = kDefaultHelp;

    // The help belongs to the nearest ancestor that has any (a knob's slider has none itself).
    for (auto* c = component; c != nullptr && c != this; c = c->getParentComponent())
    {
        if (! c->getProperties().contains (helpId))
            continue;

        title = c->getProperties()[helpTitleId].toString();
        text = c->getProperties()[helpId].toString();
        break;
    }

    if (title != helpTitle || text != helpText)
    {
        helpTitle = title;
        helpText = text;
        repaint (helpArea);
    }
}

void EditorView::poll()
{
    if (const double rate = plugin.getSampleRate(); rate > 0.0)
        display.setSampleRate (rate);

    while (const int n = plugin.readAnalyser (pullBuffer.data(), (int) pullBuffer.size()))
        display.push (pullBuffer.data(), n);
    display.analyse();

    const auto peaks = plugin.takeMeterPeaks();
    bassMeter.push (peaks.originalBass, false);
    harmonicsMeter.push (peaks.harmonics, false);
    outputMeter.push (peaks.output, peaks.clipped);
}

void EditorView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);

    g.setColour (Theme::pale);
    g.setFont (Theme::titleFont (15.0f));
    g.drawSingleLineText ("BASSIFIER", 24, 33);

    // Help, bottom left: what the control under the mouse does.
    auto help = helpArea;
    g.setColour (Theme::pale);
    g.setFont (Theme::labelFont());
    g.drawText (helpTitle, help.removeFromTop (16), juce::Justification::topLeft, false);
    g.setColour (Theme::label);
    g.setFont (Theme::bodyFont());
    g.drawFittedText (helpText, help, juce::Justification::topLeft, 3, 1.0f);
}

void EditorView::resized()
{
    constexpr int margin = 24, header = 50, rightColumn = 248, rowGap = 8;

    auto area = getLocalBounds().reduced (margin, 0);
    area.removeFromTop (header);

    // Top: the display across the full width.
    display.setBounds (area.removeFromTop (160));

    // Bottom: what the control under the mouse does.
    area.removeFromBottom (10);
    helpArea = area.removeFromBottom (54);

    // Middle: the three knobs on the left; the option row and the meters on the right.
    auto row = area.withSizeKeepingCentre (area.getWidth(), freq.preferredHeight());
    auto right = row.removeFromRight (rightColumn);

    for (auto* knob : { &freq, &intensity, &gain })
        knob->setBounds (row.removeFromLeft (Knob::kCellWidth));

    right = right.withSizeKeepingCentre (rightColumn, 4 * kRowHeight + rowGap);
    originalBass.setBounds (right.removeFromTop (kRowHeight));
    right.removeFromTop (rowGap);
    bassMeter.setBounds (right.removeFromTop (kRowHeight));
    harmonicsMeter.setBounds (right.removeFromTop (kRowHeight));
    outputMeter.setBounds (right.removeFromTop (kRowHeight));
}

//==============================================================================
BassifierEditor::BassifierEditor (BassifierProcessor& p)
    : AudioProcessorEditor (p), owner (p), view (p)
{
    setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (view);

    // Scales as a whole, 0.75x to 2x, keeping the design's aspect ratio.
    constexpr int w = EditorView::kWidth, h = EditorView::kHeight;
    setResizable (true, true);
    setResizeLimits (w * 3 / 4, h * 3 / 4, w * 2, h * 2);
    getConstrainer()->setFixedAspectRatio ((double) w / (double) h);

    const double scale = juce::jlimit (0.75, 2.0, (double) p.apvts.state.getProperty (editorScaleId, 1.0));
    setSize (juce::roundToInt (w * scale), juce::roundToInt (h * scale));
    startTimerHz (30);
}

BassifierEditor::~BassifierEditor()
{
    setLookAndFeel (nullptr);
}

void BassifierEditor::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);
}

void BassifierEditor::resized()
{
    const float scale = (float) getWidth() / (float) EditorView::kWidth;
    view.setTransform (juce::AffineTransform::scale (scale));
    view.setBounds (0, 0, EditorView::kWidth, EditorView::kHeight);

    // Remember a size the user chose (a window on screen), not the ones a host passes through
    // while opening or closing it.
    if (isShowing() && getPeer() != nullptr)
        owner.apvts.state.setProperty (editorScaleId, (double) scale, nullptr);
}

void BassifierEditor::timerCallback()
{
    view.poll();

    auto* under = juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse();
    view.showHelpFor (under != nullptr && view.isParentOf (under) ? under : nullptr);
}
