#include "BridgeProtocol.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#if JUCE_MAC && INTEL_VST_WRAPPER_ENABLE_HOSTED_UI
 #import <Cocoa/Cocoa.h>
 #import <objc/runtime.h>
#endif

namespace
{
    void helperLog (const juce::String& message)
    {
        const auto logFile = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                .getChildFile ("Logs/IntelVSTWrapperHelper.log");
        logFile.getParentDirectory().createDirectory();
        logFile.appendText (juce::Time::getCurrentTime().toISO8601 (true) + " " + message + "\n");
    }

    class HostedPlugin
    {
    public:
        ~HostedPlugin()
        {
            closeEditorSync();
            unmapSharedAudioMemory();
        }

        bool load (const juce::String& path, juce::String& error)
        {
            closeEditorSync();

            const juce::ScopedLock lock (pluginLock);

            instance.reset();
            hostedPluginPath = path;
            applyPluginResourceWorkingDirectory();
            helperLog ("Loading " + path);
            auto pathsToTry = juce::StringArray { path };

            if (auto legacyIdentifier = legacyAudioUnitIdentifierForPath (path);
                legacyIdentifier.isNotEmpty())
            {
                helperLog ("Adding legacy AudioUnit identifier fallback " + legacyIdentifier);
                pathsToTry.addIfNotAlreadyThere (legacyIdentifier);
            }

            juce::AudioPluginFormatManager manager;
           #if JUCE_MAJOR_VERSION < 8
            manager.addDefaultFormats();
           #elif INTEL_VST_WRAPPER_ENABLE_HOSTED_UI
            juce::addDefaultFormatsToManager (manager);
           #else
            juce::addHeadlessDefaultFormatsToManager (manager);
           #endif

            for (auto* format : manager.getFormats())
            {
                for (const auto& pathToTry : pathsToTry)
                {
                    juce::OwnedArray<juce::PluginDescription> descriptions;
                    format->findAllTypesForFile (descriptions, pathToTry);
                    helperLog ("Format " + format->getName() + " found " + juce::String (descriptions.size())
                               + " descriptions for " + pathToTry);

                    for (auto* description : descriptions)
                    {
                        instance = manager.createPluginInstance (*description, sampleRate, blockSize, error);
                        if (instance != nullptr)
                            return true;
                    }
                }
            }

            if (error.isEmpty())
                error = "No compatible x86_64 plugin format found for " + path;

            helperLog ("Load failed: " + error);
            return false;
        }

        bool prepare (double newSampleRate,
                      int newBlockSize,
                      int inputs,
                      int outputs,
                      const juce::String& sharedName,
                      int sharedChannels,
                      int sharedSamples,
                      size_t sharedBytes,
                      juce::String& error)
        {
            const juce::ScopedLock lock (pluginLock);

            sampleRate = newSampleRate;
            blockSize = newBlockSize;
            totalInputs = inputs;
            totalOutputs = outputs;

            if (instance == nullptr)
            {
                error = "No plugin loaded";
                return false;
            }

            if (! mapSharedAudioMemory (sharedName, sharedChannels, sharedSamples, sharedBytes, error))
                return false;

            instance->setRateAndBufferSizeDetails (sampleRate, blockSize);
            instance->prepareToPlay (sampleRate, blockSize);
            return true;
        }

        bool openEditor (juce::String& error)
        {
           #if INTEL_VST_WRAPPER_ENABLE_HOSTED_UI
            if (juce::MessageManager::getInstance()->isThisTheMessageThread())
                return openEditorOnMessageThread (error);

            juce::WaitableEvent completed;
            juce::String asyncError;
            bool opened = false;

            juce::MessageManager::callAsync ([this, &completed, &asyncError, &opened]
            {
                opened = openEditorOnMessageThread (asyncError);
                completed.signal();
            });

            if (! completed.wait (5000))
            {
                error = "Timed out opening hosted plugin UI";
                return false;
            }

            error = asyncError;
            return opened;
           #else
            error = "Hosted plugin UI is disabled in stable audio bridge mode";
            return false;
           #endif
        }

        bool process (juce::MemoryBlock& payload, juce::MemoryBlock& result)
        {
            const juce::ScopedLock lock (pluginLock);

            if (instance == nullptr)
                return false;

            juce::MemoryInputStream in (payload, false);
            const auto channels = in.readInt();
            const auto samples = in.readInt();
            const auto parameterUpdates = in.readInt();

            if (sharedAudioData == nullptr
                || channels > sharedAudioChannels
                || samples > sharedAudioSamples)
            {
                return false;
            }

            auto parameters = instance->getParameters();
            for (auto update = 0; update < parameterUpdates; ++update)
            {
                const auto index = in.readInt();
                const auto value = in.readFloat();

                if (juce::isPositiveAndBelow (index, parameters.size()))
                    parameters.getUnchecked (index)->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, value));
            }

            const auto midiEvents = in.readInt();

            juce::MidiBuffer midi;
            for (auto event = 0; event < midiEvents; ++event)
            {
                const auto position = in.readInt();
                const auto bytes = in.readInt();
                juce::HeapBlock<juce::uint8> data (static_cast<size_t> (bytes));
                in.read (data.getData(), bytes);
                midi.addEvent (data.getData(), bytes, position);
            }

            juce::AudioBuffer<float> buffer (channels, samples);
            auto* const sharedInput = static_cast<float*> (sharedAudioData);
            auto* const sharedOutput = sharedInput + static_cast<size_t> (sharedAudioChannels) * static_cast<size_t> (sharedAudioSamples);

            for (auto channel = 0; channel < channels; ++channel)
                std::memcpy (buffer.getWritePointer (channel),
                             sharedInput + static_cast<size_t> (channel) * static_cast<size_t> (sharedAudioSamples),
                             sizeof (float) * static_cast<size_t> (samples));

            instance->processBlock (buffer, midi);

            for (auto channel = 0; channel < buffer.getNumChannels(); ++channel)
                std::memcpy (sharedOutput + static_cast<size_t> (channel) * static_cast<size_t> (sharedAudioSamples),
                             buffer.getReadPointer (channel),
                             sizeof (float) * static_cast<size_t> (buffer.getNumSamples()));

            juce::MemoryOutputStream out (result, false);
            out.writeInt (buffer.getNumChannels());
            out.writeInt (buffer.getNumSamples());
            out.writeInt (midi.getNumEvents());

            for (const auto metadata : midi)
            {
                out.writeInt (metadata.samplePosition);
                out.writeInt (metadata.numBytes);
                out.write (metadata.data, static_cast<size_t> (metadata.numBytes));
            }

            return true;
        }

        bool getState (juce::MemoryBlock& result)
        {
            const juce::ScopedLock lock (pluginLock);

            if (instance == nullptr)
                return false;

            juce::MemoryBlock pluginState;
            instance->getStateInformation (pluginState);

            juce::MemoryOutputStream out (result, false);
            out.writeInt (instance->getCurrentProgram());
            out.writeInt (static_cast<int> (pluginState.getSize()));

            if (! pluginState.isEmpty())
                out.write (pluginState.getData(), pluginState.getSize());

            helperLog ("Captured hosted plugin state: " + juce::String (static_cast<int> (result.getSize())) + " bytes");
            return true;
        }

        bool setState (const juce::MemoryBlock& state)
        {
            const juce::ScopedLock lock (pluginLock);

            if (instance == nullptr)
                return false;

            juce::MemoryBlock pluginState;
            auto program = -1;

            if (state.getSize() >= 8)
            {
                juce::MemoryInputStream in (state, false);
                program = in.readInt();
                const auto stateBytes = in.readInt();

                if (stateBytes >= 0 && stateBytes <= static_cast<int> (state.getSize() - 8))
                {
                    pluginState.setSize (static_cast<size_t> (stateBytes), false);

                    if (stateBytes > 0)
                        in.read (pluginState.getData(), stateBytes);
                }
            }

            if (pluginState.isEmpty())
                pluginState = state;

            instance->setStateInformation (pluginState.getData(), static_cast<int> (pluginState.getSize()));

            if (juce::isPositiveAndBelow (program, instance->getNumPrograms()))
                instance->setCurrentProgram (program);

            helperLog ("Applied hosted plugin state: " + juce::String (static_cast<int> (state.getSize())) + " bytes");
            return true;
        }

        bool getParameters (juce::MemoryBlock& result)
        {
            const juce::ScopedLock lock (pluginLock);

            if (instance == nullptr)
                return false;

            auto parameters = instance->getParameters();
            juce::MemoryOutputStream out (result, false);
            out.writeInt (parameters.size());

            for (auto* parameter : parameters)
            {
                intelvst::bridge::writeString (out, parameter->getName (128));
                intelvst::bridge::writeString (out, parameter->getLabel());
                out.writeFloat (parameter->getDefaultValue());
                out.writeFloat (parameter->getValue());
                out.writeInt (parameter->getNumSteps());
                out.writeBool (parameter->isDiscrete());
                out.writeBool (parameter->isBoolean());
            }

            helperLog ("Reported " + juce::String (parameters.size()) + " hosted parameters");
            return true;
        }

    private:
       #if INTEL_VST_WRAPPER_ENABLE_HOSTED_UI
        bool openEditorOnMessageThread (juce::String& error)
        {
            jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
            const juce::ScopedLock lock (pluginLock);

            if (instance == nullptr)
            {
                error = "No plugin loaded";
                return false;
            }

            if (editorWindow != nullptr)
           {
                juce::Process::makeForegroundProcess();
                editorWindow->setVisible (true);
                editorWindow->setAlwaysOnTop (true);
                editorWindow->toFront (true);
                nudgeEditorWindow();
                return true;
            }

            auto* editor = instance->createEditorIfNeeded();
            if (editor == nullptr)
            {
                error = "Hosted plugin did not create an editor";
                return false;
            }

            if (editor->getWidth() < 100 || editor->getHeight() < 100)
            {
                helperLog ("Hosted editor had small bounds "
                           + juce::String (editor->getWidth()) + "x" + juce::String (editor->getHeight())
                           + ", forcing fallback size");
                editor->setSize (900, 600);
            }
            else
            {
                helperLog ("Hosted editor bounds "
                           + juce::String (editor->getWidth()) + "x" + juce::String (editor->getHeight()));
            }

            editor->setOpaque (false);
            applyPluginResourceWorkingDirectory();
            prepareVSTGUIRendererDiagnostics();
            juce::Process::setDockIconVisible (true);
            juce::Process::makeForegroundProcess();

            editorWindow = std::make_unique<PluginEditorWindow> (instance->getName());
            editorWindow->setContentOwned (editor, true);
            editorWindow->setAlwaysOnTop (true);
            editorWindow->centreWithSize (editor->getWidth(), editor->getHeight());
            editorWindow->setVisible (true);
            editorWindow->toFront (true);
            editorWindow->grabKeyboardFocus();
            nudgeEditorWindow();
            helperLog ("Opened editor for " + instance->getName());
            return true;
        }
       #endif

        void nudgeEditorWindow()
        {
           #if INTEL_VST_WRAPPER_ENABLE_HOSTED_UI
            if (editorWindow == nullptr)
                return;

            auto* content = editorWindow->getContentComponent();
            if (content == nullptr)
                return;

            wakeNativeEditorViews (*content);
            helperLog ("Editor content after attach: "
                       + juce::String (content->getWidth()) + "x" + juce::String (content->getHeight())
                       + ", children " + juce::String (content->getNumChildComponents())
                       + ", window handle " + juce::String::toHexString ((juce::pointer_sized_int) content->getWindowHandle()));
            logNativeEditorViews (*content);

            content->setVisible (true);
            content->setOpaque (false);
            content->setBounds (0, 0, content->getWidth(), content->getHeight());
            content->repaint();
            editorWindow->resized();
            editorWindow->repaint();

            const juce::Component::SafePointer<juce::Component> safeContent (content);
            for (auto delay : { 50, 150, 350, 700 })
            {
                juce::Timer::callAfterDelay (delay, [safeContent]
                {
                    if (auto* component = safeContent.getComponent())
                    {
                        wakeNativeEditorViews (*component);
                        const auto bounds = component->getBounds();
                        component->setSize (bounds.getWidth() + 1, bounds.getHeight() + 1);
                        component->setBounds (bounds);
                        component->toFront (true);
                        component->repaint();
                    }
                });
            }
           #endif
        }

        bool mapSharedAudioMemory (const juce::String& name,
                                   int channels,
                                   int samples,
                                   size_t bytes,
                                   juce::String& error)
        {
            if (name.isEmpty() || channels <= 0 || samples <= 0 || bytes == 0)
            {
                error = "Shared audio memory was not configured";
                return false;
            }

            if (sharedAudioData != nullptr
                && sharedAudioName == name
                && sharedAudioChannels >= channels
                && sharedAudioSamples >= samples
                && sharedAudioByteCount >= bytes)
            {
                return true;
            }

            unmapSharedAudioMemory();

            sharedAudioFd = shm_open (name.toRawUTF8(), O_RDWR, 0600);
            if (sharedAudioFd < 0)
            {
                error = "Could not open shared audio memory";
                helperLog ("shm_open failed for " + name + ": " + juce::String (std::strerror (errno)));
                return false;
            }

            sharedAudioData = mmap (nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, sharedAudioFd, 0);
            if (sharedAudioData == MAP_FAILED)
            {
                sharedAudioData = nullptr;
                error = "Could not map shared audio memory";
                helperLog ("mmap failed for " + name + ": " + juce::String (std::strerror (errno)));
                unmapSharedAudioMemory();
                return false;
            }

            sharedAudioName = name;
            sharedAudioByteCount = bytes;
            sharedAudioChannels = channels;
            sharedAudioSamples = samples;
            helperLog ("Mapped shared audio memory " + name + " with "
                       + juce::String (channels) + " channels, "
                       + juce::String (samples) + " samples");
            return true;
        }

        void unmapSharedAudioMemory()
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

            sharedAudioName.clear();
            sharedAudioByteCount = 0;
            sharedAudioChannels = 0;
            sharedAudioSamples = 0;
        }

        static void prepareVSTGUIRendererDiagnostics()
        {
           #if JUCE_MAC && INTEL_VST_WRAPPER_ENABLE_HOSTED_UI
            static std::once_flag once;
            std::call_once (once, []
            {
                auto* klass = objc_getClass ("VSTGUI_NSView");
                if (klass == Nil)
                {
                    helperLog ("VSTGUI_NSView class was not registered before editor open");
                    return;
                }

                const auto selector = @selector (drawRect:);
                auto* method = class_getInstanceMethod (klass, selector);
                if (method == nullptr)
                {
                    helperLog ("VSTGUI_NSView has no drawRect: method");
                    return;
                }

                originalVSTGUIDrawRectImp = method_getImplementation (method);
                auto replacement = imp_implementationWithBlock (^void (id self, NSRect rect)
                {
                    static std::atomic<int> drawCount { 0 };
                    const auto count = ++drawCount;

                    if (count <= 12)
                    {
                        helperLog ("VSTGUI_NSView drawRect #"
                                   + juce::String (count)
                                   + " rect=" + juce::String (rect.origin.x, 1) + "," + juce::String (rect.origin.y, 1)
                                   + " " + juce::String (rect.size.width, 1) + "x" + juce::String (rect.size.height, 1));
                    }

                    ((void (*) (id, SEL, NSRect)) originalVSTGUIDrawRectImp) (self, selector, rect);
                });

                method_setImplementation (method, replacement);
                helperLog ("Installed VSTGUI_NSView drawRect diagnostics");
            });
           #endif
        }

        static void logNativeEditorViews (juce::Component& content)
        {
           #if JUCE_MAC && INTEL_VST_WRAPPER_ENABLE_HOSTED_UI
            juce::StringArray lines;
            appendNativeViewDescription ((NSView*) content.getWindowHandle(), "content", lines, 0);

            if (auto* child = content.getChildComponent (0))
                appendNativeViewDescription ((NSView*) child->getWindowHandle(), "host-child", lines, 0);

            helperLog ("Native editor view tree:\n" + lines.joinIntoString ("\n"));
           #else
            juce::ignoreUnused (content);
           #endif
        }

        static void wakeNativeEditorViews (juce::Component& content)
        {
           #if JUCE_MAC && INTEL_VST_WRAPPER_ENABLE_HOSTED_UI
            wakeNativeView ((NSView*) content.getWindowHandle());

            if (auto* child = content.getChildComponent (0))
                wakeNativeView ((NSView*) child->getWindowHandle());
           #else
            juce::ignoreUnused (content);
           #endif
        }

        class PluginEditorWindow final : public juce::DocumentWindow
        {
        public:
            explicit PluginEditorWindow (const juce::String& name)
                : DocumentWindow (name,
                                  juce::Colours::black,
                                  juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton,
                                  false)
            {
                setOpaque (true);
                setUsingNativeTitleBar (true);
                setResizable (true, false);
                addToDesktop();
            }

            void closeButtonPressed() override
            {
                setVisible (false);
            }

            int getDesktopWindowStyleFlags() const override
            {
                auto flags = juce::DocumentWindow::getDesktopWindowStyleFlags();

               #if JUCE_MAJOR_VERSION >= 8
                flags |= juce::ComponentPeer::windowRequiresSynchronousCoreGraphicsRendering;
               #endif

                return flags;
            }
        };

        void closeEditorOnMessageThread()
        {
            jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
            editorWindow.reset();
        }

        void closeEditorSync()
        {
            if (editorWindow == nullptr)
                return;

            if (juce::MessageManager::getInstance()->isThisTheMessageThread())
            {
                closeEditorOnMessageThread();
                return;
            }

            juce::WaitableEvent completed;
            juce::MessageManager::callAsync ([this, &completed]
            {
                const juce::ScopedLock lock (pluginLock);
                closeEditorOnMessageThread();
                completed.signal();
            });
            completed.wait (2000);
        }

        juce::CriticalSection pluginLock;
        std::unique_ptr<juce::AudioPluginInstance> instance;
        std::unique_ptr<PluginEditorWindow> editorWindow;
        juce::String hostedPluginPath;
        juce::String sharedAudioName;
        int sharedAudioFd = -1;
        void* sharedAudioData = nullptr;
        size_t sharedAudioByteCount = 0;
        int sharedAudioChannels = 0;
        int sharedAudioSamples = 0;
        double sampleRate = 44100.0;
        int blockSize = 512;
        int totalInputs = 2;
        int totalOutputs = 2;

        void applyPluginResourceWorkingDirectory()
        {
            const auto dataDirectory = findPluginDataDirectory();
            if (! dataDirectory.isDirectory())
                return;

            if (dataDirectory.setAsCurrentWorkingDirectory())
                helperLog ("Set helper working directory to " + dataDirectory.getFullPathName());
            else
                helperLog ("Failed to set helper working directory to " + dataDirectory.getFullPathName());
        }

        juce::File findPluginDataDirectory() const
        {
            if (auto* configuredPath = std::getenv ("INTEL_VST_WRAPPER_PLUGIN_DATA_DIR"))
            {
                auto directory = juce::File (juce::String::fromUTF8 (configuredPath));

                if (directory.isDirectory())
                    return directory;
            }

            return {};
        }

        static juce::String legacyAudioUnitIdentifierForPath (const juce::String& path)
        {
            const auto file = juce::File (path);

            if (file.hasFileExtension (".component"))
                if (auto* configuredIdentifier = std::getenv ("INTEL_VST_WRAPPER_AUDIOUNIT_IDENTIFIER"))
                    return juce::String::fromUTF8 (configuredIdentifier);

            return {};
        }

       #if JUCE_MAC && INTEL_VST_WRAPPER_ENABLE_HOSTED_UI
        static juce::String nativeViewName (NSView* view)
        {
            if (view == nil)
                return "<nil>";

            return juce::String::fromUTF8 ([[view description] UTF8String]);
        }

        static juce::String nativeViewSummary (NSView* view, const juce::String& label, int depth)
        {
            if (view == nil)
                return juce::String::repeatedString ("  ", depth) + label + ": <nil>";

            const auto frame = [view frame];
            return juce::String::repeatedString ("  ", depth)
                 + label + ": " + nativeViewName (view)
                 + " frame=" + juce::String (frame.origin.x, 1) + "," + juce::String (frame.origin.y, 1)
                 + " " + juce::String (frame.size.width, 1) + "x" + juce::String (frame.size.height, 1)
                 + " hidden=" + juce::String ([view isHidden] ? "yes" : "no")
                 + " wantsLayer=" + juce::String ([view wantsLayer] ? "yes" : "no")
                 + " subviews=" + juce::String ((int) [[view subviews] count]);
        }

        static void appendNativeViewDescription (NSView* view, const juce::String& label, juce::StringArray& lines, int depth)
        {
            lines.add (nativeViewSummary (view, label, depth));

            if (view == nil || depth >= 4)
                return;

            auto* subviews = [view subviews];
            for (NSUInteger i = 0; i < [subviews count]; ++i)
                appendNativeViewDescription ((NSView*) [subviews objectAtIndex: i],
                                             "subview " + juce::String ((int) i),
                                             lines,
                                             depth + 1);
        }

        static void wakeNativeView (NSView* view)
        {
            if (view == nil)
                return;

            [view setHidden: NO];
            [view setWantsLayer: NO];

            if ([view respondsToSelector: @selector (setCanDrawSubviewsIntoLayer:)])
                [view setCanDrawSubviewsIntoLayer: NO];

            [view setNeedsDisplay: YES];
            [view displayIfNeeded];
            [view display];
            [view displayRectIgnoringOpacity: [view bounds]];

            auto* subviews = [view subviews];
            for (NSUInteger i = 0; i < [subviews count]; ++i)
                wakeNativeView ((NSView*) [subviews objectAtIndex: i]);

            if (auto* window = [view window])
            {
                [window displayIfNeeded];
                [window invalidateShadow];
            }
        }

        static IMP originalVSTGUIDrawRectImp;
       #endif
    };

   #if JUCE_MAC && INTEL_VST_WRAPPER_ENABLE_HOSTED_UI
    IMP HostedPlugin::originalVSTGUIDrawRectImp = nullptr;
   #endif

    bool sendAck (juce::StreamingSocket& socket)
    {
        return intelvst::bridge::sendMessage (socket, intelvst::bridge::MessageType::hello);
    }

    bool sendError (juce::StreamingSocket& socket, const juce::String& message)
    {
        helperLog ("Error: " + message);
        juce::MemoryBlock payload;
        juce::MemoryOutputStream out (payload, false);
        intelvst::bridge::writeString (out, message);
        return intelvst::bridge::sendMessage (socket, intelvst::bridge::MessageType::error, payload);
    }

    int runBridge (int port, const std::function<bool()>& shouldStop)
    {
        juce::StreamingSocket socket;

        if (! socket.connect ("127.0.0.1", port, 3000))
            return 3;

        helperLog ("Helper connected to wrapper on port " + juce::String (port));

        if (! sendAck (socket))
            return 4;

        HostedPlugin plugin;
        bool running = true;

        while (running && socket.isConnected() && ! shouldStop())
        {
            intelvst::bridge::MessageType type;
            juce::MemoryBlock payload;

            if (! socket.waitUntilReady (true, 100))
                continue;

            if (! intelvst::bridge::receiveMessage (socket, type, payload))
                break;

            switch (type)
            {
                case intelvst::bridge::MessageType::loadPlugin:
                {
                    juce::MemoryInputStream in (payload, false);
                    auto path = intelvst::bridge::readString (in);
                    juce::String error;

                    if (plugin.load (path, error))
                        sendAck (socket);
                    else
                        sendError (socket, error);

                    break;
                }

                case intelvst::bridge::MessageType::prepare:
                {
                    juce::MemoryInputStream in (payload, false);
                    const auto sampleRate = in.readDouble();
                    const auto blockSize = in.readInt();
                    const auto inputs = in.readInt();
                    const auto outputs = in.readInt();
                    const auto sharedName = intelvst::bridge::readString (in);
                    const auto sharedChannels = in.readInt();
                    const auto sharedSamples = in.readInt();
                    const auto sharedBytes = static_cast<size_t> (in.readInt64());
                    juce::String error;

                    if (plugin.prepare (sampleRate,
                                        blockSize,
                                        inputs,
                                        outputs,
                                        sharedName,
                                        sharedChannels,
                                        sharedSamples,
                                        sharedBytes,
                                        error))
                        sendAck (socket);
                    else
                        sendError (socket, error);

                    break;
                }

                case intelvst::bridge::MessageType::process:
                {
                    juce::MemoryBlock result;
                    if (plugin.process (payload, result))
                        intelvst::bridge::sendMessage (socket, intelvst::bridge::MessageType::processResult, result);
                    else
                        sendError (socket, "Process failed");

                    break;
                }

                case intelvst::bridge::MessageType::openEditor:
                {
                    juce::String error;
                    if (plugin.openEditor (error))
                        sendAck (socket);
                    else
                        sendError (socket, error);

                    break;
                }

                case intelvst::bridge::MessageType::getState:
                {
                    juce::MemoryBlock state;
                    if (plugin.getState (state))
                        intelvst::bridge::sendMessage (socket, intelvst::bridge::MessageType::state, state);
                    else
                        sendError (socket, "Could not get hosted plugin state");

                    break;
                }

                case intelvst::bridge::MessageType::setState:
                {
                    if (plugin.setState (payload))
                        sendAck (socket);
                    else
                        sendError (socket, "Could not restore hosted plugin state");

                    break;
                }

                case intelvst::bridge::MessageType::getParameters:
                {
                    juce::MemoryBlock parameters;
                    if (plugin.getParameters (parameters))
                        intelvst::bridge::sendMessage (socket, intelvst::bridge::MessageType::parameters, parameters);
                    else
                        sendError (socket, "Could not read hosted plugin parameters");

                    break;
                }

                case intelvst::bridge::MessageType::shutdown:
                    running = false;
                    break;

                case intelvst::bridge::MessageType::hello:
                case intelvst::bridge::MessageType::processResult:
                case intelvst::bridge::MessageType::error:
                case intelvst::bridge::MessageType::state:
                case intelvst::bridge::MessageType::parameters:
                default:
                    sendError (socket, "Unexpected bridge message");
                    break;
            }
        }

        return 0;
    }
}

#if INTEL_VST_WRAPPER_ENABLE_HOSTED_UI
class BridgeConnectionThread final : public juce::Thread
{
public:
    explicit BridgeConnectionThread (int portToUse)
        : juce::Thread ("Intel VST bridge connection"),
          port (portToUse)
    {
    }

    void run() override
    {
        exitCode = runBridge (port, [this] { return threadShouldExit(); });
        juce::MessageManager::callAsync ([] { juce::JUCEApplicationBase::quit(); });
    }

    int getExitCode() const { return exitCode; }

private:
    int port = 0;
    std::atomic<int> exitCode { 0 };
};

class BridgeHelperApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "IntelVSTBridgeHelper"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String& commandLine) override
    {
        juce::Process::setDockIconVisible (true);
        juce::Process::makeForegroundProcess();

        const auto port = commandLine.trim().getIntValue();
        if (port <= 0)
        {
            helperLog ("Missing bridge port in command line: " + commandLine);
            quit();
            return;
        }

        connectionThread = std::make_unique<BridgeConnectionThread> (port);
        connectionThread->startThread();
    }

    void shutdown() override
    {
        if (connectionThread != nullptr)
        {
            connectionThread->signalThreadShouldExit();
            connectionThread->stopThread (2000);
            connectionThread.reset();
        }
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    void anotherInstanceStarted (const juce::String&) override {}

private:
    std::unique_ptr<BridgeConnectionThread> connectionThread;
};

START_JUCE_APPLICATION (BridgeHelperApplication)
#else
int main (int argc, char* argv[])
{
    juce::ignoreUnused (argc);
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    if (argc < 2)
        return 2;

    const auto port = juce::String (argv[1]).getIntValue();
    return runBridge (port, [] { return false; });
}
#endif
