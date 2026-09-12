// Does FFTW's built-in threading help this plugin? Measured answer: no.
//
//   build/bin/Release/fft_threads_probe.exe
//
// FFTW's threaded implementation parallelizes the `howmany` loop of a rank-1 (many-arrays) plan and
// multi-dimensional transforms — NOT the inside of a single 1-D transform. The plugin's default
// configuration (Channels = Mono Mix) issues exactly one 1-D R2C transform per cook, so handing
// FFTW threads there only buys thread hand-off cost. The vendored libfftw3f-3.dll does export
// fftwf_init_threads / fftwf_plan_with_nthreads, so this is worth measuring rather than assuming.
//
// Result on an i7-class desktop, N = 32768 with the plugin's zero-padding (3175 nonzeros), run
// twice on a machine where TouchDesigner was also running (so absolute numbers drift ~15 %; the
// sign and rough size of every delta reproduced):
//
//   single 1-D R2C      nthreads=1   68.8 / 79.7 us    nthreads=2   85.3 / 95.2 us   (+19..24 %)
//                       nthreads=4   81.7 / 93.8 us    nthreads=6  102.5 / 121.1 us  (+18..52 %)
//   howmany = 4 arrays  nthreads=1    86.3 / 97.0 us/tx nthreads=2    48.3 / 55.8 us/tx  (-36..42 %)
//                       nthreads=4    29.9 / 38.2 us/tx                      (-56..61 %)
//
// So threading is a real lever for Channels = All Channels (>= 2 transforms per cook), but it needs
// a batched fftwf_plan_many_dft_r2c plan, which FFTWEngine does not currently build. Re-run this
// probe on any machine before trusting the numbers above.

#include <fftw3.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

using clk = std::chrono::steady_clock;

static double timeMany(fftwf_plan p, int iters)
{
    for (int i = 0; i < 5; ++i) fftwf_execute(p);              // warm up
    auto t0 = clk::now();
    for (int i = 0; i < iters; ++i) fftwf_execute(p);
    auto t1 = clk::now();
    return std::chrono::duration<double, std::micro>(t1 - t0).count() / iters;
}

int main(int argc, char** argv)
{
    const int N = 32768, howmany = 4, iters = 300, nonzeros = 3175;
    (void)howmany;   // used only by the rank-1 section below
    // The plugin's real configuration uses a MEASURED plan (Auto upgrades in the background), and FFTW
    // measures *with* the thread count in effect, so ESTIMATE alone would not be a fair test.
    const unsigned rigors[] = { FFTW_ESTIMATE, FFTW_MEASURE };
    const char* rigor_names[] = { "ESTIMATE", "MEASURE" };
    const bool only_measure = (argc > 1 && std::string(argv[1]) == "--measure");
    if (!fftwf_init_threads()) { std::printf("fftwf_init_threads FAILED - this FFTW has no threads\n"); return 1; }

    const int nc = N / 2 + 1;
    float* in = fftwf_alloc_real(N);
    fftwf_complex* out = fftwf_alloc_complex(nc);
    for (int i = 0; i < N; ++i) in[i] = (i < nonzeros) ? std::sin(i * 0.01f) : 0.0f;

    for (int r = only_measure ? 1 : 0; r < 2; ++r) {
        const unsigned rigor = rigors[r];
        std::printf("\nsingle 1-D R2C, N = %d, %d nonzero, planner = %s\n", N, nonzeros, rigor_names[r]);
        double base = 0.0;
        for (int nth : {1, 2, 4, 6}) {
            fftwf_plan_with_nthreads(nth);
            fftwf_plan p = fftwf_plan_dft_r2c_1d(N, in, out, rigor);
            double us = timeMany(p, iters);
            if (nth == 1) base = us;
            std::printf("  nthreads=%d  %8.2f us   %+.0f%%\n", nth, us, 100.0 * (us - base) / base);
            fftwf_destroy_plan(p);
        }
    }

    float* inM = fftwf_alloc_real(static_cast<size_t>(N) * howmany);
    fftwf_complex* outM = fftwf_alloc_complex(static_cast<size_t>(nc) * howmany);
    for (int m = 0; m < howmany; ++m)
        for (int i = 0; i < N; ++i) inM[static_cast<size_t>(m) * N + i] = in[i];
    for (int r = only_measure ? 1 : 0; r < 2; ++r) {
        // A planned (MEASURE) thread split is the only one that matters: the plugin upgrades its plan
        // in the background, so this is the configuration it actually runs with.
        const int mi = (rigors[r] == FFTW_MEASURE) ? 30 : iters;
        std::printf("\nrank-1 loop: %d simultaneous transforms of N = %d, planner = %s (what FFTW splits)\n",
                    howmany, N, rigor_names[r]);
        double baseM = 0.0;
        for (int nth : {1, 2, 4}) {
            fftwf_plan_with_nthreads(nth);
            fftwf_plan p = fftwf_plan_many_dft_r2c(1, &N, howmany, inM, nullptr, 1, N,
                                                   outM, nullptr, 1, nc, rigors[r]);
            double us = timeMany(p, mi) / howmany;
            if (nth == 1) baseM = us;
            std::printf("  nthreads=%d  %7.2f us per transform   %+.0f%%\n", nth, us, 100.0 * (us - baseM) / baseM);
            fftwf_destroy_plan(p);
        }
    }
    fftwf_cleanup_threads();
    fftwf_free(in); fftwf_free(out); fftwf_free(inM); fftwf_free(outM);
    return 0;
}
