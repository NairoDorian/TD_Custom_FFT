# {'plugin_type': 'CHOP'}
cmake_minimum_required(VERSION 3.21)
project(FFT LANGUAGES CXX)

# ---------------------------------------------------------------------------
# PluginBuilder shared module (output layout, SDK headers, MSVC flags, deploy,
# dependencies, tests). PLUGIN_BUILDER_DIR is passed by PluginBuilder (-D) or
# taken from the environment; the fallback is the sibling checkout
# ../../../PluginBuilder_V2 (three levels up from this file: Plugin_FFT ->
# PluginBuilder -> PluginBuilder_V2's parent, then PluginBuilder_V2). The
# include below brings in cmake/TDPlugin.cmake, which DEFINES every td_* call
# used later in this file: td_add_plugin, td_plugin_use_fftw3,
# td_plugin_optimize, td_plugin_add_test and td_plugin_add_bench. This repo
# does not define them; it only calls them, so a missing or wrong
# PLUGIN_BUILDER_DIR fails here at configure time with the FATAL_ERROR below
# rather than at some later, less obvious point. Cross-repo by design: the
# toolchain lives in PluginBuilder_V2 and is OUT OF SCOPE for edits here -
# override the path with -DPLUGIN_BUILDER_DIR=... or the env var of the same
# name if the sibling checkout is elsewhere.
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

# (enable_testing() is now called by TDPlugin.cmake at directory scope, so td_plugin_add_test works
# without a local workaround.)

# ---------------------------------------------------------------------------
# The plugin
# ---------------------------------------------------------------------------
td_add_plugin(FFT FAMILY CHOP)

# The version lives in ONE place, plugin.json; the popup's `Plugin:` line and the Custom OP version are
# generated from it (FFT.cpp falls back to literals only when built without this CMake). A configure
# re-runs automatically when plugin.json changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json" _fft_manifest)
string(JSON FFT_VERSION GET "${_fft_manifest}" version)
string(REPLACE "." ";" _fft_ver "${FFT_VERSION}")
list(GET _fft_ver 0 FFT_VERSION_MAJOR)
list(GET _fft_ver 1 FFT_VERSION_MINOR)
list(GET _fft_ver 2 FFT_VERSION_PATCH)
target_compile_definitions(FFT PRIVATE FFT_VERSION_MAJOR=${FFT_VERSION_MAJOR} FFT_VERSION_MINOR=${FFT_VERSION_MINOR} FFT_VERSION_PATCH=${FFT_VERSION_PATCH})
message(STATUS "FFT plugin version ${FFT_VERSION} (plugin.json)")

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
# The test executable prints "N checks, M failures" as its last line; the
# baseline is 607 checks, 0 failures (see the header of tests/dsp_tests.cpp).
# That count is a coverage SIGNAL: it must not go down between revisions, and
# a comment-only change must leave it exactly where it is.
# ---------------------------------------------------------------------------
option(FFT_BUILD_TESTS "Build the headless DSP unit tests" ON)
option(FFT_BUILD_BENCH "Build the per-stage benchmark executable" ON)
if(FFT_BUILD_TESTS)
    td_plugin_add_test(FFT fft_tests SOURCES tests/dsp_tests.cpp source/AnalysisPipeline.cpp source/AsyncAnalysis.cpp)
endif()
if(FFT_BUILD_BENCH)
    # The bench links the real pipeline and the real async handoff (AnalysisPipeline.cpp,
    # AsyncAnalysis.cpp), so --cook measures the code TouchDesigner runs, not a replica of it.
    # GATE: `ctest -L perf` runs `fft_bench --gate bench/perf_baseline.json` (see bench.cpp, --gate).
    td_plugin_add_bench(FFT fft_bench SOURCES bench/bench.cpp source/AnalysisPipeline.cpp source/AsyncAnalysis.cpp
                        GATE bench/perf_baseline.json)
    # Answers "would FFTW's built-in threading help?" with numbers (it does not for Mono Mix).
    td_plugin_add_bench(FFT fft_threads_probe SOURCES bench/fftw_threads_probe.cpp)
    # Reports which fftwf DLL is loaded, its version, whether that build has the threads API, which
    # SIMD codelets it actually selects, and the per-size r2c cost. Swapping the DLL next to this exe
    # is how two FFTW builds are compared; no such A/B is recorded in 3rdParty/fftw3/README.md (that
    # file keeps the FFTW3-vs-oneMKL comparison), so re-run the probe to take one.
    td_plugin_add_bench(FFT fft_version_probe SOURCES bench/fftw_version_probe.cpp)
endif()
