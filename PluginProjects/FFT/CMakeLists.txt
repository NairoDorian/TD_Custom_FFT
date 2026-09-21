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

# Vendored FFTW 3.3.11 built from source with AVX2+FMA codelets; see 3rdParty/fftw3/README.md and
# 3rdParty/fftw3/VERSION for the provenance of every artifact. VERSION pins the exact tag, so the
# library, the header and the deployed runtime DLL all carry the version in their names and a second
# FFTW can be dropped in beside this one without ambiguity.
#
# DYNAMIC: the plugin does NOT link this import library. fftwf_* is resolved at run time (LoadLibrary
# + GetProcAddress) because the FFT Backend toggle can also select Intel oneMKL's FFTW3 interface, and
# that library exports the very same symbol names - linking one of them would make the other
# unreachable. The DLL is still copied next to the plugin, so the default backend works out of the box.
td_plugin_use_fftw3(FFT VERSION 3.3.11-avx2 DYNAMIC)

# /O2 /Oi /Ot /Gy /arch:AVX2 /fp:fast, plus LTO: the AVX2 intrinsics in DSPModules.h are only worth
# what the inliner does with them, and /GL lets the inliner work across FFT.cpp / AnalysisPipeline.cpp
# / Parameters.cpp instead of stopping at each translation unit.
td_plugin_optimize(FFT AVX2 FAST_MATH LTO)

# ---------------------------------------------------------------------------
# Headless tests & benchmark (no TouchDesigner needed):
#   ninja -C build && ctest --test-dir build --output-on-failure
#   build/bin/Release/fft_bench.exe --channels 8 --fft 32768
# ---------------------------------------------------------------------------
option(FFT_BUILD_TESTS "Build the headless DSP unit tests" ON)
option(FFT_BUILD_BENCH "Build the per-stage benchmark executable" ON)
if(FFT_BUILD_TESTS)
    td_plugin_add_test(FFT fft_tests SOURCES tests/dsp_tests.cpp source/AnalysisPipeline.cpp)
endif()
if(FFT_BUILD_BENCH)
    td_plugin_add_bench(FFT fft_bench SOURCES bench/bench.cpp)
    # Answers "would FFTW's built-in threading help?" with numbers (it does not for Mono Mix).
    td_plugin_add_bench(FFT fft_threads_probe SOURCES bench/fftw_threads_probe.cpp)
    # Reports which fftwf DLL is loaded, its version, whether that build has the threads API, which
    # SIMD codelets it actually selects, and the per-size r2c cost. Swapping the DLL next to this exe
    # is how two FFTW builds are compared; no such A/B is recorded in 3rdParty/fftw3/README.md (that
    # file keeps the FFTW3-vs-oneMKL comparison), so re-run the probe to take one.
    td_plugin_add_bench(FFT fft_version_probe SOURCES bench/fftw_version_probe.cpp)
endif()
