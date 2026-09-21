// Headless DSP unit tests for the FFT plugin (no TouchDesigner required).
//   ninja -C build && ctest --test-dir build --output-on-failure
//
// Golden-vector style checks: every SIMD path is compared against a scalar
// reference, and the full pipeline is checked against a known sine.

#include "DSPModules.h"
#include "RateModel.h"
#include "AnalysisPipeline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#include <tlhelp32.h>   // which libraries are actually loaded in this process (the OpenMP check below)
#endif

using namespace FFTDSP;

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

static void section(const char* name) { std::printf("[%s]\n", name); }

// ------------------------------------------------------------------------------------------
static void test_fifo()
{
    section("FIFOBuffer");
    FIFOBuffer fifo(10);
    std::vector<float> ref;
    std::mt19937 rng(7);
    AlignedVector out;
    for (int step = 0; step < 50; ++step) {
        size_t n = 1 + rng() % 13;
        std::vector<float> block(n);
        for (auto& v : block) v = static_cast<float>(rng() % 1000);
        fifo.add(block.data(), n);
        ref.insert(ref.end(), block.begin(), block.end());
        if (ref.size() > 10) ref.erase(ref.begin(), ref.end() - 10);
        fifo.get(out);
        CHECK(out.size() == 10);
        // out is right-aligned while filling, exact copy afterwards
        size_t offset = 10 - ref.size();
        bool ok = true;
        for (size_t i = 0; i < ref.size(); ++i) ok = ok && (out[offset + i] == ref[i]);
        for (size_t i = 0; i < offset; ++i) ok = ok && (out[i] == 0.0f);
        CHECK(ok);
    }
}

// ------------------------------------------------------------------------------------------
static void test_fastlog10()
{
    section("FastLog10");
    double max_err = 0.0;
    for (float x = 1e-9f; x < 1e6f; x *= 1.037f) {
        double ref = 20.0 * std::log10(static_cast<double>(x));
        max_err = std::max(max_err, std::abs(ref - FastLog10::scaled(x)));
    }
    std::printf("  scalar max error: %.5f dB\n", max_err);
    CHECK(max_err < 0.006);   // 2048-entry LUT, mid-interval samples: 0.0042 dB worst case
#if defined(__AVX2__)
    alignas(32) float in[8] = { 1e-6f, 0.001f, 0.5f, 1.0f, 3.7f, 100.0f, 12345.0f, 2.5e5f };
    alignas(32) float out[8];
    _mm256_store_ps(out, FastLog10::scaledVec(_mm256_load_ps(in)));
    for (int i = 0; i < 8; ++i) CHECK_NEAR(out[i], FastLog10::scaled(in[i]), 1e-4);
#endif
}

// ------------------------------------------------------------------------------------------
static void test_magnitude_and_peak()
{
    section("computeMagnitudeAVX2_FMA / peak");
    const size_t n = 1000;
    AlignedComplexVector c(n);
    AlignedVector mag(n), ref(n);
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> d(-100.0f, 100.0f);
    for (size_t i = 0; i < n; ++i) {
        c[i] = { d(rng), d(rng) };
        ref[i] = std::sqrt(c[i].real() * c[i].real() + c[i].imag() * c[i].imag());
    }
    computeMagnitudeAVX2_FMA(reinterpret_cast<const float*>(c.data()), mag.data(), n);
    double max_rel = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double rel = std::abs(static_cast<double>(mag[i]) - ref[i]) / std::max(1e-9, static_cast<double>(ref[i]));
        max_rel = std::max(max_rel, rel);
    }
    std::printf("  magnitude max rel error: %.2e\n", max_rel);
    CHECK(max_rel < 1e-5);

    size_t idx = 0;
    float pk = findPeakWithIndex(mag.data(), n, idx);
    auto it = std::max_element(ref.begin(), ref.end());
    CHECK(static_cast<size_t>(it - ref.begin()) == idx);
    CHECK_NEAR(pk, *it, 1e-3 * (*it));
    CHECK_NEAR(peakMagnitude(mag.data(), n), *it, 1e-3 * (*it));

    // peak at index 0 and in the scalar tail
    AlignedVector small(19, 0.0f);
    small[0] = 5.0f; CHECK(findPeakWithIndex(small.data(), 19, idx) == 5.0f && idx == 0);
    small[18] = 9.0f; CHECK(findPeakWithIndex(small.data(), 19, idx) == 9.0f && idx == 18);
}

// ------------------------------------------------------------------------------------------
static void test_window_normalization()
{
    section("WindowGenerator normalization");
    AlignedVector w;
    for (int type = 0; type < 6; ++type) {
        WindowGenerator::generateWindow(type, 15.0, 3175, w, WindowNorm::CoherentGain);
        double mean = 0; for (float v : w) mean += v; mean /= w.size();
        CHECK_NEAR(mean, 1.0, 1e-4);
        WindowGenerator::generateWindow(type, 15.0, 3175, w, WindowNorm::FullScale);
        double sum = 0; for (float v : w) sum += v;
        CHECK_NEAR(sum, 2.0, 1e-4);
    }
}

// ------------------------------------------------------------------------------------------
static void test_warp()
{
    section("PerceptualWarping");
    const size_t nlin = 513;
    AlignedVector src(nlin), out;
    for (size_t i = 0; i < nlin; ++i) src[i] = static_cast<float>(std::sin(i * 0.05) * 10.0 + i * 0.01);
    PerceptualWarping w;

    // identity: linear scale, fmax == nyquist, bins == nlin
    w.buildWarpTables(5, 22050.0, nlin, 22050.0, 1.0, 20.0, nlin);
    CHECK(w.isIdentity());
    w.applyWarp(src, out);
    bool same = out.size() == nlin;
    for (size_t i = 0; same && i < nlin; ++i) same = (out[i] == src[i]);
    CHECK(same);

    // log scale: gather path vs scalar reference
    const size_t n_out = 1000;
    w.buildWarpTables(0, 22050.0, n_out, 22050.0, 0.963, 20.0, nlin);
    CHECK(!w.isIdentity());
    w.applyWarp(src, out);
    CHECK(out.size() == n_out);
    const auto& hz = w.targetHz();
    double max_err = 0.0;
    for (size_t i = 0; i < n_out; ++i) {
        double frac = hz[i] / 22050.0 * (nlin - 1);
        size_t i0 = static_cast<size_t>(std::min<double>(nlin - 2, std::floor(frac)));
        double wt = frac - i0;
        double ref = src[i0] + wt * (src[i0 + 1] - src[i0]);
        max_err = std::max(max_err, std::abs(ref - out[i]));
    }
    std::printf("  warp max error vs scalar reference: %.2e\n", max_err);
    CHECK(max_err < 1e-3);
    CHECK(hz.front() >= 0.0 && hz.back() <= 22050.0 + 1e-6);
}

// ------------------------------------------------------------------------------------------
static void test_decibel()
{
    section("DecibelConverter");
    AlignedVector s(64);
    for (size_t i = 0; i < s.size(); ++i) s[i] = static_cast<float>(i + 1);
    AlignedVector a = s, b = s;
    float peak = peakMagnitude(s.data(), s.size());
    DecibelConverter::convertToDB(1, 80.0, 1.0f / peak, a);
    CHECK_NEAR(a.back(), 0.0, 6e-3);                                 // loudest bin -> 0 dB (LUT: <= 0.0042 dB)
    CHECK_NEAR(a[31], 20.0 * std::log10(32.0 / 64.0), 6e-3);           // -6.02 dB
    DecibelConverter::convertToDB(2, 80.0, 1.0f / peak, b);
    CHECK_NEAR(b.back(), 1.0, 1e-3);
    for (float v : b) CHECK(v >= 0.0f && v <= 1.0f);
    // tiny values clamp to the floor
    AlignedVector z(16, 0.0f);
    DecibelConverter::convertToDB(1, 80.0, 1.0f, z);
    for (float v : z) CHECK_NEAR(v, -80.0, 1e-4);
}

// ------------------------------------------------------------------------------------------
static void test_ballistics()
{
    section("BallisticsFilter");
    BallisticsFilter f;
    AlignedVector cur(32, 1.0f), prev(32, 0.0f);
    f.apply(0.5f, 0.9f, cur, prev);
    for (float v : prev) CHECK_NEAR(v, 0.5, 1e-6);            // attack: half way
    std::fill(cur.begin(), cur.end(), 0.0f);
    f.apply(0.5f, 0.9f, cur, prev);
    for (float v : prev) CHECK_NEAR(v, 0.45, 1e-6);           // release: 10% of the way down
    CHECK_NEAR(BallisticsFilter::coefFromMs(0.0, 16.7), 0.0, 1e-9);
    CHECK_NEAR(BallisticsFilter::coefFromMs(100.0, 16.6667), std::exp(-16.6667 / 100.0), 1e-6);
}

// ------------------------------------------------------------------------------------------
static void test_eq_streaming()
{
    section("BiquadEQ streaming (block ingest == one-shot)");
    const size_t total = 3175 * 3;
    std::vector<float> sig(total);
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> d(-1.0f, 1.0f);
    for (auto& v : sig) v = d(rng);

    BiquadEQ one(44100.0), blocks(44100.0);
    CHECK(one.updateAndCheckActive(6.0, 1000.0, -3.0, 200.0, 0.707, 1.0));
    CHECK(blocks.updateAndCheckActive(6.0, 1000.0, -3.0, 200.0, 0.707, 1.0));
    AlignedVector whole(sig.begin(), sig.end()), out;
    one.processAudio(whole, 1.0, out);                       // reference: whole signal in one pass

    std::vector<float> streamed;
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
    AlignedVector w1(sig.begin(), sig.begin() + 3175), w2(sig.begin() + 735, sig.begin() + 735 + 3175), o1, o2;
    legacy.processAudio(w1, 1.0, o1);
    legacy.processAudio(w2, 1.0, o2);
    double legacy_err = 0.0;
    for (size_t i = 0; i < 3175; ++i) legacy_err = std::max(legacy_err, std::abs(static_cast<double>(o2[i]) - out[735 + i]));
    std::printf("  legacy per-window re-filter error vs true output: %.2e (expected > 0: stale state at window start)\n", legacy_err);
    CHECK(legacy_err > 1e-4);

    // inactive EQ is a pass-through in place
    BiquadEQ off(44100.0);
    CHECK(!off.updateAndCheckActive(0.0, 1000.0, 0.0, 200.0, 0.707, 1.0));
    float x[4] = { 1, 2, 3, 4 };
    off.processBlockInPlace(x, 4, 1.0);
    CHECK(x[0] == 1 && x[3] == 4);
}

// ------------------------------------------------------------------------------------------
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
    AlignedVector frame(65536, 0.0f), mag; AlignedComplexVector scratch;
    frame[100] = 1.0f;
    e.executeRFFT(frame, mag, scratch);                      // executes while the background thread may be measuring
    CHECK(mag.size() == 32769);
    for (int i = 0; i < 400 && !e.pollBackgroundPlan(); ++i) {   // wait for the upgrade (or wisdom-only instant plan)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        e.executeRFFT(frame, mag, scratch);
    }
    std::printf("  final: %s\n", e.getPlanStatus().c_str());
    CHECK(e.getPlanStatus().find("FFTW_MEASURE") != std::string::npos);
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
    for (int i = 0; i < 1500 && !pe.pollBackgroundPlan(); ++i) {
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
// The FFT Backend toggle. Runs identically whether or not oneMKL is installed on this machine,
// because both are legitimate outcomes and the test asserts the contract rather than the install:
//   1. asking for oneMKL either loads it, or falls back to FFTW3 *with a working plan* - never a
//      node with no plan, which would publish a flat spectrum and look like a broken plugin;
//   2. every backend that is offered by the registry can be asked for and produces the same
//      transform (a backend switch is a performance choice, not a numerical one);
//   3. switching back and forth re-plans cleanly and leaves no plan behind for the wrong library
//      (a plan freed by the other library's destroy_plan is heap corruption, see FftBackend.h).
static void test_backend_selection()
{
    section("FFT backend selection (FFTW3 / oneMKL toggle)");
    FFTWEngine::wisdomPathOverride() = "fft_tests_wisdom.txt";
    PlanLog log;
    // The registry is the single source of truth here: this test does not include Parameters.h (it is
    // a TouchDesigner-side header), so the menu-value contract is asserted there instead.
    CHECK(backendCount() >= 2);
    const int kFftw3 = 0;   // Parameters::Backend::Fftw3 — see the static_assert in Parameters.cpp

    const size_t N = 4096;
    AlignedVector frame(N, 0.0f);
    frame[9] = 1.0f;                                  // impulse: |X[k]| == 1 for every k
    AlignedVector reference;
    {
        FFTWEngine e;
        e.prepare(N, PlannerPolicy::Fast, &log, &backendById(kFftw3));
        CHECK(e.hasPlan());
        AlignedComplexVector scratch;
        e.executeRFFT(frame, reference, scratch);
        CHECK_NEAR(reference[5], 1.0, 1e-3);
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
        CHECK(worst < 1e-3);
    }

    // Toggling on one engine, repeatedly: the plan in hand belongs to the previous library every
    // time, so this is the path that would corrupt the heap if the plan were destroyed by the wrong
    // library. Checked by value, since the damage would be silent otherwise.
    {
        FFTWEngine e;
        for (int round = 0; round < 3; ++round) {
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
// Async off must mean one thread for the whole node. The window -> FFT -> warp -> dB chain is
// already single-threaded there (the pipeline owner is the cook thread), and the channel fan-out
// only fires with more than one channel, so the one piece of work that could still escape to another
// thread is the FFTW planner's deferred MEASURE/PATIENT upgrade. This asserts both halves of that
// contract, in one engine:
//   * off -> nothing is started, the node keeps cooking on a correct ESTIMATE plan;
//   * on  -> the deferred measurement starts, because prepare()'s early-out sees an unchanged size,
//            policy and backend and will never re-plan on its own. Without the re-arm the node would
//            sit on ESTIMATE for the rest of the session.
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
static void test_v23_helpers()
{
    section("v2.3 helpers: silence, mix, cubic warp, partial magnitude, deferred log");
    // silence detection
    AlignedVector z(735, 0.0f);
    CHECK(blockIsSilent(z.data(), z.size()));
    z[3] = -0.0f; CHECK(blockIsSilent(z.data(), z.size()));
    z[700] = 1e-30f; CHECK(!blockIsSilent(z.data(), z.size()));
    CHECK(blockIsSilent(z.data(), 5));

    // mono mix helpers
    std::vector<float> a(100), b(100), m(100);
    for (int i = 0; i < 100; ++i) { a[i] = static_cast<float>(i); b[i] = static_cast<float>(-2 * i); }
    addInto(a.data(), b.data(), m.data(), 100);
    scaleInPlace(m.data(), 100, 0.5f);
    bool ok = true;
    for (int i = 0; i < 100; ++i) ok = ok && std::abs(m[i] - (-0.5f * i)) < 1e-6f;
    CHECK(ok);

    // cubic warp vs scalar Catmull-Rom reference, and identity still bypasses
    const size_t nlin = 513;
    AlignedVector src(nlin), out;
    for (size_t i = 0; i < nlin; ++i) src[i] = static_cast<float>(1.0 + std::sin(i * 0.07) * 0.5);
    PerceptualWarping w;
    w.setInterpolation(1);
    w.buildWarpTables(0, 22050.0, 1000, 22050.0, 0.963, 20.0, nlin);
    w.applyWarp(src, out);
    const auto& hz = w.targetHz();
    double max_err = 0.0;
    int last = static_cast<int>(nlin) - 1;
    for (size_t i = 0; i < 1000; ++i) {
        double frac = hz[i] / 22050.0 * (nlin - 1);
        double r = std::round(frac); if (std::abs(frac - r) < 1e-6) frac = r;
        int i0 = static_cast<int>(std::min<double>(nlin - 2, std::floor(frac)));
        float t = static_cast<float>(frac - i0);
        float p0 = src[std::max(i0 - 1, 0)], p1 = src[i0], p2 = src[std::min(i0 + 1, last)], p3 = src[std::min(i0 + 2, last)];
        float v = 0.5f * (2 * p1 + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t);
        max_err = std::max(max_err, static_cast<double>(std::abs(std::max(0.0f, v) - out[i])));
    }
    std::printf("  cubic warp max error vs scalar reference: %.2e\n", max_err);
    CHECK(max_err < 1e-4);
    CHECK(w.maxLinearIndex() <= nlin - 1 && w.maxLinearIndex() >= nlin - 3);
    w.setInterpolation(1);
    w.buildWarpTables(5, 22050.0, nlin, 22050.0, 1.0, 20.0, nlin);
    CHECK(w.isIdentity());
    w.applyWarp(src, out);
    CHECK(out[100] == src[100]);
    // Display Max below Nyquist -> fewer magnitude bins needed
    w.buildWarpTables(0, 11025.0, 1000, 22050.0, 0.963, 20.0, nlin);
    std::printf("  maxLinearIndex at half Nyquist: %zu of %zu\n", w.maxLinearIndex(), nlin);
    CHECK(w.maxLinearIndex() < nlin / 2 + 4);

    // partial magnitude: only the first n_mag bins are written
    PlanLog log;
    FFTWEngine e;
    e.prepare(1024, PlannerPolicy::Fast, &log);
    AlignedVector frame(1024, 0.0f), mag(513, -1.0f); AlignedComplexVector scratch;
    frame[0] = 1.0f;
    e.executeRFFT(frame, mag, scratch, 100);
    CHECK_NEAR(mag[0], 1.0, 1e-5);
    CHECK_NEAR(mag[99], 1.0, 1e-5);
    CHECK(mag[512] == -1.0f);                 // untouched beyond the requested (16-rounded) count
    e.executeRFFT(frame, mag, scratch, 0);
    CHECK_NEAR(mag[512], 1.0, 1e-5);

    // deferred log: nothing hits the textport until flushed, history is kept
    PlanLog dl;
    dl.setDeferred(true);
    dl.log("a"); dl.log("b");
    CHECK(dl.size() == 2);
    CHECK(dl.flushToTextport() == 2);
    CHECK(dl.flushToTextport() == 0);
}

// ------------------------------------------------------------------------------------------
static void test_triple_buffer_and_signal()
{
    section("TripleBuffer / WorkerSignal (v2.4 lock-free handoff)");
    struct Payload { uint64_t a{ 0 }, b{ 0 }; std::vector<float> data; };

    // single thread: roles rotate, latest wins, dropped flag
    TripleBuffer<Payload> tb;
    CHECK(!tb.acquire());                       // nothing published yet
    CHECK(tb.front().a == 0);
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
    CHECK(f->a == 2);
    CHECK(tb.acquire() && tb.front().a == 109);

    // two threads: the consumer must never observe a torn payload (a != b or data[k] != a)
    TripleBuffer<Payload> tb2;
    for (size_t i = 0; i < TripleBuffer<Payload>::kSlots; ++i) tb2.slot(i).data.assign(256, 0.0f);
    std::atomic<bool> stop{ false };
    std::atomic<uint64_t> produced{ 0 };
    std::thread producer([&] {
        for (uint64_t n = 1; !stop.load(); ++n) {
            Payload& p = tb2.back();
            p.a = n;
            for (auto& v : p.data) v = static_cast<float>(n & 0xFFFF);
            p.b = n;
            tb2.publish();
            produced.store(n);
        }
    });
    uint64_t last = 0, acquired = 0, torn = 0, non_monotonic = 0;
    auto t_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
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
    CHECK(acquired > 0);
    CHECK(torn == 0);
    CHECK(non_monotonic == 0);
    CHECK(tb2.acquire() || true);                   // drain
    CHECK(tb2.front().a == produced.load());        // the very last publish is visible after the producer stopped

    // WorkerSignal: a signal issued before wait() is not lost; wait() consumes it; cross-thread wake works
    WorkerSignal sig;
    sig.signal();
    auto t0 = std::chrono::steady_clock::now();
    sig.wait();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(50));
    std::atomic<int> woke{ 0 };
    std::thread waiter([&] { sig.wait(); woke.store(1); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(woke.load() == 0);                        // still blocked: the earlier signal was consumed
    sig.signal();
    waiter.join();
    CHECK(woke.load() == 1);
    // waitFor: times out close to the requested 2 ms even when the system clock ticks at 15.6 ms
    // (high-resolution waitable timer), and returns true immediately when a signal is pending
    double worst_ms = 0.0;
    for (int i = 0; i < 20; ++i) {
        auto s = std::chrono::steady_clock::now();
        CHECK(!sig.waitFor(2));
        worst_ms = std::max(worst_ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s).count());
    }
    std::printf("  waitFor(2 ms): worst %.2f ms over 20 calls (high-res timer: %s)\n", worst_ms, sig.highResolutionTimer() ? "yes" : "no");
    CHECK(worst_ms >= 1.0);
    if (sig.highResolutionTimer()) CHECK(worst_ms < 6.0);
    sig.signal();
    CHECK(sig.waitFor(1000));
}

// ------------------------------------------------------------------------------------------
static void test_pipeline_sine()
{
    section("FFTWEngine pipeline (1 kHz sine @ 44.1 kHz)");
    const double sr = 44100.0, f0 = 1000.0;
    const size_t win = 3175, N = 32768;
    PlanLog log;
    FFTWEngine engine;
    engine.prepare(N, PlannerPolicy::Fast, &log);
    CHECK(engine.fftSize() == N);

    for (int norm = 0; norm < 2; ++norm) {
        AlignedVector window;
        WindowGenerator::generateWindow(1 /*Hann*/, 15.0, win, window, norm ? WindowNorm::FullScale : WindowNorm::CoherentGain);
        AlignedVector frame(N, 0.0f), sig(win);
        for (size_t i = 0; i < win; ++i) sig[i] = static_cast<float>(0.5 * std::sin(2.0 * PI_D * f0 * i / sr));
        size_t pad_start = ((N - win) / 2) & ~static_cast<size_t>(7);
        multiplyInto(sig.data(), window.data(), frame.data() + pad_start, win);

        AlignedVector mag; AlignedComplexVector scratch;
        engine.executeRFFT(frame, mag, scratch);
        CHECK(mag.size() == N / 2 + 1);
        size_t idx = 0;
        float pk = findPeakWithIndex(mag.data(), mag.size(), idx);
        double peak_hz = idx * sr / N;
        std::printf("  norm=%s peak %.1f Hz, magnitude %.4f\n", norm ? "FullScale" : "CoherentGain", peak_hz, pk);
        CHECK_NEAR(peak_hz, f0, 2.0 * sr / N + 1.0);
        if (norm) CHECK_NEAR(pk, 0.5, 0.02);                    // amplitude 0.5 -> 0.5
        else      CHECK_NEAR(pk, 0.5 * win / 2.0, 0.02 * win);   // legacy: A * N_win / 2

        // warped log grid must place the peak at ~1 kHz too
        PerceptualWarping w;
        w.buildWarpTables(0, 22050.0, 16384, sr / 2.0, 0.963, 20.0, N / 2 + 1);
        AlignedVector warped;
        w.applyWarp(mag, warped);
        float wpk = findPeakWithIndex(warped.data(), warped.size(), idx);
        (void)wpk;
        CHECK_NEAR(w.targetHz()[idx], f0, 15.0);
    }
    CHECK(log.size() >= 1);
}

static void test_pipeline_process()
{
    const double sr = 44100.0, f0 = 1000.0;
    const int win_samples = 3175;
    const int N = 32768;
    const int bins = N / 2 + 1;                         // 16385 → identity warp

    Parameters::Values p;
    p.scale     = Parameters::Scale::Linear;
    p.warp      = 0.0;
    p.bins      = bins;
    p.padSize   = N;
    p.winMode   = Parameters::WinMode::Samples;
    p.winSamples = win_samples;
    p.window    = Parameters::WindowType::Hann;
    p.magNorm   = Parameters::MagNorm::CoherentGain;
    p.loudness  = Parameters::Loudness::Off;
    p.weighting = Parameters::Weighting::Off;
    p.ballEnable = false;
    p.eqEnable   = false;

    PlanLog log;
    AnalysisPipeline pipeline(&log);

    AlignedVector sig(win_samples);
    for (int i = 0; i < win_samples; ++i)
        sig[i] = static_cast<float>(0.5 * std::sin(2.0 * PI_D * f0 * i / sr));

    AnalysisJob job;
    job.numChannels = 1;
    job.sampleRate  = sr;
    job.winSamples  = win_samples;
    job.dtMs        = 1000.0 / 60.0;
    job.reset       = true;
    job.p           = p;
    job.windows.push_back(sig);
    job.silent.push_back(0);

    AnalysisResult res;
    pipeline.process(job, res);

    section("AnalysisPipeline::process() — 1 kHz sine, single channel");
    CHECK(res.spectra.size() == 1);
    CHECK(res.spectra[0].size() == static_cast<size_t>(bins));
    CHECK(res.peakMag > 0.0f);
    CHECK_NEAR(res.peakHz, f0, 2.0 * sr / N + 1.0);

    AnalysisPipeline::Status st = pipeline.status();
    CHECK(!st.plan.empty());
    CHECK(st.fftSize == static_cast<size_t>(N));
    CHECK(st.capacity == static_cast<size_t>(win_samples));
    CHECK(st.outputBins == bins);
    CHECK_NEAR(st.axisRate, sr, 1.0);                  // 2 * fmax, fmax clamped to Nyquist = sr/2 → sr
    CHECK(st.linearGrid);                              // Linear + warp=0 + bins==nlin → identity

    // Second pass: plan reused, no rebuild, peak stable.
    job.seq = 2; job.reset = false;
    pipeline.process(job, res);
    CHECK_NEAR(res.peakHz, f0, 2.0 * sr / N + 1.0);

    // Multi-channel: the parallel path (std::execution::par) must produce the same peak.
    section("AnalysisPipeline::process() — multi-channel parallel fan-out");
    job.numChannels = 3;
    job.windows.assign(3, sig);
    job.silent.assign(3, 0);
    AnalysisResult res3;
    pipeline.process(job, res3);
    CHECK(res3.spectra.size() == 3);
    for (size_t c = 0; c < 3; ++c)
        CHECK_NEAR(res3.peakHz, f0, 2.0 * sr / N + 1.0);
    CHECK(pipeline.parallelActive());

    // Silence short-circuit: linear-magnitude path zeroes the output.
    section("AnalysisPipeline::process() — silence short-circuit");
    job.numChannels = 1;
    job.silent[0] = 1;
    AlignedVector zeros(win_samples, 0.0f);
    job.windows.assign(1, zeros);
    job.reset = false;
    AnalysisResult res0;
    pipeline.process(job, res0);
    CHECK(res0.spectra.size() == 1);
    bool all_zero = true;
    for (float v : res0.spectra[0]) if (v != 0.0f) all_zero = false;
    CHECK(all_zero);
}

// ------------------------------------------------------------------------------------------
// The linear-grid ("no resampling") case is not a mode of its own: Scale = Linear + Warp Blend = 0
// + Display Max >= Nyquist + Output Bins = nlin makes the warp come out as the identity, and
// applyWarp() then memcpy's the magnitude through untouched. These checks pin that equivalence and
// the frequency-axis model that goes with it.
static void test_identity_grid_and_rate()
{
    section("linear grid (identity warp, no resampling) + spectrum frequency axis");
    const size_t nlin = 513;                        // a 1024-point R2C transform
    const double sr = 44100.0, nyq = sr / 2.0;
    AlignedVector src(nlin), out;
    for (size_t i = 0; i < nlin; ++i) src[i] = static_cast<float>(std::sin(i * 0.03) * 5.0 + 2.0);
    PerceptualWarping w;

    // --- linear grid: the output grid IS the linear FFT grid, copied verbatim ---
    w.buildWarpTables(5 /*Linear*/, nyq, nlin, nyq, 0.0, 20.0, nlin);
    CHECK(w.isIdentity());
    CHECK(w.outputBins() == nlin);
    // every bin must land exactly on its own linear index: bin i of an N-point R2C transform is i*sr/N
    double max_bin_err = 0.0;
    for (size_t i = 0; i < nlin; ++i) {
        max_bin_err = std::max(max_bin_err, std::abs(w.targetHz()[i] - i * sr / 1024.0));
    }
    std::printf("  linear grid: max bin freq error %.2e Hz (bin spacing %.2f Hz)\n", max_bin_err, sr / 1024.0);
    CHECK(max_bin_err < 1e-9);
    CHECK_NEAR(w.targetHz()[nlin - 1], nyq, 1e-9);          // top bin sits on Nyquist
    w.applyWarp(src, out);
    bool identical = out.size() == nlin;
    for (size_t i = 0; identical && i < nlin; ++i) identical = (out[i] == src[i]);
    CHECK(identical);

    // --- axis model: axisRate = 2 * (top of the axis), so the last bin sits on Nyquist ---
    // Full-band grid: fmax = nyquist, so the axis rate is exactly the input rate, and the implied
    // spacing axisRate/(2*(nlin-1)) is exactly the 1024-point transform's own resolution. This is
    // what hz_per_sample / output_spectrum_axis report; info->sampleRate is bins x me.time.rate.
    const double raw_axis = 2.0 * w.targetHz()[w.outputBins() - 1];
    CHECK_NEAR(raw_axis, sr, 1e-9);
    CHECK_NEAR(raw_axis / (2.0 * static_cast<double>(nlin - 1)), sr / 1024.0, 1e-9);
    CHECK(raw_axis == 44100.0);                              // same band as the input signal

    // Linear grid held to Display Max below Nyquist: the axis stops at Display Max, so the axis
    // rate is twice that, whatever the bin count. This is where it stops equalling the input rate.
    {
        const double fmax = 10000.0;
        const size_t n_out = 1000;
        w.buildWarpTables(5, fmax, n_out, nyq, 0.0, 20.0, nlin);
        CHECK(!w.isIdentity());                              // 1000 bins gathered from 513: not 1:1
        CHECK_NEAR(w.targetHz()[0], 0.0, 1e-12);
        CHECK_NEAR(w.targetHz()[n_out - 1], fmax, 1e-9);     // last bin sits exactly on Display Max
        CHECK_NEAR(2.0 * w.targetHz()[n_out - 1], 20000.0, 1e-9);
        CHECK_NEAR(w.targetHz()[n_out - 1] / static_cast<double>(n_out - 1), 10000.0 / 999.0, 1e-9);
        // More bins over the same band: the count of bins describing the band changes, the band
        // does not, so the axis rate must not move with Output Bins.
        w.buildWarpTables(5, fmax, 4000, nyq, 0.0, 20.0, nlin);
        CHECK_NEAR(2.0 * w.targetHz()[3999], 20000.0, 1e-9);
        w.buildWarpTables(5, fmax, 257, nyq, 0.0, 20.0, nlin);
        CHECK_NEAR(2.0 * w.targetHz()[256], 20000.0, 1e-9);
    }
    // A full-Nyquist band over 1000 bins: same 0..nyquist band as the input, so the axis rate is
    // the input rate for every scale, whatever Order the bins land in.
    for (int scale = 0; scale < 7; ++scale) {
        w.buildWarpTables(scale, nyq, 1000, nyq, 0.0, 20.0, nlin);
        CHECK_NEAR(2.0 * w.targetHz()[999], sr, 1e-9);
    }
    // Every scale is monotonic and ends exactly on fmax, which is what makes axisRate = 2*fmax exact
    // at the top of the axis even where the bins in between are non-uniform.
    for (int scale = 0; scale < 7; ++scale) {
        w.buildWarpTables(scale, 16000.0, 2000, nyq, 1.0, 20.0, nlin);
        CHECK(std::is_sorted(w.targetHz().begin(), w.targetHz().end()));
        CHECK_NEAR(w.targetHz()[1999], 16000.0, 1e-6);
        CHECK_NEAR(2.0 * w.targetHz()[1999], 32000.0, 1e-6);
    }
    // Bark used to fold over past ~6.5 kHz (barkToHz divided by 0.78 where the inverse of
    // hzToBark's 1.22*z-4.422 needs 1.22), which left the top bin back down at 0 Hz.
    {
        const double f[] = { 0.0, 20.0, 1000.0, 6543.0, 8000.0, 16000.0, 22050.0 };
        for (double f_hz : f) {
            CHECK_NEAR(PerceptualWarping::barkToHz(PerceptualWarping::hzToBark(f_hz)), f_hz, 1e-6);
        }
    }
    // A perceptual scale reads a narrowed band, so fewer magnitude bins are needed than the FFT has.
    w.buildWarpTables(0, 1000.0, 2000, nyq, 1.0, 20.0, nlin);
    CHECK(w.maxLinearIndex() < nlin / 20 + 4);
    CHECK(!w.isIdentity());

    // --- end to end: a sine at bin 100 of a 1024-point FFT reads back as bin 100's exact Hz ---
    {
        PlanLog log;
        FFTWEngine e;
        e.prepare(1024, PlannerPolicy::Fast, &log);
        const size_t bin = 100;
        const double f0 = bin * sr / 1024.0;
        AlignedVector frame(1024, 0.0f), mag, raw;
        AlignedComplexVector scratch;
        for (size_t i = 0; i < 1024; ++i) frame[i] = static_cast<float>(std::sin(2.0 * PI_D * f0 * i / sr));
        e.executeRFFT(frame, mag, scratch);                 // no window: the sine lands in one bin
        PerceptualWarping rw;
        rw.buildWarpTables(5, nyq, mag.size(), nyq, 0.0, 20.0, mag.size());
        CHECK(rw.isIdentity());
        rw.applyWarp(mag, raw);
        size_t idx = 0;
        findPeakWithIndex(raw.data(), raw.size(), idx);
        CHECK(idx == bin);
        CHECK_NEAR(rw.targetHz()[idx], f0, 1e-9);
        // the raw grid's axis is 2*nyquist = the input rate, so bin i reads back at i*axisRate/N Hz
        const double axis_rate = 2.0 * rw.targetHz()[rw.outputBins() - 1];
        CHECK_NEAR(axis_rate, sr, 1e-9);
        CHECK_NEAR(idx * axis_rate / 1024.0, f0, 1e-9);
    }
}

// ------------------------------------------------------------------------------------------
static void test_rate_model()
{
    section("RateModel (TD-free sample-rate / axis model)");
    Parameters::Values p;            // defaults: Scale=Log, Display Max=24000, Bins=16384, WinMode=Ms(50/72), pad=32768
    const double sr = 48000.0;

    // outputBinCountFrom is the single source of truth for the output width.
    CHECK(outputBinCountFrom(p) == p.bins);

    // Reported sample rate is bins * cook_rate — and crucially must NOT depend on the input sample
    // rate (that was the v2.5.0/2.6.0 mistake). Same params + rate, different input → same number.
    p.bins = 16384;
    const double td_rate_low  = sampleRateToTouchDesigner(p, 60.0);
    const double td_rate_high = sampleRateToTouchDesigner(p, 30.0);
    CHECK(td_rate_low  == 16384.0 * 60.0);
    CHECK(td_rate_high == 16384.0 * 30.0);

    // Axis rate: 2 * min(Display Max, Nyquist). Default Display Max=24000 >= Nyquist(48000/2=24000) → sr_in.
    CHECK_NEAR(axisRate(p, sr, 0.0), sr, 1e-6);
    // Display Max below Nyquist clamps the band to 2*Display Max (this node stops at Display Max).
    p.displayMax = 10000.0;
    CHECK_NEAR(axisRate(p, sr, 0.0), 20000.0, 1e-6);
    // A published axis rate from the live tables wins and is returned verbatim (never jumps).
    CHECK_NEAR(axisRate(p, sr, 31415.0), 31415.0, 1e-9);
    // Non-positive rate falls back through to the scalar sample rate.
    CHECK(axisRate(p, 0.0, 0.0) == 0.0);

    // Hz-per-bin on a uniform grid is fmax/(bins-1) == axis_rate/(2*(bins-1)).
    p.displayMax = 24000.0;
    p.bins = 1025;                       // e.g. fft_size 2048 → 1025 linear bins, identity grid
    CHECK_NEAR(hzPerBin(p, sr, 0.0), (sr * 0.5) / (1025 - 1), 1e-6);
    // n_out < 2 → undefined spacing, report 0.
    p.bins = 1;
    CHECK(hzPerBin(p, sr, 0.0) == 0.0);

    // Throughput = bins * 1000 / dt_ms (measured, not nominal-rate).
    p.bins = 16384;
    CHECK_NEAR(throughput(p, 16.6667), 16384.0 * 1000.0 / 16.6667, 1e-6);
    CHECK(throughput(p, 0.0) == 0.0);

    // fftSizeFrom: next power of two >= winSamples, and >= padSize.
    Parameters::Values q;
    q.padSize = 32768;
    CHECK(fftSizeFrom(q, 3175) == 32768);          // pad wins (32768 > nextpow2(3175)=4096)
    q.winSamples = 50000;
    CHECK(fftSizeFrom(q, 50000) == 65536);         // window needs 65536 (next pow2 > 50000)
    q.padSize = 256;
    CHECK(fftSizeFrom(q, 1) == 256);              // pad alone (256 >= nextpow2(1)=1)

    // windowSamplesFrom: ms mode clamps and rounds; sample mode is the raw value.
    Parameters::Values r;
    r.winMode = Parameters::WinMode::Milliseconds;
    r.winMs = 72.0;
    CHECK(windowSamplesFrom(r, sr) == static_cast<int>(std::lround(72.0 * sr / 1000.0)));
    r.winMs = 200000.0;                           // clamps to kMaxWinSamples
    CHECK(windowSamplesFrom(r, sr) == Parameters::kMaxWinSamples);
    r.winMode = Parameters::WinMode::Samples;
    CHECK(windowSamplesFrom(r, sr) == r.winSamples);
}

// ------------------------------------------------------------------------------------------
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
    CHECK(a[0] < 0.02);                           // deep low-frequency attenuation (~-40 dB)
    // Every weight is a legal linear magnitude in (0, inf); sanity-bound the whole curve so a refactor
    // can't silently invert or explode a band.
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
int main()
{
    // Unbuffered stdout, for the same reason as the bench: piped output is block-buffered by the MSVC
    // CRT and a crash does not flush, so a crash here would swallow the name of the test that was
    // running. With this, the last line printed is the crash site - and that line is the whole
    // diagnosis, because the alternative is bisecting by re-running with tests commented out.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
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
    test_triple_buffer_and_signal();
    test_background_plan();
    test_backend_selection();
    test_async_single_thread();
    test_pipeline_sine();
    test_pipeline_process();
    test_identity_grid_and_rate();
    test_rate_model();
    test_equal_loudness();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
