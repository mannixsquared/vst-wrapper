cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
set(CMAKE_APPLE_SILICON_PROCESSOR "x86_64" CACHE STRING "Host architecture for helper build tools" FORCE)
set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "Helper architecture" FORCE)
set(INTEL_VST_WRAPPER_LEGACY_MACOS_SDK "" CACHE PATH "macOS 10.13 SDK used for the helper")
set(INTEL_VST_WRAPPER_LEGACY_LIBCXX_INCLUDE "/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk/usr/include/c++/v1" CACHE PATH "Modern libc++ headers used when compiling against the macOS 10.13 SDK")

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND EXISTS "${INTEL_VST_WRAPPER_LEGACY_MACOS_SDK}/SDKSettings.plist")
    set(INTEL_VST_WRAPPER_LEGACY_SDK_LINK "${CMAKE_BINARY_DIR}/MacOSX10.13.sdk")

    if(NOT EXISTS "${INTEL_VST_WRAPPER_LEGACY_SDK_LINK}")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E create_symlink
                        "${INTEL_VST_WRAPPER_LEGACY_MACOS_SDK}"
                        "${INTEL_VST_WRAPPER_LEGACY_SDK_LINK}")
    endif()

    set(CMAKE_OSX_SYSROOT "${INTEL_VST_WRAPPER_LEGACY_SDK_LINK}" CACHE PATH "macOS SDK" FORCE)
    set(CMAKE_C_FLAGS "-arch x86_64")
    set(CMAKE_CXX_FLAGS "-arch x86_64 -isystem ${INTEL_VST_WRAPPER_LEGACY_LIBCXX_INCLUDE}")
    set(CMAKE_OBJCXX_FLAGS "-arch x86_64 -isystem ${INTEL_VST_WRAPPER_LEGACY_LIBCXX_INCLUDE}")
    set(ENV{CMAKE_APPLE_SILICON_PROCESSOR} "x86_64")
    set(ENV{SDKROOT} "${INTEL_VST_WRAPPER_LEGACY_SDK_LINK}")
    set(ENV{MACOSX_DEPLOYMENT_TARGET} "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    set(ENV{ARCHFLAGS} "-arch x86_64")
    set(ENV{CFLAGS} "-arch x86_64")
    set(ENV{CXXFLAGS} "-arch x86_64 -isystem ${INTEL_VST_WRAPPER_LEGACY_LIBCXX_INCLUDE}")
    set(ENV{OBJCXXFLAGS} "-arch x86_64 -isystem ${INTEL_VST_WRAPPER_LEGACY_LIBCXX_INCLUDE}")
endif()

project(IntelVSTBridgeHelperLegacy VERSION 0.1.0 LANGUAGES C CXX OBJCXX)

include(FetchContent)

option(INTEL_VST_WRAPPER_ENABLE_VST2 "Enable VST2 hosting in the Rosetta helper" OFF)
set(INTEL_VST_WRAPPER_VST2_SDK_ROOT "" CACHE PATH "Folder containing pluginterfaces/vst2.x/aeffect.h")

if(DEFINED JUCE_ROOT AND EXISTS "${JUCE_ROOT}/CMakeLists.txt")
    add_subdirectory("${JUCE_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/JUCE")
else()
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        6.1.6
        GIT_PROGRESS   TRUE
        UPDATE_DISCONNECTED TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

set(INTEL_VST_WRAPPER_VST2_HOST_VALUE 0)

if(INTEL_VST_WRAPPER_ENABLE_VST2)
    if(NOT EXISTS "${INTEL_VST_WRAPPER_VST2_SDK_ROOT}/pluginterfaces/vst2.x/aeffect.h")
        message(FATAL_ERROR
            "INTEL_VST_WRAPPER_ENABLE_VST2 requires INTEL_VST_WRAPPER_VST2_SDK_ROOT "
            "to point at a Steinberg SDK folder containing pluginterfaces/vst2.x/aeffect.h")
    endif()

    set(INTEL_VST_WRAPPER_VST2_COMPAT_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/vst2-compat")
    file(MAKE_DIRECTORY "${INTEL_VST_WRAPPER_VST2_COMPAT_INCLUDE}/pluginterfaces")
    file(COPY "${INTEL_VST_WRAPPER_VST2_SDK_ROOT}/pluginterfaces/vst2.x"
         DESTINATION "${INTEL_VST_WRAPPER_VST2_COMPAT_INCLUDE}/pluginterfaces")
    set(INTEL_VST_WRAPPER_VST2_HOST_VALUE 1)
endif()

juce_add_gui_app(IntelVSTBridgeHelper
    PRODUCT_NAME "IntelVSTBridgeHelper"
)

target_sources(IntelVSTBridgeHelper PRIVATE
    ../Source/HelperMain.cpp
    ../Source/AvailabilityVersionCheckShim.cpp
)

set_source_files_properties(../Source/HelperMain.cpp PROPERTIES LANGUAGE OBJCXX)

target_compile_features(IntelVSTBridgeHelper PRIVATE cxx_std_17)

target_compile_definitions(IntelVSTBridgeHelper PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_VST=${INTEL_VST_WRAPPER_VST2_HOST_VALUE}
    JUCE_PLUGINHOST_VST3=1
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_REPORT_APP_USAGE=0
    INTEL_VST_WRAPPER_ENABLE_HOSTED_UI=1
)

target_include_directories(IntelVSTBridgeHelper PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../Source"
)

if(EXISTS "${INTEL_VST_WRAPPER_LEGACY_LIBCXX_INCLUDE}/algorithm")
    target_compile_options(IntelVSTBridgeHelper PRIVATE
        "-isystem" "${INTEL_VST_WRAPPER_LEGACY_LIBCXX_INCLUDE}")
endif()

if(INTEL_VST_WRAPPER_ENABLE_VST2)
    target_include_directories(IntelVSTBridgeHelper PRIVATE "${INTEL_VST_WRAPPER_VST2_COMPAT_INCLUDE}")
endif()

target_link_libraries(IntelVSTBridgeHelper PRIVATE
    juce::juce_audio_processors
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
    "-framework CoreAudioKit"
)

add_custom_command(TARGET IntelVSTBridgeHelper POST_BUILD
    COMMAND /usr/libexec/PlistBuddy
            -c "Set :NSHighResolutionCapable false"
            "$<TARGET_BUNDLE_DIR:IntelVSTBridgeHelper>/Contents/Info.plist"
    VERBATIM)
