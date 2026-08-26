// Per-stage benchmark for the FFT plugin DSP pipeline (no TouchDesigner required).
//
//   build/bin/Release/fft_bench.exe [--channels N] [--fft N] [--win N] [--bins N] [--scale S]
//                                   [--iters N] [--planner auto|fast|measured] [--db 0|1|2]
//
// Prints microseconds per stage per channel and the total per cook, so the real cost of
// each parameter choice (FFT size, bins, scale, dB mode) can be measured instead of guessed.

#include "DSPModules.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace FFTDSP;
using clk = std::chrono::steady_clock;

struct Args {
    int channels = 2, fft = 32768, win = 3175, bins = 16384, scale = 0, iters = 200, db = 1, eq = 0, weight = 1, ball = 1;
    int interp = 0;            // 0 linear, 1 cubic
    double fmax = 24000.0;     // Display Max Hz (limits the magnitude bins computed)
    PlannerPolicy planner = PlannerPolicy::Auto;
};

static Args parse(int argc, char** argv)
{
    Args a;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        const char* v = argv[i + 1];
        if (k == "--channels") a.channels = std::atoi(v);
        else if (k == "--fft") a.fft = std::atoi(v);
        else if (k == "--win") a.win = std::atoi(v);
        else if (k == "--bins") a.bins = std::atoi(v);
        else if (k == "--scale") a.scale = std::atoi(v);
        else if (k == "--iters") a.iters = std::atoi(v);
        else if (k == "--db") a.db = std::atoi(v);
        else if (k == "--eq") a.eq = std::atoi(v);
        else if (k == "--weight") a.weight = std::atoi(v);
        else if (k == "--ball") a.ball = std::atoi(v);
        else if (k == "--interp") a.interp = std::atoi(v);
        else if (k == "--fmax") a.fmax = std::atof(v);
        else if (k == "--planner") {
            std::string p = v;
            a.planner = (p == "fast") ? PlannerPolicy::Fast : (p == "measured") ? PlannerPolicy::Measured : PlannerPolicy::Auto;
        }
    }
    return a;
}

struct Stage { const char* name; double us = 0.0; };

int main(int argc, char** argv)
{
    Args a = parse(argc, argv);
    const double sr = 44100.0;
    const size_t N = static_cast<size_t>(a.fft), win = static_cast<size_t>(a.win), bins = static_cast<size_t>(a.bins);
    std::printf("FFT bench: channels=%d fft=%zu win=%zu bins=%zu scale=%d db=%d iters=%d (AVX2 %s)\n",
                a.channels, N, win, bins, a.scale, a.db, a.iters,
#if defined(__AVX2__)
                "on"
#else
                "off"
#endif
    );

    PlanLog log;
    FFTWEngine engine;
    auto t0 = clk::now();
    engine.prepare(N, a.planner, &log);
    std::printf("plan: %s (%.1f ms)\n", engine.getPlanStatus().c_str(),
                std::chrono::duration<double, std::milli>(clk::now() - t0).count());

    AlignedVector window;
    WindowGenerator::generateWindow(0, 15.0, win, window, WindowNorm::CoherentGain);
    PerceptualWarping warp;
    warp.setInterpolation(a.interp);
    warp.buildWarpTables(a.scale, a.fmax > sr / 2 ? sr / 2 : a.fmax, bins, sr / 2.0, 0.963, 20.0, N / 2 + 1);
    const size_t n_mag = std::min(N / 2 + 1, warp.maxLinearIndex() + 1);
    std::printf("warp: %s interpolation, identity=%d, magnitude bins computed %zu of %zu\n",
                a.interp ? "cubic" : "linear", warp.isIdentity() ? 1 : 0, n_mag, N / 2 + 1);
    AlignedVector weighting;
    EqualLoudness::computeCurve(1, warp.targetHz(), weighting);

    struct Ch {
        FIFOBuffer fifo; BiquadEQ eq; BallisticsFilter ball;
        AlignedVector captured, processed, frame, mag, warped, prev;
        AlignedComplexVector scratch;
    };
    std::vector<Ch> chans(static_cast<size_t>(a.channels));
    for (auto& c : chans) {
        c.fifo.resize(win);
        c.frame.assign(N, 0.0f);
        c.captured.resize(win);
    }
    std::vector<float> block(735);
    for (size_t i = 0; i < block.size(); ++i) block[i] = static_cast<float>(0.3 * std::sin(i * 0.1) + 0.1 * std::sin(i * 1.7));
    size_t pad_start = ((N > win) ? (N - win) / 2 : 0) & ~static_cast<size_t>(7);

    Stage stages[] = { {"fifo"}, {"eq"}, {"window"}, {"fft+mag"}, {"warp"}, {"weighting"}, {"dB"}, {"ballistics"}, {"peak"} };
    auto tick = [](clk::time_point& t, Stage& s) {
        auto n = clk::now();
        s.us += std::chrono::duration<double, std::micro>(n - t).count();
        t = n;
    };

    // warm-up
    for (int it = 0; it < 5; ++it) {
        for (auto& c : chans) {
            c.fifo.add(block.data(), block.size());
            c.fifo.get(c.captured);
            multiplyInto(c.captured.data(), window.data(), c.frame.data() + pad_start, win);
            engine.executeRFFT(c.frame, c.mag, c.scratch);
            warp.applyWarp(c.mag, c.warped);
        }
    }

    auto total0 = clk::now();
    for (int it = 0; it < a.iters; ++it) {
        for (auto& c : chans) {
            auto t = clk::now();
            if (a.eq == 1) {
                // current: filter only the new block at ingest (stateful IIR, time order)
                bool has = c.eq.updateAndCheckActive(6.0, 1000.0, 0.0, 200.0, 0.707, 1.0);
                if (c.processed.size() < block.size()) c.processed.resize(block.size());
                std::memcpy(c.processed.data(), block.data(), block.size() * sizeof(float));
                if (has) c.eq.processBlockInPlace(c.processed.data(), block.size(), 1.0);
                c.fifo.add(c.processed.data(), block.size());
            } else {
                c.fifo.add(block.data(), block.size());
            }
            c.fifo.get(c.captured);
            tick(t, stages[0]);
            const float* src = c.captured.data();
            if (a.eq == 2) {
                // legacy: re-filter the whole analysis window every frame
                bool has = c.eq.updateAndCheckActive(6.0, 1000.0, 0.0, 200.0, 0.707, 1.0);   // plugin defaults: 6 dB high shelf
                if (has) { c.eq.processAudio(c.captured, 1.0, c.processed); src = c.processed.data(); }
            }
            tick(t, stages[1]);
            multiplyInto(src, window.data(), c.frame.data() + pad_start, win);
            tick(t, stages[2]);
            engine.executeRFFT(c.frame, c.mag, c.scratch, n_mag);
            tick(t, stages[3]);
            warp.applyWarp(c.mag, c.warped);
            tick(t, stages[4]);
            if (a.weight) multiplyInPlace(c.warped.data(), weighting.data(), c.warped.size());
            tick(t, stages[5]);
            if (a.db) {
                float pk = peakMagnitude(c.warped.data(), c.warped.size());
                DecibelConverter::convertToDB(a.db, 80.0, 1.0f / (pk > 0 ? pk : 1.0f), c.warped);
            }
            tick(t, stages[6]);
            if (a.ball) c.ball.apply(0.3f, 0.6f, c.warped, c.prev);
            tick(t, stages[7]);
            size_t idx; findPeakWithIndex(c.warped.data(), c.warped.size(), idx);
            tick(t, stages[8]);
        }
    }
    double total_us = std::chrono::duration<double, std::micro>(clk::now() - total0).count();
    double per_cook = total_us / a.iters;
    double per_ch = per_cook / a.channels;

    std::printf("\n%-12s %12s %10s\n", "stage", "us/channel", "share");
    double sum = 0;
    for (auto& s : stages) sum += s.us;
    for (auto& s : stages) {
        double us = s.us / (a.iters * a.channels);
        std::printf("%-12s %12.2f %9.1f%%\n", s.name, us, 100.0 * s.us / sum);
    }
    std::printf("%-12s %12.2f\n", "TOTAL", per_ch);
    std::printf("\nper cook (%d channels): %.1f us  = %.2f%% of a 60 fps frame, %.2f%% of a 120 fps frame\n",
                a.channels, per_cook, 100.0 * per_cook / 16666.7, 100.0 * per_cook / 8333.3);
    return 0;
}
