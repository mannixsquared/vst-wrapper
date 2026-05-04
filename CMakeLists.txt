cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(IntelVSTWrapper VERSION 0.1.0 LANGUAGES C CXX)

if(UNIX AND NOT APPLE)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

if(APPLE)
    enable_language(OBJCXX)
endif()

include(FetchContent)

set(INTEL_VST_WRAPPER_DEFAULT_PLUGIN "" CACHE STRING "Optional absolute path to the Intel VST/VST3 to host")
option(INTEL_VST_WRAPPER_ENABLE_VST2 "Enable VST2 hosting in the Rosetta helper. Requires Steinberg VST2 SDK headers." OFF)
option(INTEL_VST_WRAPPER_ENABLE_HOSTED_UI "Experimental: open the hosted Intel plugin editor in the helper process." OFF)
option(INTEL_VST_WRAPPER_USE_LEGACY_HELPER "Build the Rosetta helper in a separate JUCE 6 project for legacy VST UI compatibility." OFF)
set(INTEL_VST_WRAPPER_VST2_SDK_ROOT "" CACHE PATH "Folder containing pluginterfaces/vst2.x/aeffect.h")
set(INTEL_VST_WRAPPER_LEGACY_JUCE_ROOT "" CACHE PATH "Optional JUCE 6 checkout for the legacy helper")
set(INTEL_VST_WRAPPER_LEGACY_MACOS_SDK "" CACHE PATH "macOS 10.13 SDK path for the legacy x86_64 helper")
set(INTEL_VST_WRAPPER_LEGACY_DEPLOYMENT_TARGET "10.13" CACHE STRING "Deployment target for the legacy x86_64 helper")
set(INTEL_VST_WRAPPER_LEGACY_LIBCXX_INCLUDE "/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk/usr/include/c++/v1" CACHE PATH "Modern libc++ headers used when compiling the legacy helper with an old macOS SDK")

if(DEFINED JUCE_ROOT)
    set(JUCE_ROOT "${JUCE_ROOT}" CACHE PATH "Path to a JUCE checkout")
elseif(DEFINED ENV{JUCE_ROOT})
    set(JUCE_ROOT "$ENV{JUCE_ROOT}" CACHE PATH "Path to a JUCE checkout")
elseif(DEFINED JUCE_DIR)
    set(JUCE_ROOT "${JUCE_DIR}" CACHE PATH "Path to a JUCE checkout")
endif()

if(DEFINED JUCE_ROOT AND EXISTS "${JUCE_ROOT}/CMakeLists.txt")
    set(INTEL_VST_WRAPPER_JUCE_SOURCE_DIR "${JUCE_ROOT}")
    add_subdirectory("${JUCE_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/JUCE")
else()
    message(STATUS "JUCE_ROOT was not provided or is not a modern JUCE checkout; fetching JUCE into the build directory")
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        501c07674e1ad693085a7e7c398f205c2677f5da
        GIT_PROGRESS   TRUE
        UPDATE_DISCONNECTED TRUE)
    FetchContent_MakeAvailable(JUCE)
    set(INTEL_VST_WRAPPER_JUCE_SOURCE_DIR "${juce_SOURCE_DIR}")
endif()

option(INTEL_VST_WRAPPER_BUILD_PLUGIN_HOST "Build JUCE AudioPluginHost for local plugin testing" OFF)

if(INTEL_VST_WRAPPER_BUILD_PLUGIN_HOST)
    if(EXISTS "${INTEL_VST_WRAPPER_JUCE_SOURCE_DIR}/extras/AudioPluginHost/CMakeLists.txt")
        add_subdirectory("${INTEL_VST_WRAPPER_JUCE_SOURCE_DIR}/extras/AudioPluginHost"
                         "${CMAKE_CURRENT_BINARY_DIR}/AudioPluginHost")
    else()
        message(FATAL_ERROR "AudioPluginHost source was not found in JUCE checkout: ${INTEL_VST_WRAPPER_JUCE_SOURCE_DIR}")
    endif()
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

set(INTEL_VST_WRAPPER_FORMATS VST3)

if(APPLE)
    list(APPEND INTEL_VST_WRAPPER_FORMATS AU)
endif()

juce_add_plugin(IntelVSTWrapper
    VERSION                         ${PROJECT_VERSION}
    COMPANY_NAME                    "Mannix Squared"
    COMPANY_WEBSITE                 "http://mannixsquared.com"
    COMPANY_COPYRIGHT               "(c)2026, Mannix Squared"
    BUNDLE_ID                       "com.mannixsquared.intelvstwrapper"
    PLUGIN_MANUFACTURER_CODE        Mann
    PLUGIN_CODE                     Ivwr
    FORMATS                         ${INTEL_VST_WRAPPER_FORMATS}
    PRODUCT_NAME                    "Intel VST Wrapper"
    PLUGIN_NAME                     "Intel VST Wrapper"
    DESCRIPTION                     "Apple Silicon wrapper for Intel VST plugins"
    VST3_CATEGORIES                 Fx
    IS_SYNTH                        FALSE
    NEEDS_MIDI_INPUT                TRUE
    NEEDS_MIDI_OUTPUT               TRUE
    IS_MIDI_EFFECT                  FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS     FALSE
    COPY_PLUGIN_AFTER_BUILD         FALSE
)

juce_generate_juce_header(IntelVSTWrapper)

target_sources(IntelVSTWrapper PRIVATE
    Source/WrapperPlugin.cpp
)

target_compile_features(IntelVSTWrapper PRIVATE cxx_std_17)

target_compile_definitions(IntelVSTWrapper PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_REPORT_APP_USAGE=0
    INTEL_VST_WRAPPER_DEFAULT_PLUGIN="${INTEL_VST_WRAPPER_DEFAULT_PLUGIN}"
)

target_link_libraries(IntelVSTWrapper PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

if (APPLE)
    set_target_properties(IntelVSTWrapper PROPERTIES OSX_ARCHITECTURES "arm64;x86_64")
endif()

if(INTEL_VST_WRAPPER_USE_LEGACY_HELPER)
    if(NOT APPLE)
        message(FATAL_ERROR "INTEL_VST_WRAPPER_USE_LEGACY_HELPER is only supported on macOS")
    endif()

    if(NOT INTEL_VST_WRAPPER_ENABLE_HOSTED_UI)
        message(FATAL_ERROR "The legacy helper path is intended for hosted UI builds")
    endif()

    if(NOT EXISTS "${INTEL_VST_WRAPPER_LEGACY_MACOS_SDK}/SDKSettings.plist")
        message(FATAL_ERROR "INTEL_VST_WRAPPER_LEGACY_MACOS_SDK must point at an extracted macOS 10.13 SDK")
    endif()

    include(ExternalProject)

    set(INTEL_VST_WRAPPER_LEGACY_HELPER_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/LegacyHelper")
    set(INTEL_VST_WRAPPER_HELPER_BUNDLE_DIR
        "${INTEL_VST_WRAPPER_LEGACY_HELPER_BINARY_DIR}/IntelVSTBridgeHelper_artefacts/IntelVSTBridgeHelper.app")
    set(INTEL_VST_WRAPPER_HELPER_EXECUTABLE
        "${INTEL_VST_WRAPPER_HELPER_BUNDLE_DIR}/Contents/MacOS/IntelVSTBridgeHelper")

    set(INTEL_VST_WRAPPER_LEGACY_HELPER_ARGS
        -DINTEL_VST_WRAPPER_ENABLE_VST2=${INTEL_VST_WRAPPER_ENABLE_VST2}
        -DINTEL_VST_WRAPPER_VST2_SDK_ROOT=${INTEL_VST_WRAPPER_VST2_SDK_ROOT}
        -DINTEL_VST_WRAPPER_LEGACY_MACOS_SDK=${INTEL_VST_WRAPPER_LEGACY_MACOS_SDK}
        -DINTEL_VST_WRAPPER_LEGACY_LIBCXX_INCLUDE=${INTEL_VST_WRAPPER_LEGACY_LIBCXX_INCLUDE}
        -DCMAKE_OSX_SYSROOT=${INTEL_VST_WRAPPER_LEGACY_MACOS_SDK}
        -DCMAKE_OSX_DEPLOYMENT_TARGET=${INTEL_VST_WRAPPER_LEGACY_DEPLOYMENT_TARGET}
        -DCMAKE_OSX_ARCHITECTURES=x86_64)

    if(INTEL_VST_WRAPPER_LEGACY_JUCE_ROOT)
        list(APPEND INTEL_VST_WRAPPER_LEGACY_HELPER_ARGS
             -DJUCE_ROOT=${INTEL_VST_WRAPPER_LEGACY_JUCE_ROOT})
    endif()

    ExternalProject_Add(IntelVSTBridgeHelperExternal
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/HelperLegacy"
        BINARY_DIR "${INTEL_VST_WRAPPER_LEGACY_HELPER_BINARY_DIR}"
        CMAKE_ARGS ${INTEL_VST_WRAPPER_LEGACY_HELPER_ARGS}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target IntelVSTBridgeHelper --config $<CONFIG>
        INSTALL_COMMAND ""
        BUILD_ALWAYS TRUE
        BUILD_BYPRODUCTS "${INTEL_VST_WRAPPER_HELPER_EXECUTABLE}")

    add_custom_target(IntelVSTBridgeHelper DEPENDS IntelVSTBridgeHelperExternal)
else()
    if(INTEL_VST_WRAPPER_ENABLE_HOSTED_UI)
        juce_add_gui_app(IntelVSTBridgeHelper
            PRODUCT_NAME "IntelVSTBridgeHelper"
        )
    else()
        juce_add_console_app(IntelVSTBridgeHelper
            PRODUCT_NAME "IntelVSTBridgeHelper"
        )
    endif()

    target_sources(IntelVSTBridgeHelper PRIVATE
        Source/HelperMain.cpp
        Source/AvailabilityVersionCheckShim.cpp
    )

    if(APPLE AND INTEL_VST_WRAPPER_ENABLE_HOSTED_UI)
        set_source_files_properties(Source/HelperMain.cpp PROPERTIES LANGUAGE OBJCXX)
    endif()

    target_compile_features(IntelVSTBridgeHelper PRIVATE cxx_std_17)

    target_compile_definitions(IntelVSTBridgeHelper PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_PLUGINHOST_AU=1
        JUCE_PLUGINHOST_VST=${INTEL_VST_WRAPPER_VST2_HOST_VALUE}
        JUCE_PLUGINHOST_VST3=1
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_REPORT_APP_USAGE=0
        INTEL_VST_WRAPPER_ENABLE_HOSTED_UI=$<BOOL:${INTEL_VST_WRAPPER_ENABLE_HOSTED_UI}>
    )

    if(INTEL_VST_WRAPPER_ENABLE_VST2)
        target_include_directories(IntelVSTBridgeHelper PRIVATE "${INTEL_VST_WRAPPER_VST2_COMPAT_INCLUDE}")
    endif()

    target_link_libraries(IntelVSTBridgeHelper PRIVATE
        juce::juce_audio_processors
        juce::juce_gui_basics
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    )

    if (APPLE)
        set_target_properties(IntelVSTBridgeHelper PROPERTIES OSX_ARCHITECTURES "x86_64")
        target_link_libraries(IntelVSTBridgeHelper PRIVATE "-framework CoreAudioKit")
    endif()

    if(APPLE AND INTEL_VST_WRAPPER_ENABLE_HOSTED_UI)
        add_custom_command(TARGET IntelVSTBridgeHelper POST_BUILD
            COMMAND /usr/libexec/PlistBuddy
                    -c "Set :NSHighResolutionCapable false"
                    "$<TARGET_BUNDLE_DIR:IntelVSTBridgeHelper>/Contents/Info.plist"
            VERBATIM)
    endif()

    set(INTEL_VST_WRAPPER_HELPER_BUNDLE_DIR "$<TARGET_BUNDLE_DIR:IntelVSTBridgeHelper>")
    set(INTEL_VST_WRAPPER_HELPER_EXECUTABLE "$<TARGET_FILE:IntelVSTBridgeHelper>")
endif()

juce_add_console_app(IntelVSTWrapperSmokeHost
    PRODUCT_NAME "IntelVSTWrapperSmokeHost"
)

target_sources(IntelVSTWrapperSmokeHost PRIVATE
    Source/SmokeHostMain.cpp
)

target_compile_features(IntelVSTWrapperSmokeHost PRIVATE cxx_std_17)

target_compile_definitions(IntelVSTWrapperSmokeHost PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_VST=${INTEL_VST_WRAPPER_VST2_HOST_VALUE}
    JUCE_PLUGINHOST_VST3=1
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_REPORT_APP_USAGE=0
)

if(INTEL_VST_WRAPPER_ENABLE_VST2)
    target_include_directories(IntelVSTWrapperSmokeHost PRIVATE "${INTEL_VST_WRAPPER_VST2_COMPAT_INCLUDE}")
endif()

target_link_libraries(IntelVSTWrapperSmokeHost PRIVATE
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

function(intel_vst_wrapper_copy_helper format_target)
    if(TARGET ${format_target})
        add_dependencies(${format_target} IntelVSTBridgeHelper)
        add_custom_command(TARGET ${format_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_BUNDLE_DIR:${format_target}>/Contents/Resources"
            COMMAND ${CMAKE_COMMAND} -E rm -rf
                    "$<TARGET_BUNDLE_DIR:${format_target}>/Contents/Resources/IntelVSTBridgeHelper.app"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${INTEL_VST_WRAPPER_HELPER_BUNDLE_DIR}"
                    "$<TARGET_BUNDLE_DIR:${format_target}>/Contents/Resources/IntelVSTBridgeHelper.app"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${INTEL_VST_WRAPPER_HELPER_EXECUTABLE}"
                    "$<TARGET_BUNDLE_DIR:${format_target}>/Contents/Resources/IntelVSTBridgeHelper"
            VERBATIM)
    endif()
endfunction()

intel_vst_wrapper_copy_helper(IntelVSTWrapper_VST3)
intel_vst_wrapper_copy_helper(IntelVSTWrapper_AU)

if(APPLE)
    add_custom_target(IntelVSTWrapperCopyHelper ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_CURRENT_BINARY_DIR}/IntelVSTWrapper_artefacts/VST3/Intel VST Wrapper.vst3/Contents/Resources"
        COMMAND ${CMAKE_COMMAND} -E rm -rf
                "${CMAKE_CURRENT_BINARY_DIR}/IntelVSTWrapper_artefacts/VST3/Intel VST Wrapper.vst3/Contents/Resources/IntelVSTBridgeHelper.app"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${INTEL_VST_WRAPPER_HELPER_BUNDLE_DIR}"
                "${CMAKE_CURRENT_BINARY_DIR}/IntelVSTWrapper_artefacts/VST3/Intel VST Wrapper.vst3/Contents/Resources/IntelVSTBridgeHelper.app"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${INTEL_VST_WRAPPER_HELPER_EXECUTABLE}"
                "${CMAKE_CURRENT_BINARY_DIR}/IntelVSTWrapper_artefacts/VST3/Intel VST Wrapper.vst3/Contents/Resources/IntelVSTBridgeHelper"
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_CURRENT_BINARY_DIR}/IntelVSTWrapper_artefacts/AU/Intel VST Wrapper.component/Contents/Resources"
        COMMAND ${CMAKE_COMMAND} -E rm -rf
                "${CMAKE_CURRENT_BINARY_DIR}/IntelVSTWrapper_artefacts/AU/Intel VST Wrapper.component/Contents/Resources/IntelVSTBridgeHelper.app"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${INTEL_VST_WRAPPER_HELPER_BUNDLE_DIR}"
                "${CMAKE_CURRENT_BINARY_DIR}/IntelVSTWrapper_artefacts/AU/Intel VST Wrapper.component/Contents/Resources/IntelVSTBridgeHelper.app"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${INTEL_VST_WRAPPER_HELPER_EXECUTABLE}"
                "${CMAKE_CURRENT_BINARY_DIR}/IntelVSTWrapper_artefacts/AU/Intel VST Wrapper.component/Contents/Resources/IntelVSTBridgeHelper"
        DEPENDS IntelVSTBridgeHelper IntelVSTWrapper_VST3 IntelVSTWrapper_AU
        VERBATIM)
endif()
