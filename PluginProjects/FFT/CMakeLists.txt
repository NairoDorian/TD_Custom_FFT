# {'plugin_type': 'CHOP'}
cmake_minimum_required(VERSION 3.21)
project(FFT LANGUAGES CXX)

# ---------------------------------------------------------------------------
# PluginBuilder shared module (output layout, SDK headers, MSVC flags, deploy,
# dependencies, tests). PLUGIN_BUILDER_DIR is passed by PluginBuilder (-D) or
# taken from the environment; the fallback is the sibling checkout.
# ---------------------------------------------------------------------------
if(NOT PLUGIN_BUILDER_DIR AND NOT DEFINED ENV{PLUGIN_BUILDER_DIR})
    get_filename_component(PLUGIN_BUILDER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../PluginBuilder_V2" ABSOLUTE)
endif()
if(NOT PLUGIN_BUILDER_DIR)
    set(PLUGIN_BUILDER_DIR "$ENV{PLUGIN_BUILDER_DIR}")
endif()
if(NOT EXISTS "${PLUGIN_BUILDER_DIR}/cmake/TDPlugin.cmake")
    message(FATAL_ERROR "PluginBuilder not found at '${PLUGIN_BUILDER_DIR}'. Pass -DPLUGIN_BUILDER_DIR=<path to PluginBuilder_V2>.")
endif()
include(${PLUGIN_BUILDER_DIR}/cmake/TDPlugin.cmake)

# CTest only writes a CTestTestfile.cmake for directories where enable_testing() ran in the TOP-LEVEL
# scope; td_plugin_add_test() calls it from inside a function, which does not take effect here, so
# `ctest --test-dir build` found nothing. Enabling it at the top level makes the documented command work.
enable_testing()

# ---------------------------------------------------------------------------
# The plugin
# ---------------------------------------------------------------------------
td_add_plugin(FFT FAMILY CHOP)
td_plugin_use_fftw3(FFT)                     # vendored 3rdParty/fftw3 (libfftw3f-3), runtime DLL deployed automatically
td_plugin_optimize(FFT AVX2 FAST_MATH)       # /O2 /Oi /Ot /fp:fast /arch:AVX2 (+ /OPT:REF,ICF)

# ---------------------------------------------------------------------------
# Headless tests & benchmark (no TouchDesigner needed):
#   ninja -C build && ctest --test-dir build --output-on-failure
#   build/bin/Release/fft_bench.exe --channels 8 --fft 32768
# ---------------------------------------------------------------------------
option(FFT_BUILD_TESTS "Build the headless DSP unit tests" ON)
option(FFT_BUILD_BENCH "Build the per-stage benchmark executable" ON)
if(FFT_BUILD_TESTS)
    td_plugin_add_test(FFT fft_tests SOURCES tests/dsp_tests.cpp)
endif()
if(FFT_BUILD_BENCH)
    td_plugin_add_bench(FFT fft_bench SOURCES bench/bench.cpp)
    # Answers "would FFTW's built-in threading help?" with numbers (it does not for Mono Mix).
    td_plugin_add_bench(FFT fft_threads_probe SOURCES bench/fftw_threads_probe.cpp)
endif()
