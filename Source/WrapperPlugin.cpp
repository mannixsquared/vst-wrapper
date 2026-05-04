#include "WrapperPlugin.h"
#include "BridgeProtocol.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <cmath>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace
{
    static constexpr int wrapperStateMagic = 0x49565753; // IVWS
    static constexpr int wrapperStateVersion = 1;
    std::atomic<juce::uint32> sharedAudioNameCounter { 0 };

    size_t sharedAudioBytesFor (int channels, int samples)
    {
        return sizeof (float) * static_cast<size_t> (channels) * static_cast<size_t> (samples) * 2u;
    }

    juce::String envVar (const char* name)
    {
        return juce::SystemStats::getEnvironmentVariable (name, {});
    }

    void wrapperLog (const juce::String& message)
    {
        const auto logFile = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                .getChildFile ("Logs/IntelVSTWrapper.log");
        logFile.getParentDirectory().createDirectory();
        logFile.appendText (juce::Time::getCurrentTime().toISO8601 (true) + " " + message + "\n");
    }

    class WrapperEditor final : public juce::AudioProcessorEditor,
                                private juce::Button::Listener,
                                private juce::Timer
    {
    public:
        explicit WrapperEditor (IntelVSTWrapperAudioProcessor& processorToUse)
            : juce::AudioProcessorEditor (processorToUse),
              wrappedProcessor (processorToUse)
        {
            title.setText ("Intel VST Wrapper", juce::dontSendNotification);
            title.setFont (juce::FontOptions (18.0f, juce::Font::bold));
            addAndMakeVisible (title);

            pathLabel.setText (displayPath(), juce::dontSendNotification);
            pathLabel.setJustificationType (juce::Justification::centredLeft);
            addAndMakeVisible (pathLabel);

            statusLabel.setText (wrappedProcessor.getBridgeStatus(), juce::dontSendNotification);
            statusLabel.setJustificationType (juce::Justification::centredLeft);
            addAndMakeVisible (statusLabel);

            chooseButton.setButtonText ("Choose...");
            chooseButton.addListener (this);
            addAndMakeVisible (chooseButton);

            editorButton.setButtonText ("Open UI");
            editorButton.addListener (this);
            addAndMakeVisible (editorButton);

            startTimerHz (4);
            setSize (650, 150);
        }

        ~WrapperEditor() override
        {
            chooseButton.removeListener (this);
            editorButton.removeListener (this);
        }

        void resized() override
        {
            auto bounds = getLocalBounds().reduced (16);
            title.setBounds (bounds.removeFromTop (28));

            auto row = bounds.removeFromTop (40);
            editorButton.setBounds (row.removeFromRight (90).reduced (4, 3));
            chooseButton.setBounds (row.removeFromRight (110).reduced (4, 3));
            pathLabel.setBounds (row.reduced (0, 3));

            statusLabel.setBounds (bounds.removeFromTop (36).reduced (0, 4));
        }

    private:
        juce::String displayPath() const
        {
            const auto path = wrappedProcessor.getHostedPluginPath();
            return path.isNotEmpty() ? path : "No Intel VST selected";
        }

        void buttonClicked (juce::Button* button) override
        {
            if (button == &editorButton)
            {
                wrappedProcessor.openHostedPluginEditor();
                return;
            }

            chooser = std::make_unique<juce::FileChooser> ("Choose an Intel VST, VST3, or Audio Unit",
                                                           juce::File ("/Library/Audio/Plug-Ins"),
                                                           "*.vst;*.vst3;*.component");

            constexpr auto flags = juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::canSelectDirectories;

            const juce::Component::SafePointer<WrapperEditor> safeThis (this);

            chooser->launchAsync (flags, [safeThis] (const juce::FileChooser& fc)
            {
                if (safeThis == nullptr)
                    return;

                const auto file = fc.getResult();
                if (file.exists())
                {
                    safeThis->wrappedProcessor.setHostedPluginPath (file.getFullPathName());
                    safeThis->pathLabel.setText (safeThis->displayPath(), juce::dontSendNotification);
                }

                safeThis->chooser.reset();
            });
        }

        void timerCallback() override
        {
            statusLabel.setText (wrappedProcessor.getBridgeStatus(), juce::dontSendNotification);
        }

        IntelVSTWrapperAudioProcessor& wrappedProcessor;
        juce::Label title;
        juce::Label pathLabel;
        juce::Label statusLabel;
        juce::TextButton chooseButton;
        juce::TextButton editorButton;
        std::unique_ptr<juce::FileChooser> chooser;
    };
}

class IntelVSTWrapperAudioProcessor::MirroredParameter final : public juce::AudioProcessorParameter
{
public:
    explicit MirroredParameter (int indexToUse)
        : index (indexToUse)
    {
        reset();
    }

    float getValue() const override { return value.load(); }

    void setValue (float newValue) override
    {
        value.store (juce::jlimit (0.0f, 1.0f, newValue));
    }

    float getDefaultValue() const override { return defaultValue.load(); }

    juce::String getName (int maximumStringLength) const override
    {
        const juce::ScopedLock lock (metadataLock);
        auto text = active.load() ? name : "Hosted Param " + juce::String (index + 1);
        return maximumStringLength > 0 ? text.substring (0, maximumStringLength) : text;
    }

    juce::String getLabel() const override
    {
        const juce::ScopedLock lock (metadataLock);
        return label;
    }

    int getNumSteps() const override { return steps.load(); }
    bool isDiscrete() const override { return discrete.load(); }
    bool isBoolean() const override { return boolean.load(); }
    bool isAutomatable() const override { return true; }

    juce::String getText (float normalisedValue, int maximumStringLength) const override
    {
        auto text = juce::String (normalisedValue, 3);
        return maximumStringLength > 0 ? text.substring (0, maximumStringLength) : text;
    }

    float getValueForText (const juce::String& text) const override
    {
        return juce::jlimit (0.0f, 1.0f, text.getFloatValue());
    }

    void reset()
    {
        const juce::ScopedLock lock (metadataLock);
        active.store (false);
        name = "Hosted Param " + juce::String (index + 1);
        label.clear();
        defaultValue.store (0.0f);
        value.store (0.0f);
        steps.store (juce::AudioProcessor::getDefaultNumParameterSteps());
        discrete.store (false);
        boolean.store (false);
    }

    void updateMetadata (const juce::String& newName,
                         const juce::String& newLabel,
                         float newDefaultValue,
                         float newValue,
                         int newSteps,
                         bool newDiscrete,
                         bool newBoolean)
    {
        {
            const juce::ScopedLock lock (metadataLock);
            active.store (true);
            name = newName.isNotEmpty() ? newName : "Parameter " + juce::String (index + 1);
            label = newLabel;
            defaultValue.store (juce::jlimit (0.0f, 1.0f, newDefaultValue));
            steps.store (juce::jmax (2, newSteps));
            discrete.store (newDiscrete);
            boolean.store (newBoolean);
        }

        setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, newValue));
    }

    bool isActive() const { return active.load(); }

private:
    int index = 0;
    mutable juce::CriticalSection metadataLock;
    std::atomic<float> value { 0.0f };
    std::atomic<float> defaultValue { 0.0f };
    std::atomic<int> steps { juce::AudioProcessor::getDefaultNumParameterSteps() };
    std::atomic<bool> discrete { false };
    std::atomic<bool> boolean { false };
    std::atomic<bool> active { false };
    juce::String name;
    juce::String label;
};

IntelVSTWrapperAudioProcessor::BusesProperties IntelVSTWrapperAudioProcessor::makeBuses()
{
    return BusesProperties()
        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true);
}

IntelVSTWrapperAudioProcessor::IntelVSTWrapperAudioProcessor()
    : AudioProcessor (makeBuses())
{
    lastSentParameterValues.fill (-1.0f);

    for (auto i = 0; i < maxMirroredParameters; ++i)
    {
        auto* parameter = new MirroredParameter (i);
        mirroredParameters[static_cast<size_t> (i)] = parameter;
        addParameter (parameter);
    }
}

IntelVSTWrapperAudioProcessor::~IntelVSTWrapperAudioProcessor()
{
    stopHelper();
}

void IntelVSTWrapperAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const juce::ScopedLock lock (bridgeLock);

    currentSampleRate = sampleRate;
    currentBlockSize = samplesPerBlock;

    if (pluginLoaded && ! helperPrepared && socket != nullptr && socket->isConnected())
        sendPrepare();
}

void IntelVSTWrapperAudioProcessor::releaseResources()
{
    const juce::ScopedLock lock (bridgeLock);
    helperPrepared = false;
}

bool IntelVSTWrapperAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto mainOut = layouts.getMainOutputChannelSet();
    return mainOut == juce::AudioChannelSet::mono()
        || mainOut == juce::AudioChannelSet::stereo();
}

void IntelVSTWrapperAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const juce::ScopedLock lock (bridgeLock);

    if (! helperPrepared || ! processRemotely (buffer, midi))
    {
        for (auto channel = getTotalNumInputChannels(); channel < getTotalNumOutputChannels(); ++channel)
            buffer.clear (channel, 0, buffer.getNumSamples());
    }
}

juce::AudioProcessorEditor* IntelVSTWrapperAudioProcessor::createEditor()
{
    return new WrapperEditor (*this);
}

void IntelVSTWrapperAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::MemoryBlock stateToStore;
    juce::String pathToStore;

    {
        const juce::ScopedLock lock (bridgeLock);

        if (pluginLoaded && socket != nullptr && socket->isConnected())
            fetchHostedState (hostedPluginState);

        stateToStore = hostedPluginState;
        pathToStore = hostedPluginPath;
    }

    juce::MemoryOutputStream out (destData, true);
    out.writeInt (wrapperStateMagic);
    out.writeInt (wrapperStateVersion);
    intelvst::bridge::writeString (out, pathToStore);
    out.writeInt (static_cast<int> (stateToStore.getSize()));

    if (! stateToStore.isEmpty())
        out.write (stateToStore.getData(), stateToStore.getSize());
}

void IntelVSTWrapperAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::MemoryInputStream in (data, static_cast<size_t> (sizeInBytes), false);
    juce::String restoredPath;
    juce::MemoryBlock restoredHostedState;

    const auto maybeMagic = in.readInt();

    if (maybeMagic == wrapperStateMagic)
    {
        const auto version = in.readInt();
        restoredPath = intelvst::bridge::readString (in);

        if (version >= 1)
        {
            const auto stateBytes = juce::jmax (0, in.readInt());
            restoredHostedState.setSize (static_cast<size_t> (stateBytes), false);

            if (stateBytes > 0)
                in.read (restoredHostedState.getData(), stateBytes);
        }
    }
    else
    {
        in.setPosition (0);
        restoredPath = intelvst::bridge::readString (in);
    }

    const juce::ScopedLock lock (bridgeLock);

    if (hostedPluginPath != restoredPath)
    {
        stopHelper();
        pluginLoaded = false;
        helperPrepared = false;
    }

    hostedPluginPath = restoredPath;
    hostedPluginState = restoredHostedState;
    pendingHostedPluginState = restoredHostedState;
    pendingHostedPluginStateValid = ! restoredHostedState.isEmpty();
    pendingStateLoad = hostedPluginPath.isNotEmpty();
    setBridgeStatus (pendingStateLoad ? "Restored " + juce::File (hostedPluginPath).getFileName()
                                      : "No hosted plugin selected");

    if (pendingStateLoad)
        triggerAsyncUpdate();
}

void IntelVSTWrapperAudioProcessor::handleAsyncUpdate()
{
    const juce::ScopedLock lock (bridgeLock);

    if (! pendingStateLoad)
        return;

    pendingStateLoad = false;
    loadSelectedHostedPlugin();
}

juce::String IntelVSTWrapperAudioProcessor::getHostedPluginPath() const
{
    return hostedPluginPath;
}

void IntelVSTWrapperAudioProcessor::setHostedPluginPath (const juce::String& path)
{
    const juce::ScopedLock lock (bridgeLock);

    if (hostedPluginPath == path)
        return;

    hostedPluginPath = path;
    hostedPluginState.reset();
    pendingHostedPluginState.reset();
    pendingHostedPluginStateValid = false;
    clearMirroredParameters();
    setBridgeStatus ("Selected " + juce::File (path).getFileName());
    wrapperLog ("Selected hosted plugin: " + path);
    stopHelper();

    loadSelectedHostedPlugin();
}

void IntelVSTWrapperAudioProcessor::openHostedPluginEditor()
{
    const juce::ScopedLock lock (bridgeLock);

    if (! helperPrepared || socket == nullptr || ! socket->isConnected())
    {
        setBridgeStatus ("Hosted plugin is not loaded");
        return;
    }

    setBridgeStatus ("Opening hosted plugin UI");

    if (! intelvst::bridge::sendMessage (*socket, intelvst::bridge::MessageType::openEditor))
    {
        setBridgeStatus ("Could not ask bridge to open hosted UI");
        return;
    }

    if (receiveAckOrStatus ("open editor"))
        setBridgeStatus ("Hosted plugin UI opened");
}

juce::String IntelVSTWrapperAudioProcessor::getBridgeStatus() const
{
    const juce::ScopedLock lock (bridgeLock);
    return bridgeStatus;
}

bool IntelVSTWrapperAudioProcessor::ensureHelper()
{
    if (socket != nullptr && socket->isConnected())
        return true;

    stopHelper();

    auto helper = envVar ("INTEL_VST_WRAPPER_HELPER_PATH").isNotEmpty()
                    ? juce::File (envVar ("INTEL_VST_WRAPPER_HELPER_PATH"))
                    : findBundledHelper();

    if (! helper.existsAsFile())
    {
        setBridgeStatus ("Bridge helper not found");
        wrapperLog ("Bridge helper not found. Looked for: " + helper.getFullPathName());
        return false;
    }

    listener = std::make_unique<juce::StreamingSocket>();
    if (! listener->createListener (0, "127.0.0.1"))
    {
        setBridgeStatus ("Could not create bridge listener");
        wrapperLog ("Could not create listener");
        return false;
    }

    const auto port = listener->getBoundPort();
    juce::StringArray args;

   #if JUCE_MAC
    const auto maybeAppBundle = helper.getParentDirectory()
                                      .getParentDirectory()
                                      .getParentDirectory();
    const auto launchViaBundle = maybeAppBundle.hasFileExtension ("app")
                              && maybeAppBundle.isDirectory()
                              && envVar ("INTEL_VST_WRAPPER_HELPER_PATH").isEmpty();

    if (launchViaBundle)
    {
        args.add ("/usr/bin/open");
        args.add ("-n");
        args.add (maybeAppBundle.getFullPathName());
        args.add ("--args");
        args.add (juce::String (port));
    }
    else
   #endif
    {
        args.add (helper.getFullPathName());
        args.add (juce::String (port));
    }

    if (! helperProcess.start (args))
    {
        setBridgeStatus ("Could not launch x86_64 bridge helper");
        wrapperLog ("Could not launch helper: " + helper.getFullPathName());
        return false;
    }

    setBridgeStatus ("Bridge helper launched");
    wrapperLog ("Launched helper: " + args.joinIntoString (" ") + " on port " + juce::String (port));

    socket.reset (listener->waitForNextConnection());
    listener.reset();

    if (socket == nullptr)
    {
        setBridgeStatus ("Bridge helper did not connect back");
        wrapperLog ("Helper did not connect back");
        return false;
    }

    intelvst::bridge::MessageType type;
    juce::MemoryBlock payload;
    const auto ok = intelvst::bridge::receiveMessage (*socket, type, payload)
                 && type == intelvst::bridge::MessageType::hello;
    setBridgeStatus (ok ? "Bridge helper connected" : "Bridge helper handshake failed");
    wrapperLog (ok ? "Helper connected" : "Helper handshake failed");
    return ok;
}

bool IntelVSTWrapperAudioProcessor::loadHostedPlugin()
{
    const auto path = configuredPluginPath();
    if (path.isEmpty())
    {
        setBridgeStatus ("No hosted plugin selected");
        return false;
    }

    if (pluginLoaded && hostedPluginPath == path)
        return true;

    juce::MemoryBlock payload;
    juce::MemoryOutputStream out (payload, false);
    intelvst::bridge::writeString (out, path);

    setBridgeStatus ("Loading " + juce::File (path).getFileName());
    wrapperLog ("Loading hosted plugin: " + path);

    if (! intelvst::bridge::sendMessage (*socket, intelvst::bridge::MessageType::loadPlugin, payload))
    {
        setBridgeStatus ("Could not send plugin path to bridge");
        return false;
    }

    if (! receiveAckOrStatus ("load plugin"))
        return false;

    hostedPluginPath = path;
    pluginLoaded = true;
    helperPrepared = false;

    refreshHostedParameters();

    if (pendingHostedPluginStateValid)
    {
        if (sendHostedState (pendingHostedPluginState))
        {
            hostedPluginState = pendingHostedPluginState;
            pendingHostedPluginState.reset();
            pendingHostedPluginStateValid = false;
            refreshHostedParameters();
        }
    }

    return true;
}

bool IntelVSTWrapperAudioProcessor::loadSelectedHostedPlugin()
{
    if (hostedPluginPath.isEmpty())
    {
        setBridgeStatus ("No hosted plugin selected");
        return false;
    }

    if (ensureHelper() && loadHostedPlugin())
        return sendPrepare();

    return false;
}

bool IntelVSTWrapperAudioProcessor::sendPrepare()
{
    const auto channels = juce::jmax (getTotalNumInputChannels(), getTotalNumOutputChannels());
    if (! ensureSharedAudioMemory (channels, currentBlockSize))
        return false;

    juce::MemoryBlock payload;
    juce::MemoryOutputStream out (payload, false);
    out.writeDouble (currentSampleRate);
    out.writeInt (currentBlockSize);
    out.writeInt (getTotalNumInputChannels());
    out.writeInt (getTotalNumOutputChannels());
    intelvst::bridge::writeString (out, sharedAudioName);
    out.writeInt (sharedAudioChannels);
    out.writeInt (sharedAudioSamples);
    out.writeInt64 (static_cast<juce::int64> (sharedAudioByteCount));

    if (! intelvst::bridge::sendMessage (*socket, intelvst::bridge::MessageType::prepare, payload))
    {
        setBridgeStatus ("Could not send prepare to bridge");
        return false;
    }

    helperPrepared = receiveAckOrStatus ("prepare");

    if (helperPrepared)
        setBridgeStatus ("Ready: " + juce::File (hostedPluginPath).getFileName());

    return helperPrepared;
}

bool IntelVSTWrapperAudioProcessor::processRemotely (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    const auto channels = buffer.getNumChannels();
    const auto samples = buffer.getNumSamples();

    if (sharedAudioData == nullptr
        || channels > sharedAudioChannels
        || samples > sharedAudioSamples)
    {
        setBridgeStatus ("Shared audio buffer is not prepared");
        return false;
    }

    auto* const sharedInput = static_cast<float*> (sharedAudioData);
    auto* const sharedOutput = sharedInput + static_cast<size_t> (sharedAudioChannels) * static_cast<size_t> (sharedAudioSamples);

    for (auto channel = 0; channel < channels; ++channel)
        std::memcpy (sharedInput + static_cast<size_t> (channel) * static_cast<size_t> (sharedAudioSamples),
                     buffer.getReadPointer (channel),
                     sizeof (float) * static_cast<size_t> (samples));

    juce::MemoryBlock payload;
    juce::MemoryOutputStream out (payload, false);
    out.writeInt (channels);
    out.writeInt (samples);

    auto parameterUpdateCountPosition = out.getPosition();
    out.writeInt (0);

    auto parameterUpdateCount = 0;
    for (auto i = 0; i < maxMirroredParameters; ++i)
    {
        auto* parameter = mirroredParameters[static_cast<size_t> (i)];
        if (parameter == nullptr || ! parameter->isActive())
            continue;

        const auto value = parameter->getValue();
        if (std::abs (value - lastSentParameterValues[static_cast<size_t> (i)]) <= 0.000001f)
            continue;

        out.writeInt (i);
        out.writeFloat (value);
        lastSentParameterValues[static_cast<size_t> (i)] = value;
        ++parameterUpdateCount;
    }

    const auto afterParameterUpdates = out.getPosition();
    out.setPosition (parameterUpdateCountPosition);
    out.writeInt (parameterUpdateCount);
    out.setPosition (afterParameterUpdates);

    out.writeInt (midi.getNumEvents());

    for (const auto metadata : midi)
    {
        out.writeInt (metadata.samplePosition);
        out.writeInt (metadata.numBytes);
        out.write (metadata.data, static_cast<size_t> (metadata.numBytes));
    }

    if (! intelvst::bridge::sendMessage (*socket, intelvst::bridge::MessageType::process, payload))
    {
        setBridgeStatus ("Could not send audio block to bridge");
        return false;
    }

    intelvst::bridge::MessageType replyType;
    juce::MemoryBlock reply;
    if (! intelvst::bridge::receiveMessage (*socket, replyType, reply)
        || replyType != intelvst::bridge::MessageType::processResult)
    {
        setBridgeStatus ("Bridge process failed");
        return false;
    }

    juce::MemoryInputStream in (reply, false);
    const auto returnedChannels = in.readInt();
    const auto returnedSamples = in.readInt();

    if (returnedChannels != channels || returnedSamples != samples)
        return false;

    midi.clear();
    const auto midiEvents = in.readInt();
    for (auto event = 0; event < midiEvents; ++event)
    {
        const auto position = in.readInt();
        const auto bytes = in.readInt();
        juce::HeapBlock<juce::uint8> data (static_cast<size_t> (bytes));
        in.read (data.getData(), bytes);
        midi.addEvent (data.getData(), bytes, position);
    }

    for (auto channel = 0; channel < channels; ++channel)
        std::memcpy (buffer.getWritePointer (channel),
                     sharedOutput + static_cast<size_t> (channel) * static_cast<size_t> (sharedAudioSamples),
                     sizeof (float) * static_cast<size_t> (samples));

    return true;
}

bool IntelVSTWrapperAudioProcessor::ensureSharedAudioMemory (int channels, int samples)
{
    channels = juce::jmax (1, channels);
    samples = juce::jmax (1, samples);

    const auto requiredBytes = sharedAudioBytesFor (channels, samples);

    if (sharedAudioData != nullptr
        && sharedAudioChannels >= channels
        && sharedAudioSamples >= samples
        && sharedAudioByteCount >= requiredBytes)
    {
        return true;
    }

    releaseSharedAudioMemory();

    sharedAudioName = "/ivw"
                    + juce::String::toHexString (static_cast<int> (getpid()))
                    + juce::String::toHexString (static_cast<int> (++sharedAudioNameCounter));
    sharedAudioByteCount = requiredBytes;
    sharedAudioChannels = channels;
    sharedAudioSamples = samples;

    shm_unlink (sharedAudioName.toRawUTF8());
    sharedAudioFd = shm_open (sharedAudioName.toRawUTF8(), O_CREAT | O_EXCL | O_RDWR, 0600);

    if (sharedAudioFd < 0)
    {
        setBridgeStatus ("Could not create shared audio memory");
        wrapperLog ("shm_open failed: " + juce::String (std::strerror (errno)));
        releaseSharedAudioMemory();
        return false;
    }

    if (ftruncate (sharedAudioFd, static_cast<off_t> (sharedAudioByteCount)) != 0)
    {
        setBridgeStatus ("Could not size shared audio memory");
        wrapperLog ("ftruncate failed: " + juce::String (std::strerror (errno)));
        releaseSharedAudioMemory();
        return false;
    }

    sharedAudioData = mmap (nullptr, sharedAudioByteCount, PROT_READ | PROT_WRITE, MAP_SHARED, sharedAudioFd, 0);
    if (sharedAudioData == MAP_FAILED)
    {
        sharedAudioData = nullptr;
        setBridgeStatus ("Could not map shared audio memory");
        wrapperLog ("mmap failed: " + juce::String (std::strerror (errno)));
        releaseSharedAudioMemory();
        return false;
    }

    std::memset (sharedAudioData, 0, sharedAudioByteCount);
    wrapperLog ("Created shared audio memory " + sharedAudioName + " with "
                + juce::String (sharedAudioChannels) + " channels, "
                + juce::String (sharedAudioSamples) + " samples");
    return true;
}

bool IntelVSTWrapperAudioProcessor::receiveAckOrStatus (const char* action)
{
    intelvst::bridge::MessageType replyType;
    juce::MemoryBlock reply;

    if (! intelvst::bridge::receiveMessage (*socket, replyType, reply))
    {
        setBridgeStatus (juce::String ("Bridge disconnected during ") + action);
        wrapperLog (juce::String ("Bridge disconnected during ") + action);
        return false;
    }

    if (replyType == intelvst::bridge::MessageType::hello)
        return true;

    if (replyType == intelvst::bridge::MessageType::error)
    {
        juce::MemoryInputStream in (reply, false);
        const auto error = intelvst::bridge::readString (in);
        setBridgeStatus (error);
        wrapperLog (juce::String ("Bridge error during ") + action + ": " + error);
        return false;
    }

    setBridgeStatus (juce::String ("Unexpected bridge reply during ") + action);
    return false;
}

bool IntelVSTWrapperAudioProcessor::refreshHostedParameters()
{
    if (socket == nullptr || ! socket->isConnected())
        return false;

    if (! intelvst::bridge::sendMessage (*socket, intelvst::bridge::MessageType::getParameters))
    {
        setBridgeStatus ("Could not ask bridge for parameters");
        return false;
    }

    intelvst::bridge::MessageType replyType;
    juce::MemoryBlock reply;
    if (! intelvst::bridge::receiveMessage (*socket, replyType, reply)
        || replyType != intelvst::bridge::MessageType::parameters)
    {
        setBridgeStatus ("Could not read hosted plugin parameters");
        return false;
    }

    clearMirroredParameters();

    juce::MemoryInputStream in (reply, false);
    const auto parameterCount = juce::jlimit (0, maxMirroredParameters, in.readInt());

    for (auto i = 0; i < parameterCount; ++i)
    {
        const auto name = intelvst::bridge::readString (in);
        const auto label = intelvst::bridge::readString (in);
        const auto defaultValue = in.readFloat();
        const auto value = in.readFloat();
        const auto steps = in.readInt();
        const auto discrete = in.readBool();
        const auto boolean = in.readBool();

        if (auto* parameter = mirroredParameters[static_cast<size_t> (i)])
            parameter->updateMetadata (name, label, defaultValue, value, steps, discrete, boolean);

        lastSentParameterValues[static_cast<size_t> (i)] = -1.0f;
    }

    updateHostDisplay (juce::AudioProcessorListener::ChangeDetails()
                           .withParameterInfoChanged (true)
                           .withNonParameterStateChanged (true));
    wrapperLog ("Mirrored " + juce::String (parameterCount) + " hosted parameters");
    return true;
}

bool IntelVSTWrapperAudioProcessor::fetchHostedState (juce::MemoryBlock& state)
{
    if (socket == nullptr || ! socket->isConnected())
        return false;

    if (! intelvst::bridge::sendMessage (*socket, intelvst::bridge::MessageType::getState))
        return false;

    intelvst::bridge::MessageType replyType;
    juce::MemoryBlock reply;
    if (! intelvst::bridge::receiveMessage (*socket, replyType, reply)
        || replyType != intelvst::bridge::MessageType::state)
        return false;

    state = reply;
    return true;
}

bool IntelVSTWrapperAudioProcessor::sendHostedState (const juce::MemoryBlock& state)
{
    if (socket == nullptr || ! socket->isConnected())
        return false;

    if (! intelvst::bridge::sendMessage (*socket, intelvst::bridge::MessageType::setState, state))
    {
        setBridgeStatus ("Could not send hosted plugin state");
        return false;
    }

    if (! receiveAckOrStatus ("restore hosted plugin state"))
        return false;

    wrapperLog ("Restored hosted plugin state: " + juce::String (static_cast<int> (state.getSize())) + " bytes");
    return true;
}

void IntelVSTWrapperAudioProcessor::clearMirroredParameters()
{
    for (auto i = 0; i < maxMirroredParameters; ++i)
    {
        if (auto* parameter = mirroredParameters[static_cast<size_t> (i)])
            parameter->reset();

        lastSentParameterValues[static_cast<size_t> (i)] = -1.0f;
    }
}

juce::File IntelVSTWrapperAudioProcessor::findBundledHelper() const
{
    auto bundle = juce::File::getSpecialLocation (juce::File::currentExecutableFile);

    for (auto i = 0; i < 6; ++i)
    {
        auto candidate = bundle.getSiblingFile ("Resources")
                            .getChildFile ("IntelVSTBridgeHelper.app/Contents/MacOS/IntelVSTBridgeHelper");
        if (candidate.existsAsFile())
            return candidate;

        candidate = bundle.getChildFile ("Contents/Resources/IntelVSTBridgeHelper.app/Contents/MacOS/IntelVSTBridgeHelper");
        if (candidate.existsAsFile())
            return candidate;

        candidate = bundle.getSiblingFile ("Resources").getChildFile ("IntelVSTBridgeHelper");
        if (candidate.existsAsFile())
            return candidate;

        candidate = bundle.getChildFile ("Contents/Resources/IntelVSTBridgeHelper");
        if (candidate.existsAsFile())
            return candidate;

        bundle = bundle.getParentDirectory();
    }

    return {};
}

juce::String IntelVSTWrapperAudioProcessor::configuredPluginPath() const
{
    if (hostedPluginPath.isNotEmpty())
        return hostedPluginPath;

    const auto envPath = envVar ("INTEL_VST_PATH");
    if (envPath.isNotEmpty())
        return envPath;

    return JUCE_STRINGIFY (INTEL_VST_WRAPPER_DEFAULT_PLUGIN);
}

void IntelVSTWrapperAudioProcessor::stopHelper()
{
    if (socket != nullptr && socket->isConnected())
        intelvst::bridge::sendMessage (*socket, intelvst::bridge::MessageType::shutdown);

    socket.reset();
    listener.reset();

    if (helperProcess.isRunning())
        helperProcess.kill();

    pluginLoaded = false;
    helperPrepared = false;
    releaseSharedAudioMemory();
}

void IntelVSTWrapperAudioProcessor::setBridgeStatus (const juce::String& status)
{
    bridgeStatus = status;
}

void IntelVSTWrapperAudioProcessor::releaseSharedAudioMemory()
{
    if (sharedAudioData != nullptr)
    {
        munmap (sharedAudioData, sharedAudioByteCount);
        sharedAudioData = nullptr;
    }

    if (sharedAudioFd >= 0)
    {
        close (sharedAudioFd);
        sharedAudioFd = -1;
    }

    if (sharedAudioName.isNotEmpty())
    {
        shm_unlink (sharedAudioName.toRawUTF8());
        sharedAudioName.clear();
    }

    sharedAudioByteCount = 0;
    sharedAudioChannels = 0;
    sharedAudioSamples = 0;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new IntelVSTWrapperAudioProcessor();
}
