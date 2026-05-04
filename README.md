# Intel VST Wrapper

This is a small JUCE proof-of-concept bridge for Apple Silicon hosts.

An ARM DAW cannot directly load an Intel-only VST because Rosetta translates whole
processes, not individual plugin dynamic libraries. This project therefore builds:

- `IntelVSTWrapper`: an AU/VST3 wrapper plugin that can be loaded by an ARM DAW.
- `IntelVSTBridgeHelper`: an `x86_64` helper executable. macOS launches it under
  Rosetta, and it hosts the Intel plugin out-of-process.

Audio is passed between the wrapper and helper over a local TCP socket using a
small synchronous binary protocol. This is intentionally simple and suitable as a
starting point, not a polished commercial bridge.

## Requirements

- macOS on Apple Silicon
- Rosetta 2 installed
- CMake 3.22+
- Xcode command line tools
- JUCE 8, either supplied with `-DJUCE_DIR=/path/to/JUCE` or fetched by CMake
- For older VST2 plug-ins with legacy UI dependencies, an extracted macOS
  10.13 SDK and a JUCE 6 checkout are recommended for the helper process.

## Build

```sh
cmake -S . -B build -G Xcode \
  -DINTEL_VST_WRAPPER_DEFAULT_PLUGIN="/absolute/path/to/IntelPlugin.vst3"

cmake --build build --config Release
```

The wrapper copies `IntelVSTBridgeHelper` into the plugin bundle resources.

For Intel VST2 plugins with older Carbon-era UI assumptions, build the helper
through the legacy path:

```sh
cmake -S . -B build \
  -DINTEL_VST_WRAPPER_ENABLE_VST2=ON \
  -DINTEL_VST_WRAPPER_ENABLE_HOSTED_UI=ON \
  -DINTEL_VST_WRAPPER_USE_LEGACY_HELPER=ON \
  -DINTEL_VST_WRAPPER_VST2_SDK_ROOT="/path/to/VST3 SDK" \
  -DINTEL_VST_WRAPPER_LEGACY_JUCE_ROOT="/path/to/JUCE-6.1.6" \
  -DINTEL_VST_WRAPPER_LEGACY_MACOS_SDK="/path/to/MacOSX10.13.sdk"
```

## Selecting the Hosted Plugin

Open the wrapper plugin editor in your DAW and click `Choose...`, then select
the Intel-only `.vst3` or `.vst` bundle to host. The selected path is stored in
the wrapper plugin state so DAW sessions can reopen with the same hosted plugin.

For headless testing or a fixed default, you can also set it at runtime:

```sh
export INTEL_VST_PATH="/absolute/path/to/IntelPlugin.vst3"
```

Some older plug-ins expect resource files to be found relative to the current
working directory. You can set an explicit working directory for the helper:

```sh
export INTEL_VST_WRAPPER_PLUGIN_DATA_DIR="/absolute/path/to/plugin/resources"
```

For legacy Audio Unit bundles that are not discoverable from the bundle path
alone, you can provide a JUCE-style identifier fallback:

```sh
export INTEL_VST_WRAPPER_AUDIOUNIT_IDENTIFIER="AudioUnit:Effects/aumf,TYPE,MANU"
```

Or bake a fallback into the binary at configure time:

```sh
cmake -S . -B build \
  -DINTEL_VST_WRAPPER_DEFAULT_PLUGIN="/absolute/path/to/IntelPlugin.vst3"
```

You can also override the helper location:

```sh
export INTEL_VST_WRAPPER_HELPER_PATH="/absolute/path/to/IntelVSTBridgeHelper"
```

## Current Scope

Implemented:

- ARM/universal JUCE wrapper target
- x86_64 Rosetta helper target
- out-of-process VST/VST3 hosting through JUCE plugin formats
- synchronous audio buffer processing
- basic MIDI event forwarding
- hosted plugin path save/restore
- hosted plugin editor windows opened from the x86_64 helper process

Known limitations:

- Hosted plugin editors appear in a separate helper-owned window, not embedded
  inside the wrapper editor
- No parameter automation bridge yet
- Hosted plugin state/preset data is not bridged yet
- The synchronous socket path is simple, but not real-time ideal
- VST2 support requires Steinberg VST2 SDK headers

## VST2 Plugins

VST2 hosting is disabled by default because JUCE needs the legacy Steinberg VST2
headers. To host Intel VST2 plug-ins, configure with:

```sh
cmake -S . -B build \
  -DINTEL_VST_WRAPPER_ENABLE_VST2=ON \
  -DINTEL_VST_WRAPPER_VST2_SDK_ROOT="/path/to/VST3 SDK"
```

The SDK root must contain `pluginterfaces/vst2.x/aeffect.h`.

The wrapper writes runtime status logs here:

```sh
~/Library/Logs/IntelVSTWrapper.log
~/Library/Logs/IntelVSTWrapperHelper.log
```

For serious low-latency work, the next step is replacing the per-block socket
payload with shared memory plus a lock-free command ring.
