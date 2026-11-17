cmake_minimum_required(VERSION 3.22)
project(Ravenyr VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FF_BUILD_TESTS  "Build the unit test runner" ON)
option(FF_BUILD_APP    "Build the GUI application" ON)
option(FF_BUILD_TEST_PLUGIN "Build a tiny VST3 effect used to test plugin hosting (FF_TEST_VST3=<path> runs the tests)" OFF)
option(FF_ENABLE_ASIO  "Enable ASIO on Windows (requires the Steinberg ASIO SDK)" OFF)
set(FF_ASIO_SDK_DIR "" CACHE PATH "Path to the ASIO SDK root (contains common/iasiodrv.h)")
# VST2 hosting needs Steinberg's VST 2 SDK, which is not open source and cannot be shipped with RAVENYR.
# If you hold a VST 2 licence, point this at the SDK folder (the one containing pluginterfaces/vst2.x/aeffect.h).
set(FF_VST2_SDK_DIR "" CACHE PATH "Path to the VST 2 SDK (enables VST2 plugin hosting)")
set(FF_JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/JUCE" CACHE PATH "Local JUCE checkout (fetched automatically if missing)")

# ---------------------------------------------------------------- JUCE
if(EXISTS "${FF_JUCE_DIR}/CMakeLists.txt")
    add_subdirectory("${FF_JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.15
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)
endif()

set(FF_COMMON_DEFS
    JUCE_WEB_BROWSER=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_LV2=1
    JUCE_MODAL_LOOPS_PERMITTED=0
    FF_VERSION_STRING="${PROJECT_VERSION}")

# JACK/ALSA only on Linux (newer JUCE can also build JACK on macOS/Windows, which needs the JACK SDK)
if(UNIX AND NOT APPLE)
    # libcurl gives juce::URL https on Linux (TONE3000 browser); macOS/Windows use the system networking
    find_package(CURL REQUIRED)
    list(APPEND FF_COMMON_DEFS JUCE_JACK=1 JUCE_ALSA=1 JUCE_USE_CURL=1)
else()
    list(APPEND FF_COMMON_DEFS JUCE_JACK=0)
endif()

if(FF_VST2_SDK_DIR)
    if(NOT EXISTS "${FF_VST2_SDK_DIR}/pluginterfaces/vst2.x/aeffect.h")
        message(FATAL_ERROR "FF_VST2_SDK_DIR does not contain pluginterfaces/vst2.x/aeffect.h")
    endif()
    juce_set_vst2_sdk_path ("${FF_VST2_SDK_DIR}")
    list(APPEND FF_COMMON_DEFS JUCE_PLUGINHOST_VST=1)
    message(STATUS "VST2 hosting enabled with the SDK in ${FF_VST2_SDK_DIR}")
endif()

if(FF_ENABLE_ASIO AND WIN32)
    if(NOT EXISTS "${FF_ASIO_SDK_DIR}/common/iasiodrv.h")
        message(FATAL_ERROR "FF_ENABLE_ASIO=ON but FF_ASIO_SDK_DIR does not point to the ASIO SDK")
    endif()
    list(APPEND FF_COMMON_DEFS JUCE_ASIO=1)
endif()

# ---------------------------------------------------------------- Neural Amp Modeler core (vendored, MIT)
file(GLOB FF_NAM_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nam/NAM/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nam/NAM/wavenet/*.cpp")
add_library(ff_nam STATIC ${FF_NAM_SOURCES})
target_include_directories(ff_nam SYSTEM PUBLIC
    third_party/nam
    third_party/nam/nlohmann
    third_party/eigen)
target_compile_definitions(ff_nam PUBLIC NAM_SAMPLE_FLOAT EIGEN_MPL2_ONLY PRIVATE NAM_ENABLE_A2_FAST)
if(WIN32)
    target_compile_definitions(ff_nam PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
    target_compile_options(ff_nam PRIVATE /W0 /bigobj /O2)
else()
    target_compile_options(ff_nam PRIVATE -w -O2)
endif()
set_target_properties(ff_nam PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------- core + engine (static, UI-free)
add_library(ff_core STATIC)
target_sources(ff_core PRIVATE
    src/core/MusicTheory.cpp
    src/core/PitchDetector.cpp
    src/core/MetronomeDsp.cpp
    src/core/ProjectFile.cpp
    src/core/Timeline.cpp
    src/core/MidiClip.cpp
    src/core/KeysGen.cpp
    src/core/Automation.cpp
    src/core/TabModel.cpp
    src/core/Exercises.cpp
    src/core/GuitarPro.cpp
    src/core/MusicXml.cpp
    src/core/DrumModel.cpp
    src/core/RigModel.cpp
    src/engine/fx/NamModel.cpp
    src/engine/fx/AmpRig.cpp
    src/engine/fx/RigBank.cpp
    src/engine/fx/PluginHost.cpp
    src/engine/fx/InsertChain.cpp
    src/engine/fx/BuiltinPlugins.cpp
    src/engine/TabSynth.cpp
    src/engine/DrumSynth.cpp
    src/engine/AudioCache.cpp
    src/engine/Recorder.cpp
    src/engine/PlaybackGraph.cpp
    src/engine/GraphMixer.cpp
    src/engine/ext/HostApi.cpp
    src/engine/TrackWorkers.cpp
    src/engine/Exporter.cpp
    src/engine/AudioEngine.cpp
    src/engine/TunerAnalyzer.cpp)
target_include_directories(ff_core PUBLIC src)
target_compile_definitions(ff_core PUBLIC ${FF_COMMON_DEFS} JUCE_STANDALONE_APPLICATION=1)
if(FF_ENABLE_ASIO AND WIN32)
    target_include_directories(ff_core PUBLIC "${FF_ASIO_SDK_DIR}/common")
endif()
target_link_libraries(ff_core
    PUBLIC
        ff_nam
        $<$<AND:$<PLATFORM_ID:Linux>,$<BOOL:${CURL_FOUND}>>:CURL::libcurl>
        juce::juce_core
        juce::juce_data_structures
        juce::juce_events
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_dsp
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
set_target_properties(ff_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(FF_VST2_SDK_DIR)
    target_include_directories(ff_core PUBLIC "${FF_VST2_SDK_DIR}")
endif()
if(APPLE)
    # Audio Unit hosting (generic AU editor views) lives in CoreAudioKit
    target_link_libraries(ff_core PUBLIC "-framework CoreAudioKit")
endif()

# ---------------------------------------------------------------- GUI application
if(FF_BUILD_APP)
juce_add_gui_app(Ravenyr
    PRODUCT_NAME "RAVENYR"
    COMPANY_NAME "Sacred Hills Studio"
    BUNDLE_ID "com.ravenyr.app"
    PLUGINHOST_AU TRUE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "RAVENYR needs audio input to tune and record your instrument."
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.png"
    VERSION ${PROJECT_VERSION})

# plugins look up the host extension API (src/engine/ext/ravenyr_host_api.h) in the executable by name. Linux:
# export only that symbol (exporting everything would let the app's JUCE clash with a plugin's own copy).
if(UNIX AND NOT APPLE)
    target_link_options(Ravenyr PRIVATE "LINKER:--dynamic-list=${CMAKE_CURRENT_SOURCE_DIR}/src/engine/ext/host_api.dynlist")
endif()

# Windows: JUCE's generated icon.ico only has 16/32/48/256 px images, so Windows scales them for desktop icons at
# 125-200 % display scaling (blurry). Replace it with packaging/ravenyr.ico (16-256 px, each sized and sharpened).
if(WIN32)
    get_target_property(ff_generated_ico Ravenyr JUCE_ICON_FILE)
    if(ff_generated_ico)
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/packaging/ravenyr.ico" "${ff_generated_ico}" COPYONLY)
    endif()
endif()

# the logo and the heading font (Cinzel, SIL Open Font License: resources/Cinzel-OFL.txt)
juce_add_binary_data(ff_resources SOURCES resources/ravenyr-logo.png resources/Cinzel-Bold.ttf)

target_sources(Ravenyr PRIVATE
    src/app/Main.cpp
    src/app/MainComponent.cpp
    src/app/WinElevation.cpp
    src/app/FloatAboveApp.cpp
    src/app/GlobalKeys.cpp
    src/app/StudioLookAndFeel.cpp
    src/app/ProjectDocument.cpp
    src/app/EngineHost.cpp
    src/app/TransportBar.cpp
    src/app/SessionController.cpp
    src/app/ExportDialog.cpp
    src/app/pages/arrange/ArrangeView.cpp
    src/app/pages/arrange/PianoRoll.cpp
    src/app/pages/tabs/TabEditor.cpp
    src/app/pages/tabs/NotationGlyphs.cpp
    src/app/pages/tabs/ScorePdf.cpp
    src/app/pages/drums/DrumPage.cpp
    src/app/pages/keys/KeysPage.cpp
    src/app/pages/training/TrainingPage.cpp
    src/app/pages/fx/RigEditor.cpp
    src/app/tone3000/Tone3000.cpp
    src/app/tone3000/Tone3000Panel.cpp
    src/app/plugins/PluginManager.cpp
    src/app/pages/tabs/TabPage.cpp
    src/app/pages/StudioPage.cpp
    src/app/pages/TunerPanel.cpp
    src/app/pages/MetronomePanel.cpp
    src/app/pages/MixerPage.cpp
    src/app/pages/SettingsPage.cpp
    src/app/pages/MilestonePage.cpp)

target_link_libraries(Ravenyr PRIVATE
    ff_core
    ff_resources
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_audio_utils
    juce::juce_cryptography
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)
endif()

# ---------------------------------------------------------------- tests
if(FF_BUILD_TESTS)
    juce_add_console_app(ff_tests PRODUCT_NAME "ff_tests")
    target_sources(ff_tests PRIVATE
        tests/TestMain.cpp
        tests/MusicTheoryTests.cpp
        tests/PitchDetectorTests.cpp
        tests/MetronomeTests.cpp
        tests/TempoMapTests.cpp
        tests/MasterAutomationTests.cpp
        tests/ExerciseTests.cpp
        tests/ProjectFileTests.cpp
        tests/AudioEngineTests.cpp
        tests/TimelineTests.cpp
        tests/ArrangementEngineTests.cpp
        tests/TabTests.cpp
        tests/GuitarProTests.cpp
        tests/MusicXmlTests.cpp
        tests/DrumTests.cpp
        tests/KeysGenTests.cpp
        tests/AmpRigTests.cpp
        tests/RoutingTests.cpp
        tests/BuiltinPluginTests.cpp
        tests/MidiAutomationTests.cpp
        tests/PerfProbe.cpp)
    target_link_libraries(ff_tests PRIVATE ff_core juce::juce_recommended_config_flags)
    target_compile_definitions(ff_tests PRIVATE FF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    enable_testing()
    add_test(NAME unit COMMAND ff_tests)
endif()

# ---------------------------------------------------------------- packaging
# Installers: Windows NSIS setup .exe (+ portable zip), macOS drag-to-Applications .dmg, Linux .deb / .rpm (+ .tar.gz).
if(FF_BUILD_APP)
    install(TARGETS Ravenyr BUNDLE DESTINATION . RUNTIME DESTINATION bin)
    if(UNIX AND NOT APPLE)
        install(FILES packaging/ravenyr.desktop DESTINATION share/applications)
        foreach(px 48 128 256 512)
            install(FILES packaging/ravenyr-${px}.png DESTINATION share/icons/hicolor/${px}x${px}/apps RENAME ravenyr.png)
        endforeach()
    endif()
endif()
set(CPACK_PACKAGE_NAME "Ravenyr")
set(CPACK_PACKAGE_VENDOR "Sacred Hills Studio")
set(CPACK_PACKAGE_CONTACT "Sacred Hills Studio")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "RAVENYR STUDIO - guitar workstation: recording, mixing, amps, tabs, drums and training")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/SacredHills/Fretforge")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "RAVENYR STUDIO")
set(CPACK_PACKAGE_EXECUTABLES "RAVENYR" "RAVENYR STUDIO")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/LICENSE.txt")
if(WIN32)
    set(CPACK_GENERATOR "NSIS;ZIP")
    set(CPACK_NSIS_PACKAGE_NAME "RAVENYR STUDIO")
    set(CPACK_NSIS_DISPLAY_NAME "RAVENYR STUDIO")
    set(CPACK_NSIS_PUBLISHER "Sacred Hills Studio")
    set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/SacredHills/Fretforge")
    set(CPACK_NSIS_MUI_ICON "${CMAKE_CURRENT_SOURCE_DIR}/packaging/ravenyr.ico")
    set(CPACK_NSIS_MUI_UNIICON "${CMAKE_CURRENT_SOURCE_DIR}/packaging/ravenyr.ico")
    file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/packaging/nsis-side.bmp" FF_NSIS_SIDE)
    file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/packaging/nsis-header.bmp" FF_NSIS_HEADER)
    string(REPLACE "\\" "\\\\" FF_NSIS_SIDE "${FF_NSIS_SIDE}")
    string(REPLACE "\\" "\\\\" FF_NSIS_HEADER "${FF_NSIS_HEADER}")
    set(CPACK_NSIS_MUI_WELCOMEFINISHPAGE_BITMAP "${FF_NSIS_SIDE}")
    set(CPACK_NSIS_MUI_UNWELCOMEFINISHPAGE_BITMAP "${FF_NSIS_SIDE}")
    set(CPACK_NSIS_MUI_HEADERIMAGE "${FF_NSIS_HEADER}")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\RAVENYR.exe")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    # The options page (desktop shortcut checkbox) only shows with MODIFY_PATH on; packaging/NSIS.InstallOptions.ini.in
    # replaces CPack's page so it only asks about the desktop shortcut and never touches PATH.
    set(CPACK_NSIS_MODIFY_PATH ON)
    list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/packaging")
    set(CPACK_NSIS_MUI_FINISHPAGE_RUN "RAVENYR.exe")
    set(CPACK_NSIS_EXECUTABLES_DIRECTORY "bin")
    set(CPACK_CREATE_DESKTOP_LINKS "RAVENYR")
    set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "RAVENYR STUDIO")
    set(CPACK_DMG_FORMAT "UDZO")
    set(CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE OFF)   # no license prompt when the .dmg opens
else()
    set(CPACK_GENERATOR "TGZ;DEB;RPM")
    set(CPACK_STRIP_FILES ON)
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Sacred Hills Studio")
    set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libasound2t64 | libasound2, libfreetype6, libfontconfig1, libx11-6, libcurl4t64 | libcurl4")
    set(CPACK_DEBIAN_PACKAGE_SUGGESTS "libjack-jackd2-0 | pipewire-jack")
    set(CPACK_RPM_PACKAGE_LICENSE "Proprietary")
    set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
    set(CPACK_RPM_PACKAGE_AUTOREQ OFF)
    set(CPACK_RPM_PACKAGE_REQUIRES "alsa-lib, freetype, fontconfig, libX11, libcurl")
    set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION /usr/share/applications /usr/share/icons /usr/share/icons/hicolor)
    foreach(px 48 128 256 512)
        list(APPEND CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION /usr/share/icons/hicolor/${px}x${px} /usr/share/icons/hicolor/${px}x${px}/apps)
    endforeach()
endif()
include(CPack)

# ---------------------------------------------------------------- test plugin (hosting tests only)
if(FF_BUILD_TEST_PLUGIN)
    juce_add_plugin(ff_test_plugin
        PRODUCT_NAME "FF Test Gain"
        COMPANY_NAME "Sacred Hills Studio"
        BUNDLE_ID "com.ravenyr.fftestgain"
        PLUGIN_MANUFACTURER_CODE Frfg
        PLUGIN_CODE Fftg
        FORMATS VST3
        IS_SYNTH FALSE NEEDS_MIDI_INPUT FALSE NEEDS_MIDI_OUTPUT FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE)
    target_sources(ff_test_plugin PRIVATE tests/plugin/TestGainPlugin.cpp)
    target_compile_definitions(ff_test_plugin PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
                               JUCE_DISPLAY_SPLASH_SCREEN=0)
    target_link_libraries(ff_test_plugin PRIVATE juce::juce_audio_utils juce::juce_recommended_config_flags)
endif()
