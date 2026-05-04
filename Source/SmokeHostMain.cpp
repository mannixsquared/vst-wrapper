#include <juce_audio_utils/juce_audio_utils.h>

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    if (argc < 2)
    {
        std::cerr << "Usage: IntelVSTWrapperSmokeHost /path/to/plugin.vst3-or-component\n";
        return 2;
    }

    const juce::File pluginFile { juce::String (argv[1]) };
    if (! pluginFile.exists())
    {
        std::cerr << "Plugin path does not exist: " << pluginFile.getFullPathName() << "\n";
        return 3;
    }

    juce::AudioPluginFormatManager manager;
    juce::addDefaultFormatsToManager (manager);

    juce::OwnedArray<juce::PluginDescription> descriptions;
    for (auto* format : manager.getFormats())
        format->findAllTypesForFile (descriptions, pluginFile.getFullPathName());

    if (descriptions.isEmpty())
    {
        std::cerr << "No plugin descriptions found in " << pluginFile.getFullPathName() << "\n";
        return 4;
    }

    juce::String error;
    auto instance = manager.createPluginInstance (*descriptions.getFirst(), 44100.0, 512, error);

    if (instance == nullptr)
    {
        std::cerr << "Could not instantiate plugin: " << error << "\n";
        return 5;
    }

    instance->setRateAndBufferSizeDetails (44100.0, 512);
    instance->prepareToPlay (44100.0, 512);

    juce::AudioBuffer<float> buffer (2, 512);
    buffer.clear();

    for (auto i = 0; i < buffer.getNumSamples(); ++i)
        buffer.setSample (0, i, std::sin (static_cast<float> (i) * 0.01f) * 0.05f);

    juce::MidiBuffer midi;
    instance->processBlock (buffer, midi);
    instance->releaseResources();

    std::cout << "Loaded: " << instance->getName() << "\n";
    std::cout << "Descriptions: " << descriptions.size() << "\n";
    std::cout << "Inputs: " << instance->getTotalNumInputChannels()
              << ", outputs: " << instance->getTotalNumOutputChannels() << "\n";
    std::cout << "Smoke process block completed\n";

    return 0;
}
