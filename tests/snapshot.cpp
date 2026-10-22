// Renders the plug-in editor to a PNG without a host or a screen, after running a synthetic
// bass line through it so the analyser and the meters have content.
//
//   bassifier_snapshot out.png

#include "../Source/PluginEditor.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

int main (int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI gui;
    const auto output = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "editor.png");

    constexpr double rate = 48000.0;
    constexpr int blockSize = 512, seconds = 4;

    std::unique_ptr<juce::AudioProcessor> processor (createPluginFilter());
    processor->setPlayConfigDetails (2, 2, rate, blockSize);
    processor->prepareToPlay (rate, blockSize);

    // Two held bass notes at about -12 dBFS, and a kick-like 60 Hz burst every half second.
    juce::AudioBuffer<float> audio (2, (int) rate * seconds);
    const float twoPi = juce::MathConstants<float>::twoPi;

    for (int n = 0; n < audio.getNumSamples(); ++n)
    {
        const float t = (float) n / (float) rate;
        const float sinceKick = std::fmod (t, 0.5f);
        const float kick = std::sin (twoPi * 60.0f * sinceKick) * std::exp (-sinceKick / 0.12f);
        const float v = 0.18f * std::sin (twoPi * 55.0f * t) + 0.12f * std::sin (twoPi * 82.0f * t) + 0.25f * kick;
        audio.setSample (0, n, v);
        audio.setSample (1, n, v);
    }

    juce::MidiBuffer midi;
    auto& bassifier = dynamic_cast<BassifierProcessor&> (*processor);
    BassifierEditor editor (bassifier);

    for (int pos = 0; pos + blockSize <= audio.getNumSamples(); pos += blockSize)
    {
        juce::AudioBuffer<float> block (audio.getArrayOfWritePointers(), 2, pos, blockSize);
        processor->processBlock (block, midi);
        editor.pollForSnapshot();
    }

    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);   // let the editor's timer catch up
    const auto image = editor.createComponentSnapshot (editor.getLocalBounds(), true, 2.0f);
    output.deleteFile();
    juce::FileOutputStream out (output);
    return juce::PNGImageFormat().writeImageToStream (image, out) ? 0 : 1;
}
