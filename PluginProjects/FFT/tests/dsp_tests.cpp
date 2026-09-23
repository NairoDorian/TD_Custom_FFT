// ==========================================================================================
// FFT DSP unit tests - headless, no TouchDesigner required.
// ==========================================================================================
//
// WHAT THIS FILE IS
//   The only test suite for the TouchDesigner-free DSP core (source/DSPModules.h,
//   source/RateModel.h, source/AnalysisPipeline.h). It is a hand-rolled harness, not a framework:
//   CHECK / CHECK_NEAR bump two counters, the section banner is printed, and main() returns non-zero
//   if anything failed. Nothing has to be installed or learned to run or extend it.
//
// WHAT IT COVERS
//   Golden-vector style checks: every SIMD path is compared against a scalar reference, and the
//   full pipeline is checked against a known sine. Concretely: the FIFO ring buffer, FastLog10,
//   magnitude and peak finding, window normalization, the perceptual warp (linear and cubic), the
//   dB converter, ballistics, the biquad EQ's streaming path, the v2.3 helpers, PlanLog,
//   clipLine, the triple buffer and worker signal, the FFTW planner (Auto/Patient, background
//   measurement, the Async toggle) and the FFT backend registry (FFTW3 / oneMKL).
//
// WHAT IT DELIBERATELY DOES NOT COVER
//   Anything TouchDesigner-side: FFT.cpp, Parameters.cpp, the CHOP plumbing, the middle-click
//   popup, the Info DAT and the Python textport logger. Where a plugin-side contract can be
//   restated against the DSP core it is asserted here instead - see test_backend_selection, which
//   notes that the menu-value mapping itself is checked in Parameters.cpp, not in this file.
//
// HOW IT IS BUILT AND RUN
//   Built as the `fft_tests` target from tests/dsp_tests.cpp + source/AnalysisPipeline.cpp
//   (see PluginProjects/FFT/CMakeLists.txt). Relative to PluginProjects/FFT, the documented command
//   is:
//       ninja -C build && ctest --test-dir build --output-on-failure
//   which runs the executable directly:
//       build/bin/Release/fft_tests.exe
//   ctest starts it with the working directory set to build/bin/Release, where the FFTW DLL lives.
//   That directory is also where the tests write their private wisdom file
//   (fft_tests_wisdom.txt) - running the exe by hand from somewhere else still works, but leaves
//   the wisdom file wherever you were standing.
//
// THE CHECK COUNT IS A SIGNAL
//   The last line printed is "N checks, M failures", and N is watched: it is compared between
//   runs and quoted when a change is described, so a difference in it means the coverage changed.
//   As of this writing the suite reports 607 checks, 0 failures on this machine. Never add, remove,
//   reorder or reword a CHECK/CHECK_NEAR, a section banner or a registration in main() to make a
//   comment fit: the comments in this file are comments and nothing else. If a deliberate change
//   moves the count, update this paragraph with it.
//
//   One caveat when comparing counts between machines: a few tests have branches that are taken or
//   skipped depending on what is installed (oneMKL present or not, a size already in the wisdom
//   file) and loop over the backend registry, so the total is genuinely machine-dependent in those
//   places. The 607 above is this machine's number; a different one is not automatically a bug.
//
// RUNNING ORDER (the order main() calls them - which is NOT the order they appear in this file)
//    1. test_fifo                       FIFOBuffer ring: right-aligned, zero-padded while filling
//    2. test_fastlog10                  FastLog10 LUT accuracy, scalar and AVX2
//    3. test_magnitude_and_peak         magnitude kernel vs scalar; findPeakWithIndex/peakMagnitude
//    4. test_window_normalization       CoherentGain == mean 1, FullScale == sum 2, all 6 windows
//    5. test_warp                       PerceptualWarping: identity bypass and the gather path
//    6. test_decibel                    DecibelConverter: dB mode, 0..1 mode, floor clamp
//    7. test_ballistics                 BallisticsFilter attack/release and coefFromMs
//    8. test_eq_streaming               BiquadEQ block ingest == one-shot, plus the legacy mismatch
//    9. test_v23_helpers                silence, mix, cubic warp, partial magnitude, deferred log
//   10. test_plan_log_tail              PlanLog::snapshotTail()/version() against snapshot()
//   11. test_clip_line                  clipLine() - the popup's per-line bound
//   12. test_triple_buffer_and_signal   TripleBuffer handoff + WorkerSignal wake/timeout
//   13. test_background_plan            FFTWEngine Auto/Patient: instant prepare, deferred upgrade
//   14. test_backend_selection          the FFTW3 / oneMKL toggle and its OpenMP-layer contract
//   15. test_async_single_thread        Async off -> no planner thread; on -> the upgrade re-arms
//   16. test_pipeline_sine              full FFT chain on a 1 kHz sine, both normalizations
//   17. test_pipeline_process           AnalysisPipeline::process(): single, multi-channel, silent
//   18. test_identity_grid_and_rate     linear grid == identity warp, and the frequency axis model
//   19. test_rate_model                 RateModel: bin counts, reported rate, axis rate, sizes
//   20. test_equal_loudness             EqualLoudness A/C/468 golden vectors and shape invariants
//
//   Read that against the file itself: entries 1-8 appear in that order, then the file holds 13,
//   14 and 15 (the planner / backend / Async tests), then 9, 10, 11 and 12 (the helper and
//   container tests), then 16-20. So the file order really is not the running order - always trust
//   main() at the bottom of the file, which is also the only place a test becomes part of the run.
//
//   Note also that test_pipeline_process() prints three section banners (one per scenario it
//   covers), so the printed banner count is higher than the test-function count.

#include "DSPModules.h"
#include "RateModel.h"
#include "AnalysisPipeline.h"
#include "AsyncAnalysis.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <vector>

// ------------------------------------------------------------------------------------------
// Allocation counter (the "no allocation on the steady-state path" gate, v2.10)
// ------------------------------------------------------------------------------------------
// Global operator new/delete replaced for this test executable only. Counting is off except inside an
// AllocWindow, and then it counts every allocation in the process - the worker thread and the parallel
// thread pool included - which is exactly what a real-time invariant has to hold against.
static std::atomic<long long> g_allocCount{ 0 };
static std::atomic<bool> g_allocCounting{ false };
static void* countedAlloc(size_t n, size_t align)
{
    if (g_allocCounting.load(std::memory_order_relaxed)) g_allocCount.fetch_add(1, std::memory_order_relaxed);
    void* p = align > alignof(std::max_align_t) ? _aligned_malloc(n ? n : 1, align) : std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new(size_t n) { return countedAlloc(n, 0); }
void* operator new[](size_t n) { return countedAlloc(n, 0); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { try { return countedAlloc(n, 0); } catch (...) { return nullptr; } }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { try { return countedAlloc(n, 0); } catch (...) { return nullptr; } }
void* operator new(size_t n, std::align_val_t a) { return countedAlloc(n, static_cast<size_t>(a)); }
void* operator new[](size_t n, std::align_val_t a) { return countedAlloc(n, static_cast<size_t>(a)); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { _aligned_free(p); }
struct AllocWindow {
    long long start;
    AllocWindow() : start(g_allocCount.load()) { g_allocCounting.store(true); }
    long long count() const { return g_allocCount.load() - start; }
    ~AllocWindow() { g_allocCounting.store(false); }
};

#ifdef _WIN32
#include <tlhelp32.h>   // which libraries are actually loaded in this process (the OpenMP check below)
#endif

using namespace FFTDSP;

// The two counters behind the final "N checks, M failures" line. g_checks is the number people
// watch (see the header), so a check that is meant to be conditional still has to be reached the
// same number of times on every machine for the count to be comparable between runs.
static int g_failures = 0;
static int g_checks = 0;

// Case-insensitive "is a module whose name starts with this loaded in this process" test. Used to
// check the oneMKL threading-layer contract, which is a statement about which DLLs are resident and
// so cannot be checked any other way: FFT results are identical either way, and the difference only
// shows up as a second Intel OpenMP runtime inside TouchDesigner's process.
static bool moduleLoaded(const char* prefix)
{
#ifdef _WIN32
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32 me{};
    me.dwSize = sizeof(me);
    bool found = false;
    const size_t n = std::strlen(prefix);
    if (Module32First(snap, &me)) {
        do {
            if (_strnicmp(me.szModule, prefix, n) == 0) { found = true; break; }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return found;
#else
    (void)prefix;
    return false;
#endif
}

// Pass/fail primitives. Both count the check first and only then test it, so a failing check is
// still counted - the reported total is "checks run", not "checks that passed".
//
// HOW TO CHANGE: these are deliberately macros rather than functions so that __LINE__ names the
// call site in the FAIL message; converting them to templates or functions would print this line
// for every failure and lose the only piece of information the message carries.
#define CHECK(cond)                                                                             \
    do {                                                                                        \
        ++g_checks;                                                                             \
        if (!(cond)) {                                                                          \
            ++g_failures;                                                                       \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                       \
        }                                                                                       \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                   \
    do {                                                                                        \
        ++g_checks;                                                                             \
        double _a = (a), _b = (b), _t = (tol);                                                  \
        if (!(std::abs(_a - _b) <= _t)) {                                                       \
            ++g_failures;                                                                       \
            std::printf("  FAIL %s:%d  %s=%g vs %s=%g (tol %g)\n", __FILE__, __LINE__, #a, _a, #b, _b, _t); \
        }                                                                                       \
    } while (0)

// Section banner. Printed, not stored: it exists so that a hung run or a crash (which leaves the
// last printed line as the whole diagnosis, see main()) says which test it was in.
static void section(const char* name) { std::printf("[%s]\n", name); }

// ------------------------------------------------------------------------------------------
// ===================== test_fifo =====================
// WHAT:  FIFOBuffer::get() always hands back exactly `capacity` samples, oldest-first, with the
//        NEWEST sample last: an exact copy of the last N once the buffer has filled, and the same
//        copy right-aligned behind zeros while it is still filling. Ordering property, not a
//        numeric one - the check is bit equality, not a tolerance.
// WHY:   not recorded in this file. The property is the one the ring buffer's own contract depends
//        on (see the FIFOBuffer block in DSPModules.h): callers rely on "now" being at the last
//        index whether the node just started or has been running for an hour.
// HOW TO CHANGE: the 10 in this function is the buffer capacity and every 10 in the body means that
//        same capacity, not a window length. The shipped default is 3175 (72 ms at 44.1 kHz); 10 is
//        used here only so that 50 blocks wrap the ring many times over.
static void test_fifo()
{
    section("FIFOBuffer");
    FIFOBuffer fifo(10);
    std::vector<float> ref;                      // the same stream kept as a plain vector to compare against
    std::mt19937 rng(7);                         // fixed seed: the run has to be reproducible
    AlignedVector out;
    for (int step = 0; step < 50; ++step) {      // 50 blocks against a 10-deep ring: wraps repeatedly
        size_t n = 1 + rng() % 13;               // blocks of 1..13: some shorter than the capacity, some longer
        std::vector<float> block(n);
        for (auto& v : block) v = static_cast<float>(rng() % 1000);   // 0..999, exact in a float
        fifo.add(block.data(), n);
        ref.insert(ref.end(), block.begin(), block.end());
        if (ref.size() > 10) ref.erase(ref.begin(), ref.end() - 10);   // keep the reference to the same depth
        fifo.get(out);
        CHECK(out.size() == 10);                 // always the full capacity, never a short buffer
        // out is right-aligned while filling, exact copy afterwards
        size_t offset = 10 - ref.size();
        bool ok = true;
        for (size_t i = 0; i < ref.size(); ++i) ok = ok && (out[offset + i] == ref[i]);
        for (size_t i = 0; i < offset; ++i) ok = ok && (out[i] == 0.0f);
        CHECK(ok);
    }
}

// ------------------------------------------------------------------------------------------
// ===================== test_fastlog10 =====================
// WHAT:  FastLog10::scaled(x) - the 2048-entry mantissa LUT that stands in for 20*log10 - stays
//        within 0.006 dB of the exact double-precision value across a 300 dB sweep, and under AVX2
//        the 8-wide vector form agrees with the scalar form to 1e-4. The scalar sweep is the real
//        contract; the vector check only guards the two implementations against drifting apart.
// WHY:   the LUT is an approximation by construction, so the question the test answers is "how
//        wrong can it be", and the answer is bounded next to the table in DSPModules.h. The
//        tolerance below is deliberately set above that documented worst case rather than pinned
//        to it, so the test fails on a real regression and not on the last digit.
// HOW TO CHANGE: if the LUT size or its sampling changes (kTableBits / kTableSize, or mid-interval
//        vs lower-edge rounding), both the 0.006 here and the comment next to the check have to be
//        re-derived together - they describe the same number.
static void test_fastlog10()
{
    section("FastLog10");
    double max_err = 0.0;
    // 1e-9 .. 1e6 is -180 dB .. +120 dB; the 3.7% geometric step means consecutive samples land
    // between the LUT's entries as well as on them, which is what makes the mid-interval sampling
    // observable here rather than a property of the table alone.
    for (float x = 1e-9f; x < 1e6f; x *= 1.037f) {
        double ref = 20.0 * std::log10(static_cast<double>(x));
        max_err = std::max(max_err, std::abs(ref - FastLog10::scaled(x)));
    }
    std::printf("  scalar max error: %.5f dB\n", max_err);
    CHECK(max_err < 0.006);   // 2048-entry LUT, mid-interval samples: 0.0042 dB worst case
#if defined(__AVX2__)
    // Exactly one AVX2 register of inputs, spanning several exponent decades and including a power
    // of two (1.0, a mantissa boundary) and a large odd mantissa (12345.0).
    alignas(32) float in[8] = { 1e-6f, 0.001f, 0.5f, 1.0f, 3.7f, 100.0f, 12345.0f, 2.5e5f };
    alignas(32) float out[8];
    _mm256_store_ps(out, FastLog10::scaledVec(_mm256_load_ps(in)));
    // Both paths read the same table with the same arithmetic, so this only has to absorb float
    // rounding at these magnitudes - hence a tolerance four orders tighter than the scalar one.
    for (int i = 0; i < 8; ++i) CHECK_NEAR(out[i], FastLog10::scaled(in[i]), 1e-4);
#endif
}

// ------------------------------------------------------------------------------------------
// ===================== test_magnitude_and_peak =====================
// WHAT:  computeMagnitudeAVX2_FMA() computes sqrt(re^2 + im^2) to within 1e-5 relative of a
//        double-precision scalar reference, and findPeakWithIndex()/peakMagnitude() agree with
//        std::max_element on where the peak is and what it is worth - including when the peak is at
//        index 0, which is the case a running-maximum loop gets wrong if it is initialised badly.
// WHY:   not recorded in this file.
// HOW TO CHANGE: the 1000-point input is intentional - it is not a multiple of 8, so the AVX2 body
//        runs on 992 samples and the last 8 fall through to the scalar tail; the peak test then uses
//        19 (16 vectorised + 3 scalar) to reach the tail at both ends of the array.
static void test_magnitude_and_peak()
{
    section("computeMagnitudeAVX2_FMA / peak");
    const size_t n = 1000;              // 125 vectors of 8, plus a scalar tail (see above)
    AlignedComplexVector c(n);
    AlignedVector mag(n), ref(n);
    std::mt19937 rng(3);                // fixed seed: reproducible data on every run
    std::uniform_real_distribution<float> d(-100.0f, 100.0f);   // both signs, so re/im are exercised
    for (size_t i = 0; i < n; ++i) {
        c[i] = { d(rng), d(rng) };
        ref[i] = std::sqrt(c[i].real() * c[i].real() + c[i].imag() * c[i].imag());   // exact, in double
    }
    computeMagnitudeAVX2_FMA(reinterpret_cast<const float*>(c.data()), mag.data(), n);
    double max_rel = 0.0;
    for (size_t i = 0; i < n; ++i) {
        // 1e-9 guards the division: a ref of exactly 0 would otherwise divide by zero.
        double rel = std::abs(static_cast<double>(mag[i]) - ref[i]) / std::max(1e-9, static_cast<double>(ref[i]));
        max_rel = std::max(max_rel, rel);
    }
    std::printf("  magnitude max rel error: %.2e\n", max_rel);
    CHECK(max_rel < 1e-5);              // relative, so it holds across the whole input range

    size_t idx = 0;
    float pk = findPeakWithIndex(mag.data(), n, idx);
    auto it = std::max_element(ref.begin(), ref.end());   // the reference is the same array, in a different order of comparison
    CHECK(static_cast<size_t>(it - ref.begin()) == idx);
    CHECK_NEAR(pk, *it, 1e-3 * (*it));                    // slack: absorbs the order a running max accumulates in
    CHECK_NEAR(peakMagnitude(mag.data(), n), *it, 1e-3 * (*it));

    // peak at index 0 and in the scalar tail
    // 19 = 16 (two AVX2 vectors) + 3 scalar; the two probes are the two ends.
    AlignedVector small(19, 0.0f);
    small[0] = 5.0f; CHECK(findPeakWithIndex(small.data(), 19, idx) == 5.0f && idx == 0);
    small[18] = 9.0f; CHECK(findPeakWithIndex(small.data(), 19, idx) == 9.0f && idx == 18);
}

// ------------------------------------------------------------------------------------------
// ===================== test_window_normalization =====================
// WHAT:  WindowGenerator::generateWindow() produces a window whose mean is 1 under CoherentGain and
//        whose sum is 2 under FullScale, for every window type the generator implements.
// WHY:   not recorded in this file. These are the two normalizations' defining properties - they are
//        what make the two magnitude scales mean anything downstream (see the WindowNorm block in
//        DSPModules.h: CoherentGain is the legacy scale whose windowed full-scale sine peaks at
//        N_win/2, FullScale is the one where a sine of amplitude A reads back as A).
// HOW TO CHANGE: this loops over all six types (0..5 = Kaiser, Hann, Hamming, Blackman,
//        Blackman-Harris, Rectangular - see Parameters::WindowType). Adding a window type means
//        raising the 6 and nothing else here; a type added to the enum but not to this loop is
//        untested, and the check count will not change to tell you so.
static void test_window_normalization()
{
    section("WindowGenerator normalization");
    AlignedVector w;
    for (int type = 0; type < 6; ++type) {           // every WindowType except COUNT
        // 15.0 is the Kaiser beta (Parameters::kaiserBeta's default); it is ignored by the other
        // five types. 3175 is the default window length: 72 ms at 44.1 kHz.
        WindowGenerator::generateWindow(type, 15.0, 3175, w, WindowNorm::CoherentGain);
        double mean = 0; for (float v : w) mean += v; mean /= w.size();
        CHECK_NEAR(mean, 1.0, 1e-4);                 // 1e-4: accumulations over 3175 floats, nothing more
        WindowGenerator::generateWindow(type, 15.0, 3175, w, WindowNorm::FullScale);
        double sum = 0; for (float v : w) sum += v;
        CHECK_NEAR(sum, 2.0, 1e-4);
    }
}

// ------------------------------------------------------------------------------------------
// ===================== test_warp =====================
// WHAT:  PerceptualWarping::applyWarp() resamples a linear magnitude spectrum onto the warped
//        frequency axis in two ways: when the warp comes out as the identity it is a verbatim copy,
//        and otherwise each output bin is a linear interpolation between two linear bins, which this
//        recomputes in scalar and compares. It also checks the identity/not-identity flag actually
//        agrees with what applyWarp() does, and that the target grid stays inside [0, Nyquist].
// WHY:   not recorded in this file.
// HOW TO CHANGE: the scalar reference below re-derives the bin mapping from targetHz() rather than
//        reading it out of the warp tables, so it is an independent check of the table build - keep
//        it that way (copying m_i0/m_w into the test would only assert that they equal themselves).
static void test_warp()
{
    section("PerceptualWarping");
    const size_t nlin = 513;                    // a 1024-point R2C transform has 1024/2 + 1 bins
    AlignedVector src(nlin), out;
    // A smooth, non-constant signal: the scalar reference models linear interpolation of a smooth
    // curve, so a bound on the difference is a statement about the kernel and not about the data.
    for (size_t i = 0; i < nlin; ++i) src[i] = static_cast<float>(std::sin(i * 0.05) * 10.0 + i * 0.01);
    PerceptualWarping w;

    // identity: linear scale, fmax == nyquist, bins == nlin
    // Scale 5 is Linear, whose perceptual grid IS the linear grid, so the warp blend cannot make a
    // difference here - 1.0 (fully perceptual) still lands on the identity. 22050.0 is the Nyquist
    // of 44.1 kHz; 20.0 is the Log Floor Hz, which the Linear scale never reads.
    w.buildWarpTables(5, 22050.0, nlin, 22050.0, 1.0, 20.0, nlin);
    CHECK(w.isIdentity());
    w.applyWarp(src, out);
    bool same = out.size() == nlin;
    for (size_t i = 0; same && i < nlin; ++i) same = (out[i] == src[i]);   // bit equality: it is a memcpy
    CHECK(same);

    // log scale: gather path vs scalar reference
    const size_t n_out = 1000;                  // != nlin, so this is the gather path and not the bypass
    // Scale 0 is Log; 0.963 is Parameters::warp's default Log blend (value copied from the
    // implementation, not derived here).
    w.buildWarpTables(0, 22050.0, n_out, 22050.0, 0.963, 20.0, nlin);
    CHECK(!w.isIdentity());
    w.applyWarp(src, out);
    CHECK(out.size() == n_out);
    const auto& hz = w.targetHz();
    double max_err = 0.0;
    for (size_t i = 0; i < n_out; ++i) {
        double frac = hz[i] / 22050.0 * (nlin - 1);       // where this target bin falls on the linear grid
        // i0 is clamped to nlin-2 so that the i0+1 tap below always exists - the same clamp the
        // table build uses, including for the top bin.
        size_t i0 = static_cast<size_t>(std::min<double>(nlin - 2, std::floor(frac)));
        double wt = frac - i0;
        double ref = src[i0] + wt * (src[i0 + 1] - src[i0]);
        max_err = std::max(max_err, std::abs(ref - out[i]));
    }
    std::printf("  warp max error vs scalar reference: %.2e\n", max_err);
    CHECK(max_err < 1e-3);                      // absolute, on values of order 10: float rounding in the gather
    // computeTargetHzGrid clamps every target to [0, fmax]; the 1e-6 is float slack on that clamp.
    CHECK(hz.front() >= 0.0 && hz.back() <= 22050.0 + 1e-6);
}

// ------------------------------------------------------------------------------------------
// ===================== test_decibel =====================
// WHAT:  DecibelConverter::convertToDB() in its two modes: mode 1 writes dB relative to the
//        reference (the loudest bin lands on 0 dB, everything quieter is negative and is floored),
//        mode 2 writes a 0..1 linear readout. The floor is checked by converting an all-zero vector.
// WHY:   not recorded in this file. The expected values are computed here rather than tabulated
//        (20*log10(32/64) = -6.02 dB one octave down from the peak), so this pins the definition
//        of the scale, not a set of recorded numbers.
// HOW TO CHANGE: the 1.0f / peak reference is what makes a.back() land on 0 dB; if the caller's
//        reference convention changes, the two "expected 0 dB" checks change with it.
static void test_decibel()
{
    section("DecibelConverter");
    AlignedVector s(64);                        // 64 bins: a power of two, so index 32 is exactly half of it
    for (size_t i = 0; i < s.size(); ++i) s[i] = static_cast<float>(i + 1);   // ramp 1..64: peak at the last bin
    AlignedVector a = s, b = s;
    float peak = peakMagnitude(s.data(), s.size());
    // 80.0 is the converter's dB range: the floor is -80 dB, which is the value the zero-vector
    // check at the bottom of this function expects.
    DecibelConverter::convertToDB(1, 80.0, 1.0f / peak, a);
    CHECK_NEAR(a.back(), 0.0, 6e-3);                                 // loudest bin -> 0 dB (LUT: <= 0.0042 dB)
    CHECK_NEAR(a[31], 20.0 * std::log10(32.0 / 64.0), 6e-3);           // -6.02 dB
    DecibelConverter::convertToDB(2, 80.0, 1.0f / peak, b);
    CHECK_NEAR(b.back(), 1.0, 1e-3);
    for (float v : b) CHECK(v >= 0.0f && v <= 1.0f);                 // mode 2 is bounded by construction
    // tiny values clamp to the floor
    AlignedVector z(16, 0.0f);                  // 16 = two AVX2 registers, every lane at the floor
    DecibelConverter::convertToDB(1, 80.0, 1.0f, z);
    for (float v : z) CHECK_NEAR(v, -80.0, 1e-4);                    // the clamp is exact, so this is tight
}

// ------------------------------------------------------------------------------------------
// ===================== test_ballistics =====================
// WHAT:  BallisticsFilter::apply() moves the displayed value a fixed fraction of the way toward the
//        input each frame, using the attack fraction while rising and the release fraction while
//        falling, and coefFromMs() converts a time constant into that per-frame fraction.
// WHY:   not recorded in this file.
// HOW TO CHANGE: the two expected values are the definition of the coefficients, not recorded
//        measurements - 0.5 covers half the gap, 0.9 covers a tenth of it. If the argument
//        convention is ever changed to "fraction retained" rather than "fraction covered", both
//        expectations invert and this test is what tells you.
static void test_ballistics()
{
    section("BallisticsFilter");
    BallisticsFilter f;
    // 32 = two unrolled AVX2 bodies of 16; prev starts at 0 so the first call is a pure attack.
    AlignedVector cur(32, 1.0f), prev(32, 0.0f);
    f.apply(0.5f, 0.9f, cur, prev);
    for (float v : prev) CHECK_NEAR(v, 0.5, 1e-6);            // attack: half way
    std::fill(cur.begin(), cur.end(), 0.0f);
    f.apply(0.5f, 0.9f, cur, prev);
    for (float v : prev) CHECK_NEAR(v, 0.45, 1e-6);           // release: 10% of the way down
    // Both of these are the function's own definition rather than a fit: a non-positive time is
    // "no smoothing" (coefficient 0), and the coefficient is exp(-dt/tau). 16.6667 ms is one frame
    // at 60 fps and 100.0 ms is the time constant (tau, the time to reach 63%).
    CHECK_NEAR(BallisticsFilter::coefFromMs(0.0, 16.7), 0.0, 1e-9);
    CHECK_NEAR(BallisticsFilter::coefFromMs(100.0, 16.6667), std::exp(-16.6667 / 100.0), 1e-6);
}

// ------------------------------------------------------------------------------------------
// ===================== test_eq_streaming =====================
// WHAT:  BiquadEQ's two entry points agree: feeding the signal through processBlockInPlace() in
//        irregular blocks gives the same output as processAudio() over the whole signal in one go.
//        It then pins the opposite for the legacy behaviour (re-filtering each window from scratch),
//        and finally that a disabled EQ is a pass-through that leaves the buffer untouched.
// WHY:   the second half exists because the mismatch is not a rounding error - the legacy path
//        starts every window with stale filter state, which is audible and is exactly what the
//        streaming rework removed. The test asserts the difference is real (> 1e-4) so the fix
//        cannot silently be reverted into "close enough".
// HOW TO CHANGE: the filter is designed by updateAndCheckActive(gain_db, cutoff_hz, low_gain_db,
//        low_cutoff_hz, q_factor, amount) - a +6 dB high shelf at 1 kHz and a -3 dB low shelf at
//        200 Hz, Q 0.707, applied at full amount. Both engines must be given identical settings or
//        the comparison is meaningless, which is the one thing an edit here must preserve.
static void test_eq_streaming()
{
    section("BiquadEQ streaming (block ingest == one-shot)");
    const size_t total = 3175 * 3;              // three default windows of audio (72 ms at 44.1 kHz each)
    std::vector<float> sig(total);
    std::mt19937 rng(11);                       // fixed seed: reproducible data
    std::uniform_real_distribution<float> d(-1.0f, 1.0f);
    for (auto& v : sig) v = d(rng);

    BiquadEQ one(44100.0), blocks(44100.0);     // same settings, two engines: one-shot vs streaming
    CHECK(one.updateAndCheckActive(6.0, 1000.0, -3.0, 200.0, 0.707, 1.0));    // returns "is it active"
    CHECK(blocks.updateAndCheckActive(6.0, 1000.0, -3.0, 200.0, 0.707, 1.0));
    AlignedVector whole(sig.begin(), sig.end()), out;
    one.processAudio(whole, 1.0, out);                       // reference: whole signal in one pass

    std::vector<float> streamed;
    // Block sizes deliberately chosen so that no block boundary lines up with another: 735 (one
    // 60 fps frame at 44.1 kHz), 512 and 1024 (power-of-two sizes that are not multiples of 735),
    // and 3 (a final stub to force a tiny partial block). The % 5 cycles them.
    size_t pos = 0, sizes[] = { 735, 512, 1024, 735, 3 };
    int k = 0;
    while (pos < total) {
        size_t n = std::min(sizes[k++ % 5], total - pos);
        std::vector<float> blk(sig.begin() + pos, sig.begin() + pos + n);
        blocks.processBlockInPlace(blk.data(), n, 1.0);
        streamed.insert(streamed.end(), blk.begin(), blk.end());
        pos += n;
    }
    double max_err = 0.0;
    for (size_t i = 0; i < total; ++i) max_err = std::max(max_err, std::abs(static_cast<double>(streamed[i]) - out[i]));
    std::printf("  streaming vs one-shot max error: %.2e\n", max_err);
    CHECK(max_err < 1e-4);   // identical recurrence; only float rounding of the blend differs
    // and a windowed-per-frame re-filter (the legacy behaviour) does NOT match the true filter output
    BiquadEQ legacy(44100.0);
    legacy.updateAndCheckActive(6.0, 1000.0, -3.0, 200.0, 0.707, 1.0);
    // Two overlapping 3175-sample windows, 735 apart - the legacy hop. o2 is compared against the
    // one-shot output at the offset it actually occupies, out[735 + i].
    AlignedVector w1(sig.begin(), sig.begin() + 3175), w2(sig.begin() + 735, sig.begin() + 735 + 3175), o1, o2;
    legacy.processAudio(w1, 1.0, o1);
    legacy.processAudio(w2, 1.0, o2);
    double legacy_err = 0.0;
    for (size_t i = 0; i < 3175; ++i) legacy_err = std::max(legacy_err, std::abs(static_cast<double>(o2[i]) - out[735 + i]));
    std::printf("  legacy per-window re-filter error vs true output: %.2e (expected > 0: stale state at window start)\n", legacy_err);
    CHECK(legacy_err > 1e-4);                   // the point of this half: the legacy path is measurably wrong

    // inactive EQ is a pass-through in place
    BiquadEQ off(44100.0);
    CHECK(!off.updateAndCheckActive(0.0, 1000.0, 0.0, 200.0, 0.707, 1.0));   // amount 0 -> not active
    float x[4] = { 1, 2, 3, 4 };                // 4 samples, unaligned stack buffer: the pass-through must not read past it
    off.processBlockInPlace(x, 4, 1.0);
    CHECK(x[0] == 1 && x[3] == 4);              // first and last unchanged
}

// ------------------------------------------------------------------------------------------
// ===================== test_background_plan =====================
// WHAT:  FFTWEngine::prepare() returns immediately with a usable plan even when the requested policy
//        wants a measurement (Auto, Patient), and the measurement happens on a background thread
//        that pollBackgroundPlan() picks up later - while executeRFFT() keeps working the whole time.
//        It also pins that a measured plan reaches the on-disk wisdom file, so a fresh engine (and
//        Auto) then gets the same size instantly.
// WHY:   the 250 ms bound is the point of the whole scheme: a synchronous FFTW_MEASURE at these
//        sizes stalls the TouchDesigner frame, so "prepare() never blocks the caller" is the
//        contract being tested, not "prepare() is fast".
// HOW TO CHANGE: this test writes a private wisdom file (fft_tests_wisdom.txt) precisely so it
//        neither reads nor modifies the user's cache. The file is shared with the later planner
//        tests and with test_v23_helpers - they assume the wisdom written here is already on disk,
//        so do not turn the wisdom override or the remove() at the top into something per-test.
static void test_background_plan()
{
    section("FFTWEngine Auto planner (instant + background measure)");
    // private wisdom file so the test neither depends on nor modifies the user's cache
    FFTWEngine::wisdomPathOverride() = "fft_tests_wisdom.txt";
    std::remove("fft_tests_wisdom.txt");
    PlanLog log;
    FFTWEngine e;
    auto t0 = std::chrono::steady_clock::now();
    e.prepare(65536, PlannerPolicy::Auto, &log);            // a size unlikely to be in wisdom on a fresh machine
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  prepare(65536, Auto) returned in %.1f ms: %s\n", ms, e.getPlanStatus().c_str());
    CHECK(ms < 250.0);                                       // never a MEASURE-length stall on the calling thread
    // 65536 + 1 zero samples with a single impulse; the impulse makes every bin's magnitude exactly
    // 1, so the magnitude checks below do not depend on the window or on any scaling choice.
    AlignedVector frame(65536, 0.0f), mag; AlignedComplexVector scratch;
    frame[100] = 1.0f;
    e.executeRFFT(frame, mag, scratch);                      // executes while the background thread may be measuring
    CHECK(mag.size() == 32769);                              // N/2 + 1 for a real-to-complex transform of 65536
    for (int i = 0; i < 400 && !e.pollBackgroundPlan(); ++i) {   // wait for the upgrade (or wisdom-only instant plan)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));   // 400 x 10 ms = a 4 s ceiling, then the check fails
        e.executeRFFT(frame, mag, scratch);
    }
    std::printf("  final: %s\n", e.getPlanStatus().c_str());
    CHECK(e.getPlanStatus().find("FFTW_MEASURE") != std::string::npos);   // the upgrade really was a measurement
    e.executeRFFT(frame, mag, scratch);
    CHECK_NEAR(mag[0], 1.0, 1e-4);                           // impulse -> flat magnitude 1

    // Patient: same non-blocking scheme with FFTW_PATIENT (small N so the test stays quick)
    FFTWEngine pe;
    t0 = std::chrono::steady_clock::now();
    pe.prepare(2048, PlannerPolicy::Patient, &log);
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  prepare(2048, Patient) returned in %.1f ms: %s\n", ms, pe.getPlanStatus().c_str());
    CHECK(ms < 250.0);
    AlignedVector pframe(2048, 0.0f), pmag; AlignedComplexVector pscratch;
    pframe[7] = 1.0f;
    for (int i = 0; i < 1500 && !pe.pollBackgroundPlan(); ++i) {   // 1500 x 10 ms = 15 s: patient planning is slower
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        pe.executeRFFT(pframe, pmag, pscratch);             // executing while the patient planner runs is fine
    }
    std::printf("  final: %s\n", pe.getPlanStatus().c_str());
    CHECK(pe.getPlanStatus().find("FFTW_PATIENT") != std::string::npos);
    pe.executeRFFT(pframe, pmag, pscratch);
    CHECK_NEAR(pmag[0], 1.0, 1e-4);
    // the patient plan is now in wisdom: a fresh engine gets it instantly, and so does Auto
    FFTWEngine pe2, ae;
    pe2.prepare(2048, PlannerPolicy::Patient, &log);
    CHECK(pe2.getPlanStatus().find("from wisdom") != std::string::npos);
    ae.prepare(2048, PlannerPolicy::Auto, &log);
    CHECK(ae.getPlanStatus().find("from wisdom") != std::string::npos);
}

// ------------------------------------------------------------------------------------------
// ===================== test_backend_selection =====================
// The FFT Backend toggle. Runs identically whether or not oneMKL is installed on this machine,
// because both are legitimate outcomes and the test asserts the contract rather than the install:
//   1. asking for oneMKL either loads it, or falls back to FFTW3 *with a working plan* - never a
//      node with no plan, which would publish a flat spectrum and look like a broken plugin;
//   2. every backend that is offered by the registry can be asked for and produces the same
//      transform (a backend switch is a performance choice, not a numerical one);
//   3. switching back and forth re-plans cleanly and leaves no plan behind for the wrong library
//      (a plan freed by the other library's destroy_plan is heap corruption, see FftBackend.h).
//
// WHAT:  For every backend the registry offers: it produces a plan, the engine reports a backend,
//        and its magnitude spectrum matches FFTW3's bin for bin. Then the same engine is switched
//        between all of them three times over and re-checked each time. When oneMKL happens to be
//        resident, it also checks that the Intel OpenMP layer was NOT pulled in with it.
// WHY:   the third point above is the real subject: a plan belongs to the library that created it,
//        and the toggle path hands the engine a plan from the *previous* library on every switch.
//        Checked by value because the damage would otherwise be silent heap corruption, not a crash.
// HOW TO CHANGE: the spectrum comparison is against a reference produced by the FFTW3 entry in the
//        registry, so it assumes backend 0 is FFTW3; and the menu-value mapping (the kFftw3 = 0
//        below) is a TouchDesigner-side contract asserted in Parameters.cpp, not here - changing one
//        without the other is how the menu and the registry drift apart.
static void test_backend_selection()
{
    section("FFT backend selection (FFTW3 / oneMKL toggle)");
    FFTWEngine::wisdomPathOverride() = "fft_tests_wisdom.txt";
    PlanLog log;
    // The registry is the single source of truth here: this test does not include Parameters.h (it is
    // a TouchDesigner-side header), so the menu-value contract is asserted there instead.
    CHECK(backendCount() >= 2);
    const int kFftw3 = 0;   // Parameters::Backend::Fftw3 — see the static_assert in Parameters.cpp

    const size_t N = 4096;                            // a small power of two: fast to plan, 2049 output bins
    AlignedVector frame(N, 0.0f);
    frame[9] = 1.0f;                                  // impulse: |X[k]| == 1 for every k
    AlignedVector reference;
    {
        FFTWEngine e;
        e.prepare(N, PlannerPolicy::Fast, &log, &backendById(kFftw3));
        CHECK(e.hasPlan());
        AlignedComplexVector scratch;
        e.executeRFFT(frame, reference, scratch);
        CHECK_NEAR(reference[5], 1.0, 1e-3);          // bin 5 is as good as any: an impulse is flat
    }

    for (int i = 0; i < backendCount(); ++i) {
        const FftBackendInfo& want = backendById(i);
        FFTWEngine e;
        e.prepare(N, PlannerPolicy::Fast, &log, &want);
        // Which library actually ended up live, which is not necessarily the one asked for.
        const std::string status = e.getPlanStatus();
        const bool loaded = status.find("(no library)") == std::string::npos;
        std::printf("  asked for %-8s -> %s\n", want.id, status.c_str());
        CHECK(loaded);                                        // never left without a plan
        CHECK(e.hasPlan());
        CHECK(e.backendReport().find("<no backend>") == std::string::npos);
        AlignedVector mag; AlignedComplexVector scratch;
        e.executeRFFT(frame, mag, scratch);
        // Same numbers either way: the backend changes how the transform is computed, never what it is.
        double worst = 0.0;
        for (size_t k = 0; k < reference.size(); ++k)
            worst = std::max(worst, static_cast<double>(std::abs(mag[k] - reference[k])));
        std::printf("     max |mag - FFTW3 mag| over %zu bins: %.2e\n", reference.size(), worst);
        CHECK(worst < 1e-3);                          // magnitudes are 1.0, so this is ~0.1% relative
    }

    // Toggling on one engine, repeatedly: the plan in hand belongs to the previous library every
    // time, so this is the path that would corrupt the heap if the plan were destroyed by the wrong
    // library. Checked by value, since the damage would be silent otherwise.
    {
        FFTWEngine e;
        for (int round = 0; round < 3; ++round) {     // 3 rounds x backendCount() switches below
            for (int i = 0; i < backendCount(); ++i) {
                e.prepare(N, PlannerPolicy::Fast, &log, &backendById(i));
                CHECK(e.hasPlan());
                AlignedVector mag; AlignedComplexVector scratch;
                e.executeRFFT(frame, mag, scratch);
                CHECK_NEAR(mag[5], 1.0, 1e-3);
            }
        }
        std::printf("  %d backend switches in one engine: plan valid every time\n", 3 * backendCount());
    }

    // If oneMKL is installed, it is now loaded, and this is the only place that can check what it
    // brought with it. The plugin calls MKL_Set_Threading_Layer(MKL_THREADING_SEQUENTIAL) at load
    // time: oneMKL otherwise defaults to its Intel OpenMP layer, which loads libiomp5md.dll into the
    // host process. TouchDesigner already ships and loads its own copy, and two Intel OpenMP runtimes
    // in one process is the "Error #15: ... but found libiomp5md.dll already initialized" abort - so
    // the whole point of the switch is that the OpenMP layer must NOT be resident here.
    //
    // Measured both ways on this machine (mkl_rt.3.dll, oneMKL 2026.1.0): a plan + execute of a
    // 4096-point r2c with no threading-layer call leaves mkl_intel_thread.3.dll resident; with the
    // call, neither mkl_intel_thread.3.dll nor libiomp5md.dll is loaded. So this check is not
    // vacuous - it is exactly the observable difference the switch makes, and it fails if the switch
    // is ever lost (for instance if a future mkl_rt stops exporting MKL_Set_Threading_Layer).
    if (moduleLoaded("mkl_rt")) {
        const bool omp = moduleLoaded("mkl_intel_thread") || moduleLoaded("libiomp5md");
        std::printf("  oneMKL resident; intel_thread/libiomp5md loaded: %s\n", omp ? "YES" : "no");
        CHECK(!omp);        // the sequential layer switch took effect
    } else {
        std::printf("  (oneMKL is not installed here - the OpenMP layer check is skipped)\n");
    }
}

// ------------------------------------------------------------------------------------------
// ===================== test_async_single_thread =====================
// Async off must mean one thread for the whole node. The window -> FFT -> warp -> dB chain is
// already single-threaded there (the pipeline owner is the cook thread), and the channel fan-out
// only fires with more than one channel, so the one piece of work that could still escape to another
// thread is the FFTW planner's deferred MEASURE/PATIENT upgrade. This asserts both halves of that
// contract, in one engine:
//   * off -> nothing is started, the node keeps cooking on a correct ESTIMATE plan;
//   * on  -> the deferred measurement starts, because prepare()'s early-out sees an unchanged size,
//            policy and backend and will never re-plan on its own. Without the re-arm the node would
//            sit on ESTIMATE for the rest of the session.
//
// WHAT:  Both halves of that contract, in one engine: with the background disabled nothing is
//        started and the status says "deferred"; with it enabled again - on the same engine, with
//        no re-prepare - the deferred upgrade starts and completes, while the engine keeps
//        executing a valid transform throughout.
// WHY:   the status wording is checked because it is what the Info DAT shows; a node that said
//        "measuring in background" while Async was off would be promising an upgrade that is never
//        coming. The 12288 size and the from_wisdom escape hatch below exist so that the deferral
//        path is actually reachable rather than assumed.
// HOW TO CHANGE: this early-returns on a machine whose wisdom already covers N, and the checks
//        above that point have already run - so the number this test contributes depends on the
//        machine's wisdom state. Do not "fix" that by hoisting the checks out of the branch; the
//        branch is what keeps the test honest about a run it cannot reach.
static void test_async_single_thread()
{
    section("Async toggle: deferred background upgrade (single-threaded node)");
    FFTWEngine::wisdomPathOverride() = "fft_tests_wisdom.txt";   // the file the earlier tests already imported
    PlanLog log;
    // A size no other test plans, so an Auto plan cannot come from wisdom and there is a real upgrade
    // to defer. If a machine's wisdom happens to cover it, the branch below says so and skips rather
    // than asserting a path the run cannot reach.
    const size_t N = 12288;

    FFTWEngine e;
    e.setBackgroundAllowed(false);                 // Async off, exactly as the node delivers it per cook
    e.prepare(N, PlannerPolicy::Auto, &log);
    const bool from_wisdom = e.getPlanStatus().find("from wisdom") != std::string::npos;
    std::printf("  Async off, prepare(%zu, Auto): %s\n", N, e.getPlanStatus().c_str());
    CHECK(e.hasPlan());                            // deferred, never planless
    if (from_wisdom) {
        std::printf("  (N is already measured in wisdom here - the deferral path is not reachable)\n");
        CHECK(!e.upgradeInProgress());
        return;
    }
    CHECK(!e.upgradeInProgress());                 // no planner thread was started
    // The status has to say "deferred", not "measuring in background": it is what the Info DAT shows,
    // and it would otherwise promise an upgrade that is not coming.
    CHECK(e.getPlanStatus().find("deferred") != std::string::npos);
    CHECK(e.getPlanStatus().find("measuring in background") == std::string::npos);

    // Twenty cooks' worth of polls with Async off: still no thread, still a valid transform.
    AlignedVector frame(N, 0.0f), mag; AlignedComplexVector scratch;
    frame[11] = 1.0f;
    for (int i = 0; i < 20; ++i) {
        e.executeRFFT(frame, mag, scratch);        // executes on this thread, the whole point of the toggle
        e.setBackgroundAllowed(false);             // delivered every cook, so a missed flip cannot hide
        CHECK(!e.pollBackgroundPlan());
    }
    CHECK(!e.upgradeInProgress());
    CHECK_NEAR(mag[0], 1.0, 1e-4);

    // Async back on, same engine, no re-prepare: the deferred measurement must start now.
    e.setBackgroundAllowed(true);
    CHECK(e.upgradeInProgress());
    bool upgraded = false;
    for (int i = 0; i < 400 && !upgraded; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        e.executeRFFT(frame, mag, scratch);        // cooking while the planner measures is the design
        upgraded = e.pollBackgroundPlan();
    }
    std::printf("  Async on  -> %s\n", e.getPlanStatus().c_str());
    CHECK(upgraded);
    CHECK(e.getPlanStatus().find("upgraded in background") != std::string::npos);
    e.executeRFFT(frame, mag, scratch);
    CHECK_NEAR(mag[0], 1.0, 1e-4);
}

// ------------------------------------------------------------------------------------------
// ===================== test_v23_helpers =====================
// WHAT:  A group of independent v2.3 helper contracts, in the order they appear:
//        - blockIsSilent(): true for 0.0 / -0.0 only, false as soon as one sample is nonzero, and
//          it must stop at the count it is given rather than scanning the whole buffer;
//        - addInto() + scaleInPlace(): the mono-mix helpers compute (a + b) * g exactly;
//        - the cubic (Catmull-Rom) warp setting against a scalar reference, and that identity still
//          bypasses it;
//        - maxLinearIndex() shrinking when Display Max is below Nyquist;
//        - the partial-magnitude argument of executeRFFT();
//        - PlanLog's deferred mode (nothing reaches the textport until flush, and flush is
//          idempotent).
// WHY:   not recorded in this file, except for the cubic reference, which exists because that path
//        is SIMD-gathered and had no independent check.
// HOW TO CHANGE: the 1e-30 sample below is the interesting one - it is denormal-adjacent and exists
//        to prove blockIsSilent() is testing bits and not comparing against 0.0 with a tolerance.
static void test_v23_helpers()
{
    section("v2.3 helpers: silence, mix, cubic warp, partial magnitude, deferred log");
    // silence detection
    AlignedVector z(735, 0.0f);                          // 735 = one 60 fps frame of audio
    CHECK(blockIsSilent(z.data(), z.size()));
    z[3] = -0.0f; CHECK(blockIsSilent(z.data(), z.size()));   // negative zero is silence too
    z[700] = 1e-30f; CHECK(!blockIsSilent(z.data(), z.size()));  // a denormal is NOT silence
    CHECK(blockIsSilent(z.data(), 5));                   // and the count argument is honoured (index 3 only)

    // mono mix helpers
    std::vector<float> a(100), b(100), m(100);
    for (int i = 0; i < 100; ++i) { a[i] = static_cast<float>(i); b[i] = static_cast<float>(-2 * i); }
    addInto(a.data(), b.data(), m.data(), 100);
    scaleInPlace(m.data(), 100, 0.5f);
    bool ok = true;
    for (int i = 0; i < 100; ++i) ok = ok && std::abs(m[i] - (-0.5f * i)) < 1e-6f;   // (i + -2i) * 0.5
    CHECK(ok);

    // cubic warp vs scalar Catmull-Rom reference, and identity still bypasses
    const size_t nlin = 513;                             // a 1024-point R2C transform
    AlignedVector src(nlin), out;
    // Strictly positive and smooth: the reference clamps its result at 0, so a signal that stayed
    // near zero would make the two agree for the wrong reason.
    for (size_t i = 0; i < nlin; ++i) src[i] = static_cast<float>(1.0 + std::sin(i * 0.07) * 0.5);
    PerceptualWarping w;
    w.setInterpolation(1);                               // 1 = Catmull-Rom cubic (4 taps), the path under test
    w.buildWarpTables(0, 22050.0, 1000, 22050.0, 0.963, 20.0, nlin);   // Log, 1000 out bins, the default blend
    w.applyWarp(src, out);
    const auto& hz = w.targetHz();
    double max_err = 0.0;
    int last = static_cast<int>(nlin) - 1;
    for (size_t i = 0; i < 1000; ++i) {
        double frac = hz[i] / 22050.0 * (nlin - 1);
        // The table build snaps near-integer positions to the integer before storing them (so an
        // exact 1:1 grid is detected as identity); the reference has to do the same or it compares
        // against a position the kernel never saw.
        double r = std::round(frac); if (std::abs(frac - r) < 1e-6) frac = r;
        int i0 = static_cast<int>(std::min<double>(nlin - 2, std::floor(frac)));
        float t = static_cast<float>(frac - i0);
        // The kernels clamp the two outer taps to the ends of the array; the reference clamps the
        // same way (max(i0-1, 0) / min(i0+2, last)).
        float p0 = src[std::max(i0 - 1, 0)], p1 = src[i0], p2 = src[std::min(i0 + 1, last)], p3 = src[std::min(i0 + 2, last)];
        float v = 0.5f * (2 * p1 + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t);
        max_err = std::max(max_err, static_cast<double>(std::abs(std::max(0.0f, v) - out[i])));
    }
    std::printf("  cubic warp max error vs scalar reference: %.2e\n", max_err);
    CHECK(max_err < 1e-4);                               // float rounding in the gather: the curve is smooth
    // The cubic path reads one tap either side of the linear one, so the highest bin it touches is
    // within a couple of the array end. This is the value the partial-magnitude optimisation trusts.
    CHECK(w.maxLinearIndex() <= nlin - 1 && w.maxLinearIndex() >= nlin - 3);
    w.setInterpolation(1);
    w.buildWarpTables(5, 22050.0, nlin, 22050.0, 1.0, 20.0, nlin);     // Linear, out bins == in bins
    CHECK(w.isIdentity());
    w.applyWarp(src, out);
    CHECK(out[100] == src[100]);                         // identity means memcpy: bit equality, one spot check
    // Display Max below Nyquist -> fewer magnitude bins needed
    w.buildWarpTables(0, 11025.0, 1000, 22050.0, 0.963, 20.0, nlin);   // fmax is half Nyquist here
    std::printf("  maxLinearIndex at half Nyquist: %zu of %zu\n", w.maxLinearIndex(), nlin);
    CHECK(w.maxLinearIndex() < nlin / 2 + 4);            // +4: the cubic taps either side, plus slack

    // partial magnitude: only the first n_mag bins are written
    PlanLog log;
    FFTWEngine e;
    e.prepare(1024, PlannerPolicy::Fast, &log);
    AlignedVector frame(1024, 0.0f), mag(513, -1.0f); AlignedComplexVector scratch;   // -1.0 marks "untouched"
    frame[0] = 1.0f;                                     // impulse at sample 0 -> flat magnitude 1
    e.executeRFFT(frame, mag, scratch, 100);
    CHECK_NEAR(mag[0], 1.0, 1e-5);
    CHECK_NEAR(mag[99], 1.0, 1e-5);                      // the last bin actually requested
    CHECK(mag[512] == -1.0f);                 // untouched beyond the requested (16-rounded) count
    e.executeRFFT(frame, mag, scratch, 0);
    CHECK_NEAR(mag[512], 1.0, 1e-5);                     // 0 = "all of them", so the marker is overwritten

    // deferred log: nothing hits the textport until flushed, history is kept
    PlanLog dl;
    dl.setDeferred(true);
    dl.log("a"); dl.log("b");
    CHECK(dl.size() == 2);
    CHECK(dl.flushToTextport() == 2);                    // returns how many lines it wrote
    CHECK(dl.flushToTextport() == 0);                    // and there is nothing left to write
}

// ------------------------------------------------------------------------------------------
// ===================== test_plan_log_tail =====================
// PlanLog::snapshotTail() / version() - the two accessors the info callbacks use to avoid
// re-copying the whole history per cook. The property that matters is not "it is fast" but "it
// returns exactly what snapshot() would have returned, minus the entries nobody reads": the popup
// renders those lines, so a tail that disagreed with the full snapshot would silently change what
// the middle-click shows.
//
// WHAT:  version() moves when and only when the history content changes (not when accessors are
//        called), and snapshotTail(n) is exactly the last n entries of snapshot() for every n,
//        including n larger than the history and a history that has just been truncated.
// WHY:   the equivalence is what protects the popup; the truncation case below is the one moment a
//        tail and a full snapshot could plausibly disagree, because log() drops the older half once
//        the history hits kMaxPlanLogEntries.
// HOW TO CHANGE: this test reads the cap from FFTDSP::kMaxPlanLogEntries rather than hard-coding it,
//        so changing the cap changes how long this test runs but not what it asserts. Keep it that
//        way - a literal here would silently stop exercising the truncation path.
// ------------------------------------------------------------------------------------------
static void test_plan_log_tail()
{
    section("PlanLog snapshotTail / version");

    PlanLog log;
    std::vector<std::string> full, tail;

    // Empty history: both accessors agree, and asking for more than exists is not an error.
    CHECK(log.version() == 0);                  // a fresh log is version 0, the value the cache starts from
    log.snapshotTail(3, tail);
    CHECK(tail.empty());
    log.snapshotTail(0, tail);
    CHECK(tail.empty());

    // version() must track the content, not the calls. setDeferred / flushToTextport / size do not
    // change the history, so they must not move it - that is the whole basis for caching on it.
    const uint64_t v0 = log.version();
    log.setDeferred(true);
    log.flushToTextport();
    log.size();
    CHECK(log.version() == v0);

    log.log("one", false);
    CHECK(log.version() != v0);
    const uint64_t v1 = log.version();
    log.log("two", false);
    CHECK(log.version() != v1);

    // tail for n < size: the last n, newest last
    log.log("three", false);
    log.log("four", false);
    log.snapshotTail(3, tail);
    CHECK(tail.size() == 3);
    CHECK(tail[0] == "two");
    CHECK(tail[1] == "three");
    CHECK(tail[2] == "four");

    // n >= size: the whole history
    log.snapshotTail(99, tail);                 // 99 is just "more than the 4 entries there are"
    CHECK(tail.size() == 4);
    CHECK(tail.front() == "one");
    CHECK(tail.back() == "four");

    // The equivalence that protects the popup: for every n, the tail is exactly the last n entries of
    // the full snapshot.
    full = log.snapshot();
    for (size_t n = 0; n <= full.size() + 2; ++n) {   // +2: two values of n past the end, where n >= size
        log.snapshotTail(n, tail);
        const size_t expect = n < full.size() ? n : full.size();
        CHECK(tail.size() == expect);
        for (size_t i = 0; i < expect; ++i)
            CHECK(tail[i] == full[full.size() - expect + i]);
    }

    // The case the change is actually about: a log at its cap. log() truncates by half once the history
    // reaches kMaxPlanLogEntries, so the tail is read off a history that just lost its older half - the
    // one moment where a tail and a full snapshot could plausibly disagree.
    PlanLog big;
    const size_t cap = FFTDSP::kMaxPlanLogEntries;   // read from the implementation, not hard-coded
    for (size_t i = 0; i < cap + 40; ++i)            // +40: enough entries past the cap to force one truncation
        big.log("entry_" + std::to_string(i), false);
    CHECK(big.size() <= cap);
    std::vector<std::string> bigFull, bigTail;
    bigFull = big.snapshot();
    big.snapshotTail(3, bigTail);
    CHECK(bigTail.size() == 3);
    CHECK(bigTail[0] == bigFull[bigFull.size() - 3]);
    CHECK(bigTail[1] == bigFull[bigFull.size() - 2]);
    CHECK(bigTail[2] == bigFull[bigFull.size() - 1]);
    // ...and the newest entry is still the one just logged, truncation or not
    CHECK(bigTail[2] == "entry_" + std::to_string(cap + 39));

    // clear() is a content change, so the version moves and the tail goes empty.
    const uint64_t v2 = big.version();
    big.clear();
    CHECK(big.version() != v2);
    big.snapshotTail(3, bigTail);
    CHECK(bigTail.empty());
}

// ------------------------------------------------------------------------------------------
// ===================== test_clip_line =====================
// clipLine: the popup's per-line bound.
//
// The middle-click popup is the one surface whose rendering has been observed to depend on the
// length of what it is given (~1660 characters rendered, ~1760 came up empty), and a backend
// description - which embeds the absolute path of the FFT library - runs to ~240 characters. Three
// of those made the popup's total length move by hundreds of characters from cook to cook, which is
// what made a length-dependent failure look like a random one. So the contract worth asserting is
// not only that a long line is shortened but that a short one is handed through *byte for byte*:
// the clipped text is what a user reads, and silently altering a line that already fits would be a
// worse bug than a long popup.
//
// WHAT:  clipLine(s, maxChars) returns s untouched when it fits, and otherwise maxChars characters
//        followed by a 3-character marker. Boundaries are pinned exactly (fits, one short, exactly
//        at the bound, one over, and maxChars == 0).
// WHY:   see above. The 3-character marker is the reason the clipped length is maxChars + 3 and not
//        maxChars, and the reason the "three lines" total at the bottom is 3 * 76 rather than 3 * 72.
// HOW TO CHANGE: 72 is the bound the plugin actually calls this with, so the checks below are only
//        meaningful at that value - the function itself is generic, but this test is not.
// ------------------------------------------------------------------------------------------
static void test_clip_line()
{
    section("clipLine (popup tail bound)");

    // Fits exactly: unchanged, and no marker added.
    const std::string exact(72, 'x');           // exactly the bound: the <= branch, not the substr branch
    CHECK(FFTDSP::clipLine(exact, 72) == exact);
    CHECK(FFTDSP::clipLine(exact, 72).size() == 72);

    // Shorter than the bound: unchanged.
    CHECK(FFTDSP::clipLine("short", 72) == "short");
    CHECK(FFTDSP::clipLine("", 72).empty());

    // One over the bound: trimmed to exactly the bound, then marked.
    const std::string over(73, 'y');            // one character over: the first input that is clipped
    const std::string cut = FFTDSP::clipLine(over, 72);
    CHECK(cut.size() == 72 + 3);                // see the WHAT block: "..." is appended, not substituted
    CHECK(cut.compare(0, 72, std::string(72, 'y')) == 0);
    CHECK(cut.substr(72) == "...");

    // The real shape of the input: a backend description, 240 characters, whose head is what
    // identifies the library. Whatever comes back must start with that head.
    const std::string plan =
        "fftw-3.3.11-sse2-avx-avx2-avx2_128 [C:\\Users\\Z\\Downloads\\PROJECTS\\TD_PROJECTS\\"
        "PluginBuilder\\Plugin_FFT\\__Plugins__\\FFT\\libfftw3f-3.3.11-avx2.dll, wisdom on] - plan "
        "kernels: AVX2 (256-bit vectors, FMA-capable codelets)";
    CHECK(plan.size() > 200);                   // guards that the literal above is still the long one
    const std::string clipped = FFTDSP::clipLine(plan, 72);
    CHECK(clipped.size() == 75);                // 72 + the 3-character marker
    CHECK(clipped.compare(0, 72, plan.substr(0, 72)) == 0);
    CHECK(clipped.compare(0, 12, "fftw-3.3.11-") == 0);   // 12 = the length of the version prefix that identifies it
    // The path is no longer in the popup's view of the line - which is the point: the same path is on
    // the Binary: line, and it is what made this line's length unbounded.
    CHECK(clipped.find("__Plugins__") == std::string::npos);

    // maxChars == 0 is degenerate but must not read past the string or return the whole thing.
    CHECK(FFTDSP::clipLine("abc", 0) == "...");   // 0 characters kept, marker still added

    // Three real lines at the real bound: the popup's tail contribution is now a constant, which is
    // what makes the character count in the info_callback_calls row comparable between cooks.
    size_t total = 0;
    for (int i = 0; i < 3; ++i) total += FFTDSP::clipLine(plan, 72).size() + 1;   // +1 for the newline
    CHECK(total == 3 * 76);                     // (72 + 3) + 1, times 3
}

// ------------------------------------------------------------------------------------------
// ===================== test_triple_buffer_and_signal =====================
// WHAT:  Two concurrency primitives, in three parts. Single-threaded TripleBuffer: the roles rotate
//        so a writer never touches what the reader is holding, the reader always gets the LATEST
//        published payload, and publish() reports whether it overwrote something unread. Two-thread
//        TripleBuffer: under a 200 ms producer/consumer hammering, no acquire ever sees a torn or
//        out-of-order payload. WorkerSignal: a signal issued before wait() is not lost, wait()
//        consumes it, a cross-thread signal wakes a blocked waiter, and waitFor() honours its
//        timeout with a high-resolution timer instead of the system clock tick.
// WHY:   the two-thread half is the one that matters: a torn payload is exactly the bug a lock-free
//        handoff can have and the one thing that would be invisible in a single-threaded test. The
//        non_monotonic counter guards the other half of "latest wins" - that a payload cannot go
//        backwards in time.
// HOW TO CHANGE: the 256-float payload is sized so that a torn read is easy to produce but the
//        whole payload still fits in cache (the test has to lose the race often enough to be
//        meaningful). The timings below are deliberately generous - this is a smoke test with real
//        threads on a real scheduler, not a benchmark - so do not tighten them into flakes.
static void test_triple_buffer_and_signal()
{
    section("TripleBuffer / WorkerSignal (v2.4 lock-free handoff)");
    struct Payload { uint64_t a{ 0 }, b{ 0 }; std::vector<float> data; };   // a and b must always be equal; data follows a

    // single thread: roles rotate, latest wins, dropped flag
    TripleBuffer<Payload> tb;
    CHECK(!tb.acquire());                       // nothing published yet
    CHECK(tb.front().a == 0);                   // front is still readable before the first acquire
    tb.back().a = 1; tb.back().b = 1;
    CHECK(!tb.publish());                       // nothing was pending -> not dropped
    tb.back().a = 2; tb.back().b = 2;
    CHECK(tb.publish());                        // consumer did not take #1 -> dropped
    CHECK(tb.acquire());
    CHECK(tb.front().a == 2);                   // latest wins
    CHECK(!tb.acquire());                       // consumed
    CHECK(tb.front().a == 2);                   // front stays valid until the next acquire
    // the three slots are always distinct roles: writing back never touches front
    Payload* f = &tb.front();
    for (int i = 0; i < 10; ++i) { tb.back().a = 100 + i; CHECK(&tb.back() != f); tb.publish(); }
    CHECK(f->a == 2);                           // the pointer taken above still holds the old payload
    CHECK(tb.acquire() && tb.front().a == 109);   // 100 + 9: the last value written in the loop

    // two threads: the consumer must never observe a torn payload (a != b or data[k] != a)
    TripleBuffer<Payload> tb2;
    // Every slot is filled up front: the producer below writes only the first kSlots of the vector,
    // so a slot that started empty would make the length check trivially true.
    for (size_t i = 0; i < TripleBuffer<Payload>::kSlots; ++i) tb2.slot(i).data.assign(256, 0.0f);
    std::atomic<bool> stop{ false };
    std::atomic<uint64_t> produced{ 0 };
    std::thread producer([&] {
        for (uint64_t n = 1; !stop.load(); ++n) {
            Payload& p = tb2.back();
            p.a = n;
            for (auto& v : p.data) v = static_cast<float>(n & 0xFFFF);   // 0xFFFF: exact in a float, so the compare below is bit-exact
            p.b = n;
            tb2.publish();
            produced.store(n);
        }
    });
    uint64_t last = 0, acquired = 0, torn = 0, non_monotonic = 0;
    auto t_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);   // a fixed budget: the counts, not the duration, are what is checked
    while (std::chrono::steady_clock::now() < t_end) {
        if (tb2.acquire()) {
            const Payload& p = tb2.front();
            if (p.a != p.b) ++torn;
            for (float v : p.data) if (v != static_cast<float>(p.a & 0xFFFF)) { ++torn; break; }
            if (p.a < last) ++non_monotonic;
            last = p.a;
            ++acquired;
        }
    }
    stop.store(true);
    producer.join();
    std::printf("  produced %llu, consumer acquired %llu, torn %llu, non-monotonic %llu\n",
                static_cast<unsigned long long>(produced.load()), static_cast<unsigned long long>(acquired),
                static_cast<unsigned long long>(torn), static_cast<unsigned long long>(non_monotonic));
    CHECK(acquired > 0);                        // the loop actually ran: guards the two checks below from being vacuous
    CHECK(torn == 0);
    CHECK(non_monotonic == 0);
    tb2.acquire();                                  // drain (not a check: either outcome is fine)
    CHECK(tb2.front().a == produced.load());        // the very last publish is visible after the producer stopped

    // WorkerSignal: a signal issued before wait() is not lost; wait() consumes it; cross-thread wake works
    WorkerSignal sig;
    sig.signal();
    auto t0 = std::chrono::steady_clock::now();
    sig.wait();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(50));   // 50 ms: a real wait would be far longer than this
    std::atomic<int> woke{ 0 };
    std::thread waiter([&] { sig.wait(); woke.store(1); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));   // 20 ms: long enough for the thread to reach wait()
    CHECK(woke.load() == 0);                        // still blocked: the earlier signal was consumed
    sig.signal();
    waiter.join();
    CHECK(woke.load() == 1);
    // waitFor: times out close to the requested 2 ms even when the system clock ticks at 15.6 ms
    // (high-resolution waitable timer), and returns true immediately when a signal is pending
    // 200 waits, judged on the 95th percentile: a single scheduling hiccup (measured: an occasional
    // ~19 ms outlier on a loaded machine) is not what this checks, and a max-of-20 bound made the suite
    // fail 2 runs in 5. A coarse 15.6 ms clock would put the p95 at ~15.6 ms, so 6 ms still separates a
    // working high-resolution timer from a broken one.
    std::vector<double> waits;
    int timeouts = 0;
    for (int i = 0; i < 200; ++i) {
        auto s = std::chrono::steady_clock::now();
        if (!sig.waitFor(2)) ++timeouts;            // 2 ms request; nothing signals it, so every call must time out
        waits.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s).count());
    }
    std::sort(waits.begin(), waits.end());
    const double p95_ms = waits[waits.size() * 95 / 100], min_ms = waits.front(), worst_ms = waits.back();
    std::printf("  waitFor(2 ms): min %.2f / p95 %.2f / worst %.2f ms over 200 calls (high-res timer: %s)\n",
                min_ms, p95_ms, worst_ms, sig.highResolutionTimer() ? "yes" : "no");
    CHECK(timeouts == 200);
    CHECK(min_ms >= 1.0);                           // it did wait, and did not return early
    if (sig.highResolutionTimer()) CHECK(p95_ms < 6.0);
    sig.signal();
    CHECK(sig.waitFor(1000));                       // a pending signal returns true instead of timing out
}

// ------------------------------------------------------------------------------------------
// ===================== test_pipeline_sine =====================
// WHAT:  The whole window -> pad -> FFT -> warp chain on a known 1 kHz sine at 44.1 kHz, once per
//        magnitude normalization: the spectrum has the right length, the peak lands on 1 kHz, its
//        height is the value the two normalizations predict, and the warped log grid puts the peak
//        back at 1 kHz as well.
// WHY:   this is the closest this suite gets to an end-to-end check: it is the test that would catch
//        a window, a pad offset or an FFT factor being wrong by something other than a rounding
//        error. The two peak-height checks are the two documented normalizations' definitions.
// HOW TO CHANGE: win = 3175 is the default window (72 ms at 44.1 kHz); N = 32768 is the "Analysis"
//        preset's pad (the default pad is 16384). If you change one, check the two expected magnitudes below -
//        0.5 and 0.5 * win / 2 both depend on win, and the second is the legacy scale's whole point.
static void test_pipeline_sine()
{
    section("FFTWEngine pipeline (1 kHz sine @ 44.1 kHz)");
    const double sr = 44100.0, f0 = 1000.0;         // a plain tone, one bin's worth of ambiguity at most
    const size_t win = 3175, N = 32768;             // the default window length and the Analysis preset's FFT size
    PlanLog log;
    FFTWEngine engine;
    engine.prepare(N, PlannerPolicy::Fast, &log);   // Fast: this test measures the numbers, not the planner
    CHECK(engine.fftSize() == N);

    for (int norm = 0; norm < 2; ++norm) {          // 0 = CoherentGain (legacy), 1 = FullScale
        AlignedVector window;
        WindowGenerator::generateWindow(1 /*Hann*/, 15.0, win, window, norm ? WindowNorm::FullScale : WindowNorm::CoherentGain);
        AlignedVector frame(N, 0.0f), sig(win);
        for (size_t i = 0; i < win; ++i) sig[i] = static_cast<float>(0.5 * std::sin(2.0 * PI_D * f0 * i / sr));   // amplitude 0.5, in a double before the cast
        // Zero-pad symmetrically around a 32768-point frame; the & ~7 rounds the start down to a
        // multiple of 8 so the window multiply below stays on the vector path.
        size_t pad_start = ((N - win) / 2) & ~static_cast<size_t>(7);
        multiplyInto(sig.data(), window.data(), frame.data() + pad_start, win);

        AlignedVector mag; AlignedComplexVector scratch;
        engine.executeRFFT(frame, mag, scratch);
        CHECK(mag.size() == N / 2 + 1);             // 16385 for a real-to-complex transform of 32768
        size_t idx = 0;
        float pk = findPeakWithIndex(mag.data(), mag.size(), idx);
        double peak_hz = idx * sr / N;              // bin index -> Hz on the unwarped grid
        std::printf("  norm=%s peak %.1f Hz, magnitude %.4f\n", norm ? "FullScale" : "CoherentGain", peak_hz, pk);
        // Two bins plus 1 Hz: the bin spacing here is sr/N = 1.35 Hz and the peak of a windowed sine
        // can sit a bin or so away from the true tone without anything being wrong.
        CHECK_NEAR(peak_hz, f0, 2.0 * sr / N + 1.0);
        // The two normalizations, stated as the amplitudes they are meant to read back:
        // FullScale: a sine of amplitude A reads A, so 0.5. CoherentGain: the legacy scale, where a
        // windowed full-scale sine peaks at N_win / 2. The tolerances scale with the value, so both
        // are relative-ish (0.02 absolute vs 2% of the ~794 expected here).
        if (norm) CHECK_NEAR(pk, 0.5, 0.02);                    // amplitude 0.5 -> 0.5
        else      CHECK_NEAR(pk, 0.5 * win / 2.0, 0.02 * win);   // legacy: A * N_win / 2

        // warped log grid must place the peak at ~1 kHz too
        PerceptualWarping w;
        // Log scale, full band (fmax == nyquist == 22050 = sr/2), 16384 output bins, the default
        // 0.963 blend, 20 Hz log floor, gathering from the 16385 linear bins.
        w.buildWarpTables(0, 22050.0, 16384, sr / 2.0, 0.963, 20.0, N / 2 + 1);
        AlignedVector warped;
        w.applyWarp(mag, warped);
        float wpk = findPeakWithIndex(warped.data(), warped.size(), idx);
        (void)wpk;                                  // the peak VALUE is not what is checked here, only where it landed
        // 15 Hz is deliberately loose: this is the warped bin centre nearest the peak, not an
        // interpolated peak position, so the bound describes the axis model rather than the tone.
        CHECK_NEAR(w.targetHz()[idx], f0, 15.0);
    }
    CHECK(log.size() >= 1);                         // prepare() said something worth reading in the Info DAT
}

// ------------------------------------------------------------------------------------------
// ===================== test_pipeline_process =====================
// WHAT:  AnalysisPipeline::process() driven the way the node drives it - a Parameters::Values
//        snapshot plus an AnalysisJob - through three scenarios that share one pipeline instance:
//        1. one channel, linear grid, so the output width, the peak and the whole Status block are
//           checked against the parameter values that produced them;
//        2. a second pass on the same instance with reset = false, to show the plan is reused;
//        3. three channels, which takes the std::execution::par fan-out, and must give the same peak;
//        4. a silent channel, which must short-circuit to an all-zero spectrum.
// WHY:   not recorded in this file beyond the inline notes. The axis-rate check below is the one
//        that carries a contract (2 * fmax), because that number is published to TouchDesigner and is
//        NOT info->sampleRate - see the note next to the check.
// HOW TO CHANGE: the three section banners in this function print in the middle of the body, and the
//        first one sits after the first process() call rather than at the top - moving them changes
//        the printed order, so leave them where they are unless you mean to change the output.
static void test_pipeline_process()
{
    const double sr = 44100.0, f0 = 1000.0;
    const int win_samples = 3175;                       // 72 ms at 44.1 kHz, the default window
    const int N = 32768;                                // the Analysis preset's pad (default is 16384)
    const int bins = N / 2 + 1;                         // 16385 → identity warp

    Parameters::Values p;
    p.scale     = Parameters::Scale::Linear;
    p.warp      = 0.0;                                  // blend 0 + Linear + bins == nlin: the linear grid
    p.bins      = bins;
    p.binsMode  = Parameters::BinsMode::Fixed;          // exactly `bins` (Auto would size from the window)
    p.padSize   = N;
    p.winMode   = Parameters::WinMode::Samples;
    p.winSamples = win_samples;
    p.window    = Parameters::WindowType::Hann;
    p.magNorm   = Parameters::MagNorm::CoherentGain;    // the legacy scale, so the peak check below is the legacy one
    p.loudness  = Parameters::Loudness::Off;            // weighting dB, ballistics and EQ are all off: this
    p.weighting = Parameters::Weighting::Off;           // test is about the pipeline's structure, not the chain
    p.ballEnable = false;
    p.eqEnable   = false;

    PlanLog log;
    AnalysisPipeline pipeline(&log);

    AlignedVector sig(win_samples);
    for (int i = 0; i < win_samples; ++i)
        sig[i] = static_cast<float>(0.5 * std::sin(2.0 * PI_D * f0 * i / sr));   // amplitude 0.5, as elsewhere

    AnalysisJob job;
    job.numChannels = 1;
    job.sampleRate  = sr;
    job.winSamples  = win_samples;
    job.dtMs        = 1000.0 / 60.0;                    // one 60 fps frame
    job.reset       = true;
    job.p           = p;
    job.windows.push_back(sig);                         // one channel: windows and silent are indexed by channel
    job.silent.push_back(0);

    AnalysisResult res;
    pipeline.process(job, res);

    // (prints here rather than at the top of the function; see HOW TO CHANGE)
    section("AnalysisPipeline::process() — 1 kHz sine, single channel");
    CHECK(res.spectra.size() == 1);                     // one spectrum per channel
    CHECK(res.spectra[0].size() == static_cast<size_t>(bins));
    CHECK(res.peakMag > 0.0f);                          // the channel is not silent, so the peak must be real
    // Same tolerance as test_pipeline_sine: two bins (sr/N = 1.35 Hz) plus 1 Hz.
    CHECK_NEAR(res.peakHz, f0, 2.0 * sr / N + 1.0);

    AnalysisPipeline::Status st = pipeline.status();
    CHECK(!st.plan.empty());                            // a description of the live plan, for the Info DAT
    CHECK(st.fftSize == static_cast<size_t>(N));        // padSize won: fftSizeFrom(p, 3175) == 32768
    CHECK(st.capacity == static_cast<size_t>(win_samples));   // the FIFO is exactly one window long
    CHECK(st.outputBins == bins);
    CHECK_NEAR(st.axisRate, sr, 1.0);                  // 2 * fmax, fmax clamped to Nyquist = sr/2 → sr
    CHECK(st.linearGrid);                              // Linear + warp=0 + bins==nlin → identity

    // Second pass: plan reused, no rebuild, peak stable.
    job.seq = 2; job.reset = false;
    pipeline.process(job, res);
    CHECK_NEAR(res.peakHz, f0, 2.0 * sr / N + 1.0);

    // Multi-channel: the parallel path (std::execution::par) must produce the same peak.
    section("AnalysisPipeline::process() — multi-channel parallel fan-out");
    job.numChannels = 3;                                // > 1 channel is what switches the fan-out on
    job.windows.assign(3, sig);
    job.silent.assign(3, 0);
    AnalysisResult res3;
    pipeline.process(job, res3);
    CHECK(res3.spectra.size() == 3);
    for (size_t c = 0; c < 3; ++c)
        CHECK_NEAR(res3.peakHz, f0, 2.0 * sr / N + 1.0);   // one peak for the whole result, so this is really
                                                          // "the reported peak is still the tone" per iteration
    CHECK(pipeline.parallelActive());                   // and the parallel path is what ran

    // Silence short-circuit: linear-magnitude path zeroes the output.
    section("AnalysisPipeline::process() — silence short-circuit");
    job.numChannels = 1;
    job.silent[0] = 1;                                  // declared silent by the ingest side
    AlignedVector zeros(win_samples, 0.0f);
    job.windows.assign(1, zeros);                       // and actually all-zero, so the two agree
    job.reset = false;
    AnalysisResult res0;
    pipeline.process(job, res0);
    CHECK(res0.spectra.size() == 1);
    bool all_zero = true;
    for (float v : res0.spectra[0]) if (v != 0.0f) all_zero = false;
    CHECK(all_zero);                                    // skipped, not merely quiet
}

// ------------------------------------------------------------------------------------------
// ===================== test_identity_grid_and_rate =====================
// The linear-grid ("no resampling") case is not a mode of its own: Scale = Linear + Warp Blend = 0
// + Display Max >= Nyquist + Output Bins = nlin makes the warp come out as the identity, and
// applyWarp() then memcpy's the magnitude through untouched. These checks pin that equivalence and
// the frequency-axis model that goes with it.
//
// WHAT:  Two things at once, because they are the same statement seen from two sides. (1) The
//        identity grid: targetHz() is exactly i*sr/1024 for a 513-bin grid, the top bin sits on
//        Nyquist, and applyWarp() copies bit for bit. (2) The axis model that goes with it:
//        axisRate = 2 * (top of the axis), so the last bin sits on Nyquist - which is what makes
//        axisRate equal the input rate ONLY while the band runs to Nyquist, and twice Display Max
//        whenever it does not. Also pins the Bark round trip, which used to fold over past ~6.5 kHz.
// WHY:   axisRate is published to TouchDesigner (as hz_per_sample / output_spectrum_axis) and is a
//        different number from info->sampleRate, which is bins x me.time.rate. Getting that
//        distinction wrong was a real bug - see the note in test_rate_model.
// HOW TO CHANGE: 1024 is the FFT size the 513-bin grid belongs to; it appears as a literal here
//        (sr / 1024.0) because the grid does not carry its own FFT size. If nlin changes, every
//        1024 below has to change with it.
static void test_identity_grid_and_rate()
{
    section("linear grid (identity warp, no resampling) + spectrum frequency axis");
    const size_t nlin = 513;                        // a 1024-point R2C transform
    const double sr = 44100.0, nyq = sr / 2.0;
    AlignedVector src(nlin), out;
    for (size_t i = 0; i < nlin; ++i) src[i] = static_cast<float>(std::sin(i * 0.03) * 5.0 + 2.0);
    PerceptualWarping w;

    // --- linear grid: the output grid IS the linear FFT grid, copied verbatim ---
    w.buildWarpTables(5 /*Linear*/, nyq, nlin, nyq, 0.0, 20.0, nlin);   // blend 0.0 is the "no resampling" half
    CHECK(w.isIdentity());
    CHECK(w.outputBins() == nlin);
    // every bin must land exactly on its own linear index: bin i of an N-point R2C transform is i*sr/N
    double max_bin_err = 0.0;
    for (size_t i = 0; i < nlin; ++i) {
        max_bin_err = std::max(max_bin_err, std::abs(w.targetHz()[i] - i * sr / 1024.0));
    }
    std::printf("  linear grid: max bin freq error %.2e Hz (bin spacing %.2f Hz)\n", max_bin_err, sr / 1024.0);
    CHECK(max_bin_err < 1e-9);                      // 1e-9: the grid is built in double, so this is exact
    CHECK_NEAR(w.targetHz()[nlin - 1], nyq, 1e-9);          // top bin sits on Nyquist
    w.applyWarp(src, out);
    bool identical = out.size() == nlin;
    for (size_t i = 0; identical && i < nlin; ++i) identical = (out[i] == src[i]);   // bit equality: it is a memcpy
    CHECK(identical);

    // --- axis model: axisRate = 2 * (top of the axis), so the last bin sits on Nyquist ---
    // Full-band grid: fmax = nyquist, so the axis rate is exactly the input rate, and the implied
    // spacing axisRate/(2*(nlin-1)) is exactly the 1024-point transform's own resolution. This is
    // what hz_per_sample / output_spectrum_axis report; info->sampleRate is bins x me.time.rate.
    const double raw_axis = 2.0 * w.targetHz()[w.outputBins() - 1];
    CHECK_NEAR(raw_axis, sr, 1e-9);
    // nlin - 1 = 512, so raw_axis / (2*512) is 44100/1024: the same number from the other direction.
    CHECK_NEAR(raw_axis / (2.0 * static_cast<double>(nlin - 1)), sr / 1024.0, 1e-9);
    CHECK(raw_axis == 44100.0);                              // same band as the input signal

    // Linear grid held to Display Max below Nyquist: the axis stops at Display Max, so the axis
    // rate is twice that, whatever the bin count. This is where it stops equalling the input rate.
    {
        const double fmax = 10000.0;                // a Display Max well below the 22050 Nyquist
        const size_t n_out = 1000;                  // 1000 != 513, so this is the gather path
        w.buildWarpTables(5, fmax, n_out, nyq, 0.0, 20.0, nlin);
        CHECK(!w.isIdentity());                              // 1000 bins gathered from 513: not 1:1
        CHECK_NEAR(w.targetHz()[0], 0.0, 1e-12);             // a Linear axis starts at DC
        CHECK_NEAR(w.targetHz()[n_out - 1], fmax, 1e-9);     // last bin sits exactly on Display Max
        CHECK_NEAR(2.0 * w.targetHz()[n_out - 1], 20000.0, 1e-9);   // 2 * 10000: the axis rate
        CHECK_NEAR(w.targetHz()[n_out - 1] / static_cast<double>(n_out - 1), 10000.0 / 999.0, 1e-9);
        // More bins over the same band: the count of bins describing the band changes, the band
        // does not, so the axis rate must not move with Output Bins.
        // 4000 and 257 are arbitrary bin counts chosen only to be far from 1000 in both directions.
        w.buildWarpTables(5, fmax, 4000, nyq, 0.0, 20.0, nlin);
        CHECK_NEAR(2.0 * w.targetHz()[3999], 20000.0, 1e-9);
        w.buildWarpTables(5, fmax, 257, nyq, 0.0, 20.0, nlin);
        CHECK_NEAR(2.0 * w.targetHz()[256], 20000.0, 1e-9);
    }
    // A full-Nyquist band over 1000 bins: same 0..nyquist band as the input, so the axis rate is
    // the input rate for every scale, whatever Order the bins land in.
    // 7 is every scale_code computeTargetHzGrid knows (0 Log .. 6 Mel+Log); see that switch.
    for (int scale = 0; scale < 7; ++scale) {
        w.buildWarpTables(scale, nyq, 1000, nyq, 0.0, 20.0, nlin);
        CHECK_NEAR(2.0 * w.targetHz()[999], sr, 1e-9);   // 999 = the last of the 1000 bins
    }
    // Every scale is monotonic and ends exactly on fmax, which is what makes axisRate = 2*fmax exact
    // at the top of the axis even where the bins in between are non-uniform.
    for (int scale = 0; scale < 7; ++scale) {
        w.buildWarpTables(scale, 16000.0, 2000, nyq, 1.0, 20.0, nlin);   // blend 1.0: the pure Scale axis
        CHECK(std::is_sorted(w.targetHz().begin(), w.targetHz().end()));
        CHECK_NEAR(w.targetHz()[1999], 16000.0, 1e-6);   // 1999 = the last of the 2000 bins
        CHECK_NEAR(2.0 * w.targetHz()[1999], 32000.0, 1e-6);   // 2 * fmax
    }
    // Bark used to fold over past ~6.5 kHz (barkToHz divided by 0.78 where the inverse of
    // hzToBark's 1.22*z-4.422 needs 1.22), which left the top bin back down at 0 Hz.
    {
        // 6543 sits just past the ~6.5 kHz where the old formula started folding; the rest bracket
        // the range - DC, the log floor, the 1 kHz anchor, both sides of the fold, and Nyquist.
        const double f[] = { 0.0, 20.0, 1000.0, 6543.0, 8000.0, 16000.0, 22050.0 };
        for (double f_hz : f) {
            CHECK_NEAR(PerceptualWarping::barkToHz(PerceptualWarping::hzToBark(f_hz)), f_hz, 1e-6);
        }
    }
    // A perceptual scale reads a narrowed band, so fewer magnitude bins are needed than the FFT has.
    w.buildWarpTables(0, 1000.0, 2000, nyq, 1.0, 20.0, nlin);   // Log axis that stops at 1 kHz
    CHECK(w.maxLinearIndex() < nlin / 20 + 4);          // 513/20 + 4: a 1 kHz band needs a small fraction of the bins
    CHECK(!w.isIdentity());

    // --- end to end: a sine at bin 100 of a 1024-point FFT reads back as bin 100's exact Hz ---
    {
        PlanLog log;
        FFTWEngine e;
        e.prepare(1024, PlannerPolicy::Fast, &log);
        const size_t bin = 100;                         // an arbitrary but non-trivial bin index
        const double f0 = bin * sr / 1024.0;            // 100 * 44100/1024 = 4306.64 Hz
        AlignedVector frame(1024, 0.0f), mag, raw;
        AlignedComplexVector scratch;
        // An integer number of cycles is not required: no window is applied, because the point here
        // is the bin-to-Hz mapping, not spectral leakage.
        for (size_t i = 0; i < 1024; ++i) frame[i] = static_cast<float>(std::sin(2.0 * PI_D * f0 * i / sr));
        e.executeRFFT(frame, mag, scratch);                 // no window: the sine lands in one bin
        PerceptualWarping rw;
        rw.buildWarpTables(5, nyq, mag.size(), nyq, 0.0, 20.0, mag.size());   // Linear over the FFT's own 513 bins
        CHECK(rw.isIdentity());
        rw.applyWarp(mag, raw);
        size_t idx = 0;
        findPeakWithIndex(raw.data(), raw.size(), idx);
        CHECK(idx == bin);                                  // the peak is where the sine was put
        CHECK_NEAR(rw.targetHz()[idx], f0, 1e-9);           // 1e-9: the grid is exact, so this is not a tolerance
        // the raw grid's axis is 2*nyquist = the input rate, so bin i reads back at i*axisRate/N Hz
        const double axis_rate = 2.0 * rw.targetHz()[rw.outputBins() - 1];
        CHECK_NEAR(axis_rate, sr, 1e-9);
        CHECK_NEAR(idx * axis_rate / 1024.0, f0, 1e-9);     // 1024 = the transform size, as above
    }
}

// ------------------------------------------------------------------------------------------
// ===================== test_rate_model =====================
// WHAT:  RateModel.h - the pure functions behind every number the node reports about rate, size and
//        spacing: outputBinCountFrom, sampleRateToTouchDesigner, axisRate, hzPerBin, throughput,
//        fftSizeFrom and windowSamplesFrom - each on the inputs where its behaviour changes (at a
//        clamp, at a fallback, at a degenerate size).
// WHY:   recorded inline, and the important one is the reported-rate check: it must depend on the
//        bin count and the cook rate and NOT on the input sample rate, which was the v2.5.0/2.6.0
//        mistake. The two calls with different cook rates below are that bug, written down.
// HOW TO CHANGE: these are TD-free restatements of contracts that also live on the node side. If a
//        parameter default moves (bins, displayMax, winMs, padSize), the expectations here move with
//        it - they are all stated in terms of the parameter, not as literal expected outputs.
static void test_rate_model()
{
    section("RateModel (TD-free sample-rate / axis model)");
    Parameters::Values p;            // defaults: Scale=Log, Display Max=24000, Bins=16384, WinMode=Samples
                                     // (3175 samples = 72 ms), pad=16384 (index 4 of kPadValues)
    const double sr = 48000.0;       // deliberately not 44100, so a hard-coded rate would show up here

    // outputBinCountFrom is the single source of truth for the output width. Fixed: exactly Output Bins
    // (Auto is covered by test_v210_rate_helpers).
    p.binsMode = Parameters::BinsMode::Fixed;
    CHECK(outputBinCountFrom(p, sr) == p.bins);

    // Reported sample rate is bins * cook_rate — and crucially must NOT depend on the input sample
    // rate (that was the v2.5.0/2.6.0 mistake). Same params + rate, different input → same number.
    p.bins = 16384;
    const double td_rate_low  = sampleRateToTouchDesigner(p, sr, 60.0);   // 60 fps cook
    const double td_rate_high = sampleRateToTouchDesigner(p, sr, 30.0);   // 30 fps cook
    CHECK(td_rate_low  == 16384.0 * 60.0);                            // exact equality: it is a product, not a measurement
    CHECK(td_rate_high == 16384.0 * 30.0);

    // Axis rate: 2 * min(Display Max, Nyquist). Default Display Max=24000 >= Nyquist(48000/2=24000) → sr_in.
    CHECK_NEAR(axisRate(p, sr, 0.0), sr, 1e-6);
    // Display Max below Nyquist clamps the band to 2*Display Max (this node stops at Display Max).
    p.displayMax = 10000.0;
    CHECK_NEAR(axisRate(p, sr, 0.0), 20000.0, 1e-6);                  // 2 * 10000, not 48000
    // A published axis rate from the live tables wins and is returned verbatim (never jumps).
    CHECK_NEAR(axisRate(p, sr, 31415.0), 31415.0, 1e-9);              // the literal is arbitrary: any value would do
    // Non-positive rate falls back through to the scalar sample rate.
    CHECK(axisRate(p, 0.0, 0.0) == 0.0);

    // Hz-per-bin on a uniform grid is fmax/(bins-1) == axis_rate/(2*(bins-1)).
    p.displayMax = 24000.0;
    p.bins = 1025;                       // e.g. fft_size 2048 → 1025 linear bins, identity grid
    CHECK_NEAR(hzPerBin(p, sr, 0.0), (sr * 0.5) / (1025 - 1), 1e-6);  // 2*24000 bins over the band: 48000/(2*1024)
    // n_out < 2 → undefined spacing, report 0.
    p.bins = 1;                          // the smallest degenerate width
    CHECK(hzPerBin(p, sr, 0.0) == 0.0);

    // Throughput = bins * 1000 / dt_ms (measured, not nominal-rate).
    p.bins = 16384;
    CHECK_NEAR(throughput(p, sr, 16.6667), 16384.0 * 1000.0 / 16.6667, 1e-6);   // 16.6667 ms = one 60 fps frame
    CHECK(throughput(p, sr, 0.0) == 0.0);              // a zero frame time has no rate: report 0, not infinity

    // fftSizeFrom: next power of two >= winSamples, and >= padSize.
    Parameters::Values q;
    q.padSize = 32768;
    CHECK(fftSizeFrom(q, 3175) == 32768);          // pad wins (32768 > nextpow2(3175)=4096)
    q.winSamples = 50000;                          // larger than any pad: the window now sets the size
    CHECK(fftSizeFrom(q, 50000) == 65536);         // window needs 65536 (next pow2 > 50000)
    // 256 and 1 are the smallest sizes this can be exercised at without leaving the power-of-two path.
    q.padSize = 256;
    CHECK(fftSizeFrom(q, 1) == 256);              // pad alone (256 >= nextpow2(1)=1)

    // windowSamplesFrom: ms mode clamps and rounds; sample mode is the raw value.
    Parameters::Values r;
    r.winMode = Parameters::WinMode::Milliseconds;
    r.winMs = 72.0;                                // 72 ms at 48 kHz -> 3456 samples, rounded not truncated
    CHECK(windowSamplesFrom(r, sr) == static_cast<int>(std::lround(72.0 * sr / 1000.0)));
    r.winMs = 200000.0;                           // clamps to kMaxWinSamples
    CHECK(windowSamplesFrom(r, sr) == Parameters::kMaxWinSamples);
    r.winMode = Parameters::WinMode::Samples;
    CHECK(windowSamplesFrom(r, sr) == r.winSamples);   // the sample count is passed through untouched
}

// ------------------------------------------------------------------------------------------
// ===================== test_equal_loudness =====================
// WHAT:  EqualLoudness::computeCurve() for the three weightings, on a fixed frequency list:
//        A and C are anchored at 1.0 (0 dB) at 1 kHz, A attenuates 31.5 Hz far more than C does, A
//        has its small peak just above 1 kHz, and ITU-R 468 rises to its ~6.3 kHz peak and then
//        falls. Every weight is bounded as a legal positive linear magnitude.
// WHY:   the anchor at 1 kHz is the property everything downstream relies on, so it is asserted
//        exactly; the rest are shape invariants, chosen because they hold for any correct A/C curve
//        and so do not need a dB table to be re-derived whenever the implementation is touched.
// HOW TO CHANGE: the frequency list is indexed directly (a[5] is 1 kHz, a[6] is 2 kHz, g[7] is
//        6.3 kHz), so inserting a frequency in the middle renumbers every index below - which is
//        why the list is fixed and commented rather than generated.
static void test_equal_loudness()
{
    section("EqualLoudness (A/C/468) golden vectors");
    // freqs[5] = 1 kHz (the normalisation anchor for A and C).
    std::vector<double> freqs = { 31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 6300.0, 10000.0 };
    AlignedVector a, c, g;
    EqualLoudness::computeCurve(1, freqs, a);   // A
    EqualLoudness::computeCurve(2, freqs, c);    // C
    EqualLoudness::computeCurve(3, freqs, g);    // ITU-R 468

    // A and C are normalized so 1 kHz == 0 dB (weight 1.0) by construction (inv_ref). Lock that in —
    // it is the property downstream relies on.
    CHECK_NEAR(a[5], 1.0, 1e-6);
    CHECK_NEAR(c[5], 1.0, 1e-6);

    // Shape invariants (true for any correct A/C weighting, independent of an exact dB table):
    // A rolls off steeper than C at low frequencies (A(31.5) << C(31.5)), and C stays closer to 1.0
    // than A at high frequencies (C(10k) nearer 1 than A(10k)). A @ 31.5 Hz is ~-40 dB (weight ~0.01).
    CHECK(a[0] < c[0]);                          // A(31.5) more attenuated than C(31.5)
    CHECK(a[0] < 0.02);                           // deep low-frequency attenuation (~-40 dB); 0.02 is a 2x margin over ~0.01
    // Every weight is a legal linear magnitude in (0, inf); sanity-bound the whole curve so a refactor
    // can't silently invert or explode a band.
    // 100.0 is a loose ceiling: no weighting in this range legitimately amplifies by 40 dB.
    for (float w : a) { CHECK(w > 0.0f && w < 100.0f); }
    for (float w : c) { CHECK(w > 0.0f && w < 100.0f); }
    // A has its small peak just above 1 kHz, so A(2000) > A(1000) = 1.
    CHECK(a[6] > 1.0);

    // ITU-R 468: rises toward its ~6.3 kHz peak, then falls — check the shape, not absolute dB
    // (the implemented normalisation does not sit at 0 dB at 1 kHz).
    CHECK(g[5] > 0.0 && g[7] > 0.0);
    CHECK(g[7] > g[5]);   // 6300 Hz > 1000 Hz
    CHECK(g[7] > g[8]);   // past the peak: 6300 Hz > 10000 Hz
}


// ------------------------------------------------------------------------------------------
// ===================== v2.10 tests =====================
// ------------------------------------------------------------------------------------------

// Auto Kaiser beta, main-lobe width, Auto bins, presets, and targetHzAt == the built grid.
static void test_v210_rate_helpers()
{
    section("v2.10 rate helpers: Auto beta, Auto bins, presets, targetHzAt");
    // Kaiser's relation: 80 dB -> 10.7 (Values default dB range), 114 dB <-> the old beta 15.
    CHECK_NEAR(kaiserBetaForSidelobeDb(80.0), 0.12438 * 86.3, 1e-9);
    CHECK_NEAR(kaiserBetaForSidelobeDb(114.3), 15.0, 0.01);
    CHECK(kaiserBetaForSidelobeDb(10.0) == 0.0);              // below 13.26 dB: rectangular is enough
    Parameters::Values p;
    CHECK(p.betaMode == Parameters::BetaMode::Manual);        // the default keeps the pre-2.10 window
    CHECK(effectiveKaiserBeta(p) == p.kaiserBeta);
    p.betaMode = Parameters::BetaMode::Auto;
    CHECK_NEAR(effectiveKaiserBeta(p), kaiserBetaForSidelobeDb(p.dbRange), 1e-12);
    p.betaMode = Parameters::BetaMode::Manual;
    CHECK_NEAR(mainLobeHalfWidthBins(p), std::sqrt(1.0 + (15.0 / PI_D) * (15.0 / PI_D)), 1e-12);
    p.window = Parameters::WindowType::Hann;
    CHECK(mainLobeHalfWidthBins(p) == 2.0);

    // The DEFAULT is Auto with Zero-Padding on: N/2+1 of the padded FFT (pad 16384 -> 8193), whatever
    // Output Bins or the window say.
    {
        Parameters::Values a0;
        CHECK(a0.binsMode == Parameters::BinsMode::Auto && a0.zeroPad && !a0.rawBins);
        CHECK(outputBinCountFrom(a0, 44100.0) == 8193);
        a0.winSamples = 4096; CHECK(outputBinCountFrom(a0, 44100.0) == 8193);   // the reported 3081 case
        a0.bins = 777;        CHECK(outputBinCountFrom(a0, 44100.0) == 8193);
    }
    // Fixed: Output Bins is the output sample count, whatever the window or the pad - including far more
    // bins than the zero-padded FFT's N/2+1.
    {
        Parameters::Values f0; f0.binsMode = Parameters::BinsMode::Fixed;
        CHECK(outputBinCountFrom(f0, 44100.0) == f0.bins);
        f0.bins = 65536; f0.padSize = 8192;                    // 8x more bins than the 4097 FFT bins
        CHECK(outputBinCountFrom(f0, 44100.0) == 65536);
        f0.winSamples = 512;                                   // the window never changes a Fixed count
        CHECK(outputBinCountFrom(f0, 44100.0) == 65536);
        Parameters::Values pr = f0; pr.preset = Parameters::Preset::Visual120; applyPreset(pr);
        CHECK(pr.bins == 65536 && pr.binsMode == Parameters::BinsMode::Fixed);   // presets never touch the count
    }
    // Auto = the rfft's own N/2+1 of the (zero-padded) transform; Output Bins and the window's
    // resolution play no part. Raw RFFT Bins has the same count. Zero-Padding off = the window itself.
    {
        Parameters::Values d;
        d.binsMode = Parameters::BinsMode::Auto;
        CHECK(outputBinCountFrom(d, 44100.0) == 16384 / 2 + 1);          // default pad 16384
        d.padSize = 65536; CHECK(outputBinCountFrom(d, 44100.0) == 32769);
        d.bins = 300;      CHECK(outputBinCountFrom(d, 44100.0) == 32769);   // Output Bins ignored
        d.padSize = 1024;  CHECK(outputBinCountFrom(d, 44100.0) == 4096 / 2 + 1);   // window 3175 > pad -> 4096
        d.zeroPad = false; CHECK(fftSizeFrom(d, 3175) == 3176);              // odd window: one zero sample
        CHECK(outputBinCountFrom(d, 44100.0) == 3176 / 2 + 1);
        d.winSamples = 2048; CHECK(outputBinCountFrom(d, 44100.0) == 1025);
        d.winMode = Parameters::WinMode::Milliseconds; d.winMs = 100.0;   // 4800 samples at 48 kHz
        CHECK(outputBinCountFrom(d, 48000.0) == 2401);
        Parameters::Values r; r.rawBins = true;                             // Raw overrides Fixed too
        CHECK(outputBinCountFrom(r, 44100.0) == 8193);
        r.zeroPad = false; CHECK(outputBinCountFrom(r, 44100.0) == 1589);
        CHECK(axisRate(r, 44100.0, 0.0) == 44100.0);                        // Raw spans DC..Nyquist whatever Display Max
        Parameters::Values f; f.zeroPad = false; f.binsMode = Parameters::BinsMode::Fixed;   // Fixed stays Output Bins
        CHECK(outputBinCountFrom(f, 44100.0) == f.bins);
    }

    // targetHzAt reproduces computeTargetHzGrid exactly, every scale.
    for (int scale = 0; scale < 7; ++scale) {
        std::vector<double> grid;
        PerceptualWarping::computeTargetHzGrid(scale, 20000.0, 513, 0.963, 20.0, grid);
        double worst = 0.0;
        for (size_t i = 0; i < grid.size(); ++i)
            worst = std::max(worst, std::abs(grid[i] - PerceptualWarping::targetHzAt(scale, 20000.0, i * (1.0 / 512.0), 0.963, 20.0)));
        CHECK(worst < 1e-9);
    }

    // Presets override exactly what they own.
    Parameters::Values v60; v60.preset = Parameters::Preset::Visual60; v60.warpInterp = Parameters::WarpInterp::Linear;
    v60.binsMode = Parameters::BinsMode::Fixed; applyPreset(v60);
    CHECK(v60.padSize == 8192 && v60.padIndex == 3);
    CHECK(v60.warpInterp == Parameters::WarpInterp::Cubic);
    CHECK(v60.binsMode == Parameters::BinsMode::Fixed && v60.betaMode == Parameters::BetaMode::Auto);
    Parameters::Values v120; v120.preset = Parameters::Preset::Visual120; applyPreset(v120);
    CHECK(v120.padSize == 4096 && v120.bins == 16384);
    Parameters::Values an; an.preset = Parameters::Preset::Analysis; applyPreset(an);
    CHECK(an.padSize == 32768 && an.warpAggregate == Parameters::WarpAggregate::Rms);
    Parameters::Values cu; cu.padSize = 1024; applyPreset(cu);
    CHECK(cu.padSize == 1024);                                 // Custom changes nothing
}

// Peak aggregation never drops a narrow peak on a coarse grid; interpolation does. RMS is the power mean.
static void test_warp_aggregation()
{
    section("v2.10 warp aggregation (peak / rms on the coarse part of the axis)");
    const size_t nlin = 8193;                        // a 16384-point transform
    const double nyq = 22050.0;
    PerceptualWarping off, peak, rms;
    off.buildWarpTables(0, nyq, 1024, nyq, 0.963, 20.0, nlin);
    peak.setAggregation(1);
    peak.buildWarpTables(0, nyq, 1024, nyq, 0.963, 20.0, nlin);
    rms.setAggregation(2);
    rms.buildWarpTables(0, nyq, 1024, nyq, 0.963, 20.0, nlin);
    CHECK(off.aggregatedBins() == 0);
    CHECK(peak.aggregatedBins() > 300);              // a 1024-bin log axis is coarse over most of its top half
    std::printf("  1024-bin log axis over 8193 FFT bins: %zu bins aggregate\n", peak.aggregatedBins());
    // A single-bin "partial" swept across the upper half of the band: the peak-aggregated output must
    // report it at full height at every position; the interpolated one loses it between taps.
    AlignedVector mag(nlin, 0.0f), o1, o2;
    double worstPeak = 1.0, worstOff = 1.0;
    for (size_t k = nlin / 2; k < nlin - 1; k += 7) {
        std::fill(mag.begin(), mag.end(), 0.0f);
        mag[k] = 1.0f;
        peak.applyWarp(mag, o1);
        off.applyWarp(mag, o2);
        worstPeak = std::min(worstPeak, static_cast<double>(*std::max_element(o1.begin(), o1.end())));
        worstOff = std::min(worstOff, static_cast<double>(*std::max_element(o2.begin(), o2.end())));
    }
    std::printf("  swept partial: smallest reported level peak=%.3f interpolate=%.3f\n", worstPeak, worstOff);
    CHECK(worstPeak == 1.0);
    CHECK(worstOff < 0.5);                           // documents the legacy loss
    // RMS of a constant range is the constant; the linear (fine) part keeps interpolated values.
    std::fill(mag.begin(), mag.end(), 2.0f);
    rms.applyWarp(mag, o1);
    double maxDev = 0.0;
    for (float v : o1) maxDev = std::max(maxDev, std::abs(v - 2.0));
    CHECK(maxDev < 1e-5);
    // the cubic kernel aggregates too
    PerceptualWarping cub;
    cub.setInterpolation(1); cub.setAggregation(1);
    cub.buildWarpTables(0, nyq, 1024, nyq, 0.963, 20.0, nlin);
    std::fill(mag.begin(), mag.end(), 0.0f); mag[nlin - 100] = 3.0f;
    cub.applyWarp(mag, o1);
    CHECK(*std::max_element(o1.begin(), o1.end()) == 3.0f);
    // the magnitude range the aggregated tables need is covered by maxLinearIndex
    CHECK(peak.maxLinearIndex() >= nlin - 2);
}

// IngestCursor: only new samples, nothing when the input did not cook, whole block on a discontinuity.
static void test_ingest_cursor()
{
    section("v2.10 ingest cursor (Input Ingest = Auto)");
    IngestCursor c;
    CHECK(c.fresh(0.0, 735, 1) == 735);               // first block: everything
    CHECK(c.fresh(735.0, 735, 2) == 735);             // timesliced: advanced by a whole block
    CHECK(c.fresh(735.0, 735, 2) == 0);               // input did not cook: nothing new
    CHECK(c.fresh(1000.0, 1024, 3) == 554);           // sliding window re-delivered with overlap: only the newest 554
    CHECK(c.fresh(1000.0, 1024, 4) == 1024);          // re-cooked without advancing (generator): whole block
    CHECK(c.fresh(0.0, 1024, 5) == 1024);             // jumped back (loop / reset): whole block
    CHECK(c.fresh(50000.0, 1024, 6) == 1024);         // gap larger than a block: whole block
    c.reset();
    CHECK(c.fresh(51024.0, 1024, 6) == 1024);         // after reset: whole block even with the same cook count
}

// Spectral features on known signals.
static void test_spectral_features()
{
    section("v2.10 spectral features");
    const double sr = 44100.0;
    const size_t N = 8192, win = 4096;
    PlanLog log;
    FFTWEngine e;
    e.prepare(N, PlannerPolicy::Fast, &log);
    AlignedVector window, frame(N, 0.0f), mag, sig(win), prev;
    AlignedComplexVector scratch;
    WindowGenerator::generateWindow(1, 0.0, win, window, WindowNorm::FullScale);
    auto analyse = [&](SpectralFeatures& f) {
        std::fill(frame.begin(), frame.end(), 0.0f);
        multiplyInto(sig.data(), window.data(), frame.data(), win);
        e.executeRFFT(frame, mag, scratch);
        computeSpectralFeatures(mag.data(), mag.size(), sr / N, sig.data(), win, prev, f);
    };
    SpectralFeatures f;
    for (size_t i = 0; i < win; ++i) sig[i] = static_cast<float>(0.5 * std::sin(2.0 * PI_D * 1000.0 * i / sr));
    analyse(f);
    std::printf("  1 kHz sine: centroid %.1f Hz, rolloff %.1f Hz, flatness %.4f, rms %.2f dBFS\n", f.centroidHz, f.rolloffHz, f.flatness, f.rmsDb);
    CHECK_NEAR(f.centroidHz, 1000.0, 20.0);
    CHECK_NEAR(f.rolloffHz, 1000.0, 20.0);
    CHECK(f.flatness < 0.01);                         // tonal
    CHECK_NEAR(f.rmsDb, 20.0 * std::log10(0.5 / std::sqrt(2.0)), 0.05);
    CHECK(f.midDb > f.bassDb + 30.0 && f.midDb > f.highDb + 30.0);   // 1 kHz sits in the mid band
    analyse(f);
    CHECK(f.flux < 1e-6);                             // an identical frame has no flux
    for (size_t i = 0; i < win; ++i) sig[i] *= 4.0f;  // an attack: level x4
    analyse(f);
    CHECK(f.flux > 0.5);
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.0f, 0.1f);
    for (size_t i = 0; i < win; ++i) sig[i] = nd(rng);
    analyse(f);
    std::printf("  white noise: centroid %.0f Hz, flatness %.3f\n", f.centroidHz, f.flatness);
    CHECK(f.flatness > 0.3);                          // noise-like
    CHECK_NEAR(f.centroidHz, sr / 4.0, 1500.0);       // flat spectrum -> centroid at Nyquist/2
}

// The pipeline's steady state allocates nothing - 1 channel and 4 channels (the parallel fan-out).
static void test_allocation_gate()
{
    section("v2.10 allocation gate (steady-state process() and AsyncAnalysis)");
    auto run = [&](int channels, bool features) {
        Parameters::Values p;                         // the plugin defaults: Auto (8193 bins), beta 15, Peak aggregation
        p.planner = Parameters::Planner::Fast;        // no background measurement thread in this test
        p.async = false;
        p.features = features;
        p.loudness = Parameters::Loudness::Db;        // the full chain: dB + ballistics + weighting
        p.ballEnable = true;
        p.weighting = Parameters::Weighting::AWeighting;
        PlanLog log;
        AnalysisPipeline pipe(&log);
        AnalysisJob job;
        job.numChannels = channels;
        job.sampleRate = 48000.0;
        job.winSamples = windowSamplesFrom(p, 48000.0);
        job.p = p;
        job.windows.assign(static_cast<size_t>(channels), AlignedVector(static_cast<size_t>(job.winSamples)));
        for (auto& w : job.windows) for (size_t i = 0; i < w.size(); ++i) w[i] = static_cast<float>(std::sin(i * 0.05));
        job.silent.assign(static_cast<size_t>(channels), 0);
        AnalysisResult res;
        for (int i = 0; i < 20; ++i) pipe.process(job, res);   // warm-up: plans, tables, buffers
        long long n;
        {
            AllocWindow w;
            for (int i = 0; i < 500; ++i) pipe.process(job, res);
            n = w.count();
        }
        std::printf("  %d channel(s)%s: %lld allocations over 500 steady-state process() calls\n",
                    channels, features ? " + features" : "", n);
        return n;
    };
    CHECK(run(1, false) == 0);
    CHECK(run(1, true) == 0);
    // std::execution::par (the Windows thread pool) may allocate per submission; report it, and hold
    // the pipeline's own code to zero by checking the 1-channel runs above.
    const long long par = run(4, false);
    (void)par;

    // AsyncAnalysis, sync and async, steady state (publish + acquire on the "cook" side).
    Parameters::Values p;
    p.planner = Parameters::Planner::Fast;
    PlanLog log;
    AnalysisPipeline pipe(&log);
    AsyncAnalysis a(pipe, log);
    auto fill = [&](AnalysisJob& j, uint64_t seq) {
        j.seq = seq; j.numChannels = 1; j.sampleRate = 44100.0; j.winSamples = 3175; j.p = p; j.dtMs = 16.7;
        if (j.windows.size() != 1) j.windows.assign(1, AlignedVector(3175, 0.25f));
        if (j.silent.size() != 1) j.silent.assign(1, 0);
    };
    a.configure(false, Parameters::WorkerWake::Poll, Parameters::WorkerPriority::Highest);
    for (uint64_t i = 1; i <= 20; ++i) { fill(a.jobSlot(), i); a.publish(); a.acquireResult(); }
    long long nSync;
    {
        AllocWindow w;
        for (uint64_t i = 21; i <= 520; ++i) { fill(a.jobSlot(), i); a.publish(); a.acquireResult(); }
        nSync = w.count();
    }
    std::printf("  AsyncAnalysis sync: %lld allocations over 500 cooks\n", nSync);
    CHECK(nSync == 0);
}

// AsyncAnalysis: sync runs inline, async hands over, dormancy never loses a job, Signal wakes quickly.
static void test_async_analysis()
{
    section("v2.10 AsyncAnalysis (handoff, dormancy, wake policies)");
    Parameters::Values p;
    p.planner = Parameters::Planner::Fast;
    PlanLog log;
    AnalysisPipeline pipe(&log);
    AsyncAnalysis a(pipe, log);
    uint64_t seq = 0;
    auto publish = [&]() {
        AnalysisJob& j = a.jobSlot();
        j.seq = ++seq; j.numChannels = 1; j.sampleRate = 44100.0; j.winSamples = 3175; j.p = p; j.dtMs = 16.7;
        if (j.windows.size() != 1) j.windows.assign(1, AlignedVector(3175));
        for (size_t i = 0; i < 3175; ++i) j.windows[0][i] = static_cast<float>(std::sin(i * 0.1));
        j.silent.assign(1, 0);
        a.publish();
    };
    auto waitResult = [&](uint64_t want, double timeoutMs) {
        const auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            a.acquireResult();
            if (a.result().seq == want) return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > timeoutMs) return -1.0;
            std::this_thread::yield();   // not sleep_for: on Windows any sleep rounds up to the ~15.6 ms tick
        }
    };
    // sync: the result is there when publish() returns
    a.configure(false, Parameters::WorkerWake::Poll, Parameters::WorkerPriority::Highest);
    CHECK(!a.async());
    publish();
    CHECK(a.acquireResult() && a.result().seq == seq);
    // async / poll
    a.configure(true, Parameters::WorkerWake::Poll, Parameters::WorkerPriority::Highest);
    CHECK(a.async());
    publish();
    const double tPoll = waitResult(seq, 2000.0);
    CHECK(tPoll >= 0.0);
    // dormancy: after > 500 ms idle the worker sleeps; the next job must still be picked up promptly
    int lost = 0;
    double worstWake = 0.0;
    for (int cycle = 0; cycle < 3; ++cycle) {
        std::this_thread::sleep_for(std::chrono::milliseconds(650));
        publish();
        const double t = waitResult(seq, 1000.0);
        if (t < 0.0) ++lost;
        worstWake = std::max(worstWake, t);
    }
    std::printf("  dormant -> woken pickup: worst %.2f ms over 3 cycles, lost %d\n", worstWake, lost);
    CHECK(lost == 0);
    CHECK(worstWake < 10.0);                          // one kernel wake-up + one analysis, not a poll period
    // many jobs at random gaps across the poll interval: latest-wins, never a lost final job
    std::mt19937 rng(11);
    std::uniform_int_distribution<int> gap(0, 3000);
    for (int i = 0; i < 300; ++i) { publish(); std::this_thread::sleep_for(std::chrono::microseconds(gap(rng))); }
    CHECK(waitResult(seq, 1000.0) >= 0.0);
    // Signal policy: pickup well under the 2 ms poll
    a.configure(true, Parameters::WorkerWake::Signal, Parameters::WorkerPriority::Highest);
    std::vector<double> sig;
    for (int i = 0; i < 50; ++i) {
        publish();
        const double t = waitResult(seq, 1000.0);
        sig.push_back(t);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    std::sort(sig.begin(), sig.end());
    std::printf("  Signal wake: result latency median %.3f ms (includes the analysis)\n", sig[sig.size() / 2]);
    CHECK(sig.front() >= 0.0);
    CHECK(sig[sig.size() / 2] < 1.5);                 // woken per job: well under the 2 ms poll period
    // MMCSS priority restarts the worker and keeps working
    a.configure(true, Parameters::WorkerWake::Poll, Parameters::WorkerPriority::Mmcss);
    publish();
    CHECK(waitResult(seq, 2000.0) >= 0.0);
    const AsyncAnalysis::Pickup pk = a.pickup();
    std::printf("  pickup telemetry: p50 %.0f us, p99 %.0f us, max %.0f us, late %llu\n", pk.p50Us, pk.p99Us, pk.maxUs,
                static_cast<unsigned long long>(pk.late));
    CHECK(pk.p50Us > 0.0);
    a.configure(false, Parameters::WorkerWake::Poll, Parameters::WorkerPriority::Highest);
    CHECK(!a.async());
}

// FFTWEngine never blocks the owner on an in-flight measurement: a size change abandons it.
static void test_planner_graveyard()
{
    section("v2.10 planner graveyard (no blocking join on a size change)");
    std::remove("fft_tests_wisdom_graveyard.txt");
    const std::string saved = FFTWEngine::wisdomPathOverride();
    PlanLog log;
    FFTWEngine e;
    e.prepare(65536 * 2, PlannerPolicy::Auto, &log);   // 131072 is not in this run's wisdom: measures in background
    const bool measuring = e.upgradeInProgress();
    const auto t0 = std::chrono::steady_clock::now();
    e.prepare(1024, PlannerPolicy::Fast, &log);         // must not wait for the measurement
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  size change during a measurement: prepare() returned in %.2f ms (measuring: %s, abandoned: %zu)\n",
                ms, measuring ? "yes" : "no", e.abandonedMeasurements());
    CHECK(ms < 250.0);
    CHECK(e.hasPlan() && e.fftSize() == 1024);
    // the abandoned measurement is reaped once it finishes (poll drives the reaping)
    const auto t1 = std::chrono::steady_clock::now();
    while (e.abandonedMeasurements() > 0 &&
           std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count() < 20.0) {
        e.pollBackgroundPlan();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(e.abandonedMeasurements() == 0);
    FFTWEngine::wisdomPathOverride() = saved;
}

// ------------------------------------------------------------------------------------------
// ===================== main =====================
// The runner. There is no discovery mechanism: a test runs if and only if it is called here, in the
// order written, which is the order the header's table of contents lists.
//
// HOW TO CHANGE: adding a test means writing the function, adding the call below, and updating the
// table of contents and the check count in the header. Removing a call silently removes its coverage
// while the suite still reports success, so the header list and this block have to be kept in step.
// v2.12 SIMD kernels against scalar references: the load+permute warp paths (linear and cubic, on
// grids that switch between the permute and the gather path), the branch-free peak index (first
// maximum wins, every length and tail), the silence test, the FMA dB normalisation, and the
// vectorized spectral features against the pre-2.12 scalar loop.
static void test_v212_simd_kernels()
{
    section("v2.12 SIMD kernels vs scalar references (warp permute, peak index, dB, features)");
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);

    // --- warp: Log (mixed permute/gather), Linear upsampled (all permute), Linear downsampled (all gather)
    struct G { int scale; size_t nout; size_t nlin; double blend; };
    const G grids[] = { { 0, 16384, 8193, 0.963 }, { 5, 20000, 4097, 0.0 }, { 5, 1000, 8193, 0.0 }, { 1, 777, 2049, 1.0 }, { 0, 37, 513, 0.963 } };
    for (const G& g : grids) {
        AlignedVector src(g.nlin);
        for (auto& v : src) v = U(rng);
        for (int interp = 0; interp <= 1; ++interp) {
            PerceptualWarping w;
            w.setInterpolation(interp);
            w.setAggregation(0);
            w.buildWarpTables(g.scale, 22050.0, g.nout, 22050.0, g.blend, 20.0, g.nlin);
            AlignedVector out;
            w.applyWarp(src, out);
            CHECK(out.size() == g.nout);
            // scalar reference from the same grid definition
            const auto& hz = w.targetHz();
            double worst = 0.0;
            const int last = static_cast<int>(g.nlin) - 1;
            for (size_t i = 0; i < g.nout; ++i) {
                double frac = hz[i] / 22050.0 * static_cast<double>(g.nlin - 1);
                const double r = std::round(frac);
                if (std::abs(frac - r) < 1e-6) frac = r;
                const int i0 = static_cast<int>(std::max(0.0, std::min(static_cast<double>(g.nlin - 2), std::floor(frac))));
                const double t = std::clamp(frac - i0, 0.0, 1.0);
                double ref;
                if (interp == 0) {
                    ref = src[i0] + t * (static_cast<double>(src[i0 + 1]) - src[i0]);
                } else {
                    const double p0 = src[std::max(i0 - 1, 0)], p1 = src[i0], p2 = src[std::min(i0 + 1, last)], p3 = src[std::min(i0 + 2, last)];
                    ref = std::max(0.0, 0.5 * (2.0 * p1 + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t * t + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t * t * t));
                }
                worst = std::max(worst, std::abs(ref - out[i]));
            }
            CHECK(worst < 1e-5);
            if (worst >= 1e-5) std::printf("  warp scale %d nout %zu nlin %zu interp %d: worst %.3g\n", g.scale, g.nout, g.nlin, interp, worst);
        }
    }

    // --- peak index: first maximum, every length 0..100 plus large, random / rising / ties / constant
    auto refPeak = [](const float* d, size_t n, size_t& idx) { idx = 0; if (!n) return 0.0f; float m = d[0]; for (size_t i = 1; i < n; ++i) if (d[i] > m) { m = d[i]; idx = i; } return m; };
    bool peakOk = true;
    for (size_t n : { size_t(0), size_t(1), size_t(7), size_t(8), size_t(31), size_t(32), size_t(33), size_t(63), size_t(64), size_t(100), size_t(1000), size_t(16384), size_t(16385) }) {
        AlignedVector d(n + 1);
        for (int pattern = 0; pattern < 4; ++pattern) {
            for (size_t i = 0; i < n; ++i) d[i] = pattern == 0 ? U(rng) : pattern == 1 ? static_cast<float>(i) : pattern == 2 ? static_cast<float>((i * 7) % 5) : 1.0f;
            size_t a1 = 99, a2 = 99;
            const float v1 = findPeakWithIndex(d.data(), n, a1), v2 = refPeak(d.data(), n, a2);
            if (v1 != v2 || a1 != a2) { peakOk = false; std::printf("  peak n %zu pattern %d: %zu vs %zu\n", n, pattern, a1, a2); }
        }
    }
    CHECK(peakOk);

    // --- silence: -0.0 is silent, one denormal / one sample in the tail is not
    {
        AlignedVector z(3175, 0.0f);
        CHECK(blockIsSilent(z.data(), z.size()));
        z[100] = -0.0f; CHECK(blockIsSilent(z.data(), z.size()));
        z[3174] = 1e-40f; CHECK(!blockIsSilent(z.data(), z.size()));
        z[3174] = 0.0f; z[5] = -1e-30f; CHECK(!blockIsSilent(z.data(), z.size()));
    }

    // --- dB normalised: FMA form vs the exact formula (the table's own error is ~0.003 dB)
    {
        AlignedVector m(4099);
        for (auto& v : m) v = std::exp((U(rng) - 0.7f) * 20.0f);
        AlignedVector d = m;
        DecibelConverter::convertToDB(2, 80.0, 1.0f / 3.0f, d);
        double worst = 0.0;
        for (size_t i = 0; i < m.size(); ++i) {
            const double ex = std::clamp((20.0 * std::log10(std::max(1e-12, static_cast<double>(m[i]) / 3.0)) + 80.0) / 80.0, 0.0, 1.0);
            worst = std::max(worst, std::abs(ex - d[i]) * 80.0);
        }
        CHECK(worst < 0.005);   // dB
    }

    // --- FastLog2Seg: scalar and vector agree, max error 0.0002 dB over 1e-12 .. 1e6
    {
        double worstS = 0.0, worstV = 0.0;
        const FastLog2Seg::Regs lr = FastLog2Seg::regs();
        alignas(32) float in[8], outv[8];
        for (int i = 0; i < 400000; ++i) {
            const float x = static_cast<float>(std::exp(-27.6 + 41.4 * (i / 400000.0)));
            const double ex = std::log2(static_cast<double>(x));
            worstS = std::max(worstS, std::abs(FastLog2Seg::log2(x) - ex));
            in[i & 7] = x;
            if ((i & 7) == 7) {
                _mm256_store_ps(outv, FastLog2Seg::log2(_mm256_load_ps(in), lr));
                for (int l = 0; l < 8; ++l) worstV = std::max(worstV, std::abs(outv[l] - std::log2(static_cast<double>(in[l]))));
            }
        }
        CHECK(worstS * FastLog2Seg::kDbPerOctave < 2e-4);
        CHECK(worstV * FastLog2Seg::kDbPerOctave < 2e-4);
    }

    // --- aggregation skip: with Peak/RMS on, every output bin is written (NaN sentinel), the fine part
    //     equals the plain interpolation, and a Peak bin holds an actual FFT-bin value
    for (int interp = 0; interp <= 1; ++interp) {
        for (int agg = 1; agg <= 2; ++agg) {
            AlignedVector src(8193);
            for (auto& v : src) v = U(rng);
            PerceptualWarping plain, aggd;
            plain.setInterpolation(interp); plain.setAggregation(0);
            aggd.setInterpolation(interp);  aggd.setAggregation(agg);
            plain.buildWarpTables(0, 22050.0, 8193, 22050.0, 0.963, 20.0, 8193);
            aggd.buildWarpTables(0, 22050.0, 8193, 22050.0, 0.963, 20.0, 8193);
            AlignedVector o1, o2(8193, std::numeric_limits<float>::quiet_NaN());
            plain.applyWarp(src, o1);
            aggd.applyWarp(src, o2);
            size_t nan = 0, same = 0, fromSrc = 0;
            for (size_t i = 0; i < o2.size(); ++i) {
                if (std::isnan(o2[i])) { ++nan; continue; }
                if (o2[i] == o1[i]) ++same;
                else if (agg == 1 && std::find(src.begin(), src.end(), o2[i]) != src.end()) ++fromSrc;
            }
            CHECK(nan == 0);
            CHECK(aggd.aggregatedBins() > 100);
            CHECK(same + aggd.aggregatedBins() >= o2.size());           // every non-aggregated bin is the interpolation
            if (agg == 1) CHECK(same + fromSrc == o2.size());            // every Peak bin is a real FFT-bin value
        }
    }

    // --- in-place ballistics == apply() + copy, bit for bit (and the reset / off paths)
    {
        AlignedVector cur(16389), prevA(16389), prevB;
        for (size_t i = 0; i < cur.size(); ++i) { cur[i] = U(rng); prevA[i] = U(rng); }
        prevB = prevA;
        AlignedVector io = cur;
        BallisticsFilter bf;
        bf.apply(0.3f, 0.8f, cur, prevA);
        bf.applyInPlace(0.3f, 0.8f, io, prevB);
        CHECK(std::memcmp(prevA.data(), prevB.data(), prevA.size() * 4) == 0);
        CHECK(std::memcmp(io.data(), prevA.data(), io.size() * 4) == 0);
        AlignedVector empty, io2 = cur;
        bf.applyInPlace(0.3f, 0.8f, io2, empty);                          // no history yet: restart at the frame
        CHECK(empty.size() == cur.size() && std::memcmp(io2.data(), cur.data(), cur.size() * 4) == 0);
    }

    // --- weighting + reference peak in one pass
    {
        AlignedVector a1(1003), a2, curve(1003);
        for (size_t i = 0; i < a1.size(); ++i) { a1[i] = U(rng); curve[i] = 0.5f + U(rng); }
        a2 = a1;
        const float m1 = multiplyInPlaceMax(a1.data(), curve.data(), a1.size());
        multiplyInPlace(a2.data(), curve.data(), a2.size());
        CHECK(std::memcmp(a1.data(), a2.data(), a1.size() * 4) == 0);
        CHECK(m1 == peakMagnitude(a2.data(), a2.size()));
    }

    // --- spectral features vs the pre-2.12 scalar loop
    {
        const size_t n = 8193;
        const double binHz = 44100.0 / 16384.0;
        AlignedVector mag(n), tim(3175), prevA, prevB;
        for (size_t i = 0; i < n; ++i) mag[i] = U(rng) * static_cast<float>(std::exp(-static_cast<double>(i) / 1500.0));
        for (auto& x : tim) x = U(rng) - 0.5f;
        SpectralFeatures f;
        computeSpectralFeatures(mag.data(), n, binHz, tim.data(), tim.size(), prevA, f);   // prime prev
        for (auto& v : prevA) v *= 0.7f;
        prevB = prevA;
        computeSpectralFeatures(mag.data(), n, binHz, tim.data(), tim.size(), prevA, f);
        // reference
        const float* lut = FastLog10::dbTable();
        const size_t b250 = std::min(n, static_cast<size_t>(250.0 / binHz) + 1), b4k = std::min(n, static_cast<size_t>(4000.0 / binHz) + 1);
        double total = 0, weighted = 0, sumMag = 0, logSum = 0, bass = 0, mid = 0, high = 0, rise = 0;
        for (size_t k = 0; k < n; ++k) {
            const float m = mag[k];
            const double pw = static_cast<double>(m) * m;
            total += pw; weighted += pw * static_cast<double>(k); sumMag += m;
            logSum += FastLog10::scaled(std::max(m, 1e-12f), lut);
            if (k < b250) bass += pw; else if (k < b4k) mid += pw; else high += pw;
            const float dd = m - prevB[k]; if (dd > 0.0f) rise += dd;
        }
        double acc = 0; size_t kr = 0;
        for (; kr < n; ++kr) { acc += static_cast<double>(mag[kr]) * mag[kr]; if (acc >= 0.85 * total) break; }
        CHECK_NEAR(f.centroidHz, weighted / total * binHz, 1e-3 * weighted / total * binHz);
        CHECK_NEAR(f.rolloffHz, std::min(kr, n - 1) * binHz, binHz * 1.01);
        const double geo = std::pow(10.0, (logSum / n) / 10.0);
        CHECK_NEAR(f.flatness, std::clamp(geo / (total / n), 0.0, 1.0), 1e-4);
        CHECK_NEAR(f.flux, rise / sumMag, 1e-5);
        CHECK_NEAR(f.bassDb, 10.0 * std::log10(bass), 1e-3);
        CHECK_NEAR(f.midDb, 10.0 * std::log10(mid), 1e-3);
        CHECK_NEAR(f.highDb, 10.0 * std::log10(high), 1e-3);
        double s2 = 0; for (float x : tim) s2 += static_cast<double>(x) * x;
        CHECK_NEAR(f.rmsDb, 20.0 * std::log10(std::sqrt(s2 / tim.size())), 1e-3);
    }
}

// v2.11: the output sample count through the real pipeline, for every bins mode. Fixed must give exactly
// Output Bins - also far above the rfft's N/2+1 (the zero-pad + interpolation use case); Auto and Raw
// give N/2+1 of the transform actually run; Raw is bit-identical to the rfft magnitude; Zero-Padding off
// runs the transform on the window itself.
static void test_output_bin_modes()
{
    section("v2.11 output sample count: Fixed (upsampled), Auto = N/2+1, Raw rfft, Zero-Padding off");
    const double sr = 48000.0;
    const int win = 3000;
    AlignedVector sig(win);
    for (int i = 0; i < win; ++i) sig[i] = static_cast<float>(0.5 * std::sin(2.0 * PI_D * 1000.0 * i / sr));

    auto run = [&](const Parameters::Values& p, AnalysisPipeline::Status* st = nullptr) {
        PlanLog log;
        AnalysisPipeline pipe(&log);
        AnalysisJob job;
        job.numChannels = 1; job.sampleRate = sr; job.winSamples = win; job.dtMs = 1000.0 / 60.0;
        job.reset = true; job.p = p; job.windows.push_back(sig); job.silent.push_back(0);
        AnalysisResult res;
        pipe.process(job, res);
        if (st) *st = pipe.status();
        return res;
    };
    Parameters::Values base;
    base.winMode = Parameters::WinMode::Samples; base.winSamples = win;
    base.loudness = Parameters::Loudness::Off; base.ballEnable = false;

    // Fixed, 32768 output bins from a 16384-point transform (8193 rfft bins): 4x upsampled.
    Parameters::Values fx = base; fx.binsMode = Parameters::BinsMode::Fixed; fx.bins = 32768; fx.padSize = 16384;
    AnalysisResult r = run(fx);
    CHECK(r.spectra[0].size() == 32768);
    CHECK(outputBinCountFrom(fx, sr) == 32768);
    CHECK_NEAR(r.peakHz, 1000.0, 60.0);

    // Auto: N/2+1 of the padded transform, whatever Output Bins says.
    Parameters::Values au = base; au.binsMode = Parameters::BinsMode::Auto; au.bins = 1000; au.padSize = 65536;
    AnalysisPipeline::Status st;
    r = run(au, &st);
    CHECK(st.fftSize == 65536);
    CHECK(r.spectra[0].size() == 32769);
    CHECK(outputBinCountFrom(au, sr) == 32769);

    // Raw: identity (memcpy) of the rfft magnitude, DC..Nyquist, Scale / Display Max ignored.
    Parameters::Values rw = base; rw.rawBins = true; rw.padSize = 8192; rw.scale = Parameters::Scale::Mel; rw.displayMax = 5000.0;
    r = run(rw, &st);
    CHECK(r.spectra[0].size() == 4097);
    CHECK(st.linearGrid);
    CHECK_NEAR(st.axisRate, sr, 1e-6);
    CHECK_NEAR(r.peakHz, 1000.0, sr / 8192.0 + 1e-3);
    {
        // bit-identical to an independent FFT of the same windowed, centred frame
        AlignedVector wb;
        FFTDSP::WindowGenerator::generateWindow(static_cast<int>(rw.window), rw.kaiserBeta, win, wb, FFTDSP::WindowNorm::CoherentGain);
        const size_t start = ((8192 - win) / 2) & ~static_cast<size_t>(7);
        std::vector<double> re(4097, 0.0), im(4097, 0.0);
        size_t k_peak = static_cast<size_t>(std::lround(1000.0 / sr * 8192.0));
        for (size_t k = k_peak - 2; k <= k_peak + 2; ++k) {
            for (int n = 0; n < win; ++n) {
                const double x = static_cast<double>(sig[n]) * wb[n];
                const double ph = -2.0 * PI_D * static_cast<double>(k) * static_cast<double>(start + n) / 8192.0;
                re[k] += x * std::cos(ph); im[k] += x * std::sin(ph);
            }
            const double mag = std::hypot(re[k], im[k]);
            CHECK_NEAR(r.spectra[0][k], mag, 1e-4 * mag + 1e-4);
        }
    }

    // Zero-Padding off: N = window (3000, even) -> 1501 rfft bins, for Raw and Auto alike.
    Parameters::Values np = rw; np.zeroPad = false;
    r = run(np, &st);
    CHECK(st.fftSize == 3000);
    CHECK(r.spectra[0].size() == 1501);
    CHECK(st.linearGrid);
    Parameters::Values npa = au; npa.zeroPad = false;
    r = run(npa, &st);
    CHECK(r.spectra[0].size() == 1501);
    // ... and Fixed still interpolates the unpadded rfft onto exactly Output Bins.
    Parameters::Values npf = fx; npf.zeroPad = false;
    r = run(npf, &st);
    CHECK(st.fftSize == 3000);
    CHECK(r.spectra[0].size() == 32768);
}

int main()
{
    // Unbuffered stdout, for the same reason as the bench: piped output is block-buffered by the MSVC
    // CRT and a crash does not flush, so a crash here would swallow the name of the test that was
    // running. With this, the last line printed is the crash site - and that line is the whole
    // diagnosis, because the alternative is bisecting by re-running with tests commented out.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // Hermetic wisdom: every FFTWEngine in this process reads and writes a private file next to the
    // executable, set BEFORE the first prepare() - importWisdomOnce() latches once per process, and the
    // first prepare() (test_v23_helpers) used to import the user's real %LOCALAPPDATA% cache.
    FFTWEngine::wisdomPathOverride() = "fft_tests_wisdom.txt";
    std::remove("fft_tests_wisdom.txt");
    // The build answer (was this compiled with AVX2?) and the runtime one (can this CPU run it?) are
    // independent, and the interesting rows in a failure report are the ones where they disagree.
    std::printf("FFT plugin DSP tests (AVX2 %s, CPU AVX2 %s)\n",
#if defined(__AVX2__)
                "build",
#else
                "off",
#endif
                cpuSupportsAVX2() ? "yes" : "no");
    test_fifo();
    test_fastlog10();
    test_magnitude_and_peak();
    test_window_normalization();
    test_warp();
    test_decibel();
    test_ballistics();
    test_eq_streaming();
    test_v23_helpers();
    test_plan_log_tail();
    test_clip_line();
    test_triple_buffer_and_signal();
    test_background_plan();
    test_backend_selection();
    test_async_single_thread();
    test_pipeline_sine();
    test_pipeline_process();
    test_identity_grid_and_rate();
    test_rate_model();
    test_equal_loudness();
    test_v210_rate_helpers();
    test_output_bin_modes();
    test_v212_simd_kernels();
    test_warp_aggregation();
    test_ingest_cursor();
    test_spectral_features();
    test_allocation_gate();
    test_async_analysis();
    test_planner_graveyard();
    // The line the header's "the check count is a signal" section is about. Exit code is 0 only when
    // every check passed, which is what ctest --output-on-failure keys off.
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
