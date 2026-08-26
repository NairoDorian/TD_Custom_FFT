// Headless DSP unit tests for the FFT plugin (no TouchDesigner required).
//   ninja -C build && ctest --test-dir build --output-on-failure
//
// Golden-vector style checks: every SIMD path is compared against a scalar
// reference, and the full pipeline is checked against a known sine.

#include "DSPModules.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace FFTDSP;

static int g_failures = 0;
static int g_checks = 0;

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

// ------------------------------------------------------------------------------------------
int main()
{
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
    test_pipeline_sine();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
