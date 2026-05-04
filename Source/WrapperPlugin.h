#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <array>

class IntelVSTWrapperAudioProcessor final : public juce::AudioProcessor,
                                            private juce::AsyncUpdater
{
public:
    IntelVSTWrapperAudioProcessor();
    ~IntelVSTWrapperAudioProcessor() override;

    const juce::String getName() const override { return "Intel VST Wrapper"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;
    void handleAsyncUpdate() override;

    juce::String getHostedPluginPath() const;
    void setHostedPluginPath (const juce::String& path);
    juce::String getBridgeStatus() const;
    void openHostedPluginEditor();

private:
    class MirroredParameter;

    static constexpr int maxMirroredParameters = 128;

    static BusesProperties makeBuses();

    bool ensureHelper();
    bool loadHostedPlugin();
    bool sendPrepare();
    bool processRemotely (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi);
    bool receiveAckOrStatus (const char* action);
    bool refreshHostedParameters();
    bool fetchHostedState (juce::MemoryBlock& state);
    bool sendHostedState (const juce::MemoryBlock& state);
    juce::File findBundledHelper() const;
    juce::String configuredPluginPath() const;
    void setBridgeStatus (const juce::String& status);
    void stopHelper();
    bool loadSelectedHostedPlugin();
    void clearMirroredParameters();

    juce::CriticalSection bridgeLock;
    std::unique_ptr<juce::StreamingSocket> listener;
    std::unique_ptr<juce::StreamingSocket> socket;
    juce::ChildProcess helperProcess;
    std::array<MirroredParameter*, maxMirroredParameters> mirroredParameters {};
    std::array<float, maxMirroredParameters> lastSentParameterValues {};
    juce::String hostedPluginPath;
    juce::MemoryBlock hostedPluginState;
    juce::MemoryBlock pendingHostedPluginState;
    juce::String bridgeStatus = "No hosted plugin selected";
    double currentSampleRate = 44100.0;
    int currentBlockSize = 512;
    bool pluginLoaded = false;
    bool helperPrepared = false;
    bool pendingStateLoad = false;
    bool pendingHostedPluginStateValid = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IntelVSTWrapperAudioProcessor)
};
