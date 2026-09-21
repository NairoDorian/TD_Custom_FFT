#ifndef ANALYSIS_PIPELINE_H
#define ANALYSIS_PIPELINE_H

/*
 * ===========================================================================
 *             ANALYSIS PIPELINE — HEADLESS DSP CORE
 * ===========================================================================
 * Header File: AnalysisPipeline.h
 *
 * TD-free. Depends only on DSPModules.h (FFTDSP namespace) and Parameters.h.
 * No TouchDesigner C-ABI types, no CHOP_CPlusPlusBase — the pipeline is
 * unit-testable without TouchDesigner or a worker thread.
 *
 * Everything after the FIFO. Owned by exactly one thread at a time:
 * the worker thread when Async is on, the cook thread when it is off.
 * Inside one process() the channel loop fans out over std::execution::par,
 * and each thread then touches only its own DspState plus read-only tables.
 * ===========================================================================
 */

#include "DSPModules.h"
#include "Parameters.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------------------------
// Snapshot handed from the cook thread to the pipeline (one TripleBuffer slot)
// ---------------------------------------------------------------------------------------------
struct AnalysisJob {
    uint64_t seq{ 0 };
    int numChannels{ 0 };
    double sampleRate{ 44100.0 };
    int winSamples{ 3175 };
    double dtMs{ 1000.0 / 60.0 };
    bool reset{ false };
    Parameters::Values p;
    std::vector<FFTDSP::AlignedVector> windows;   // per channel: linearized FIFO (winSamples floats)
    std::vector<uint8_t> silent;                  // per channel: whole window is digital silence
};

// ---------------------------------------------------------------------------------------------
// Result handed from the pipeline to the cook thread (one TripleBuffer slot)
// ---------------------------------------------------------------------------------------------
struct AnalysisResult {
    uint64_t seq{ 0 };                            // job this result belongs to (0 = none yet)
    float peakHz{ 0.0f };                         // channel 0 spectral peak (computed by the pipeline owner)
    float peakMag{ 0.0f };
    std::vector<FFTDSP::AlignedVector> spectra;   // per channel: p.bins floats
};

// ---------------------------------------------------------------------------------------------
// The full analysis: window -> FFT -> magnitude -> warp -> weighting -> dB -> ballistics.
// Owned by exactly one thread at a time.
// =============================================================================================
class AnalysisPipeline {
public:
    explicit AnalysisPipeline(FFTDSP::PlanLog* log);
    ~AnalysisPipeline();

    // Runs the full analysis for every channel of the job into res (spectra resized to p.bins, peak filled).
    void process(const AnalysisJob& job, AnalysisResult& res);

    // Telemetry (owner thread only). statusVersion() changes whenever status() would.
    struct Status {
        std::string plan;
        std::string backend;                 // engine's own description of the live FFT library, or empty
        bool planFailed{ false };          // prepare() could not produce a plan — surface via getErrorString
        size_t fftSize{ 0 }, capacity{ 0 }, linearBins{ 0 }, magnitudeBins{ 0 };
        double axisRate{ 0.0 };              // 2 x (top of the axis) for the grid that was built; NOT info->sampleRate
        double axisBottom{ 0.0 };            // target_hz[0]: DC for Mel/ERB/Linear, the log floor / 20 Hz / ~13 Hz otherwise
        int outputBins{ 0 };
        bool linearGrid{ false };            // the warp came out as the identity: output == linear FFT bins
        bool planUpgrading{ false };
    };
    Status status() const;
    uint64_t statusVersion() const noexcept { return myStatusVersion; }
    double lastUs() const noexcept { return myLastUs; }
    // 2 x the top of the axis read off the current tables (never fitted, never assumed); cheap accessor
    // for the cook thread's publication. This is the SPECTRUM axis rate, not the rate reported to
    // TouchDesigner — that one is FFT::outputSampleRate(), i.e. bins x me.time.rate. See the "Output
    // rate model" block in FFT.cpp.
    double outputSampleRate() const noexcept { return myOutputSampleRate; }
    // True when the current tables are the identity warp, i.e. the output grid IS the linear FFT grid.
    // Derived from the tables themselves (PerceptualWarping::isIdentity), never from a parameter, so it
    // cannot disagree with what applyWarp() actually does.
    bool linearGrid() const noexcept { return myLinearGrid; }
    // Atomic because it crosses threads: written by the pipeline owner (the worker when Async is on,
    // the cook thread when it is off) and read by the Info CHOP/DAT callbacks, which TouchDesigner calls
    // from its own thread. Plain relaxed telemetry — never used to make a decision.
    bool parallelActive() const noexcept { return myParallelActive.load(std::memory_order_relaxed); }
    // True if the engine has no plan ready (owner thread only; read once per published job by FFT::runJob
    // to decide whether to escalate the failure to getErrorString).
    bool planFailed() const noexcept { return !myEngine || !myEngine->hasPlan(); }
    // Which FFT library the engine last planned against. Never null after the first process().
    // A pointer to one of the two static descriptors in FftBackend.h, so reading it from another
    // thread is safe even while the owner thread switches backends: the pointee never moves.
    const FFTDSP::FftBackendInfo& backendInfo() const noexcept {
        return myBackend ? *myBackend : FFTDSP::defaultBackend();
    }

private:
    struct DspState {
        FFTDSP::AlignedVector padded_frame, rfft_magnitude, prev_spectrum;
        FFTDSP::AlignedComplexVector scratch_complex;
        int prev_loudness_mode{ -1 };
        float agc_peak{ 0.0f };
    };
    struct WindowKey { int type{ -1 }; double beta{ -1.0 }; size_t len{ 0 }; int norm{ -1 };
                       bool operator==(const WindowKey& o) const { return type == o.type && beta == o.beta && len == o.len && norm == o.norm; } };
    struct WarpKey   { int scale{ -1 }; double fmax{ -1.0 }; int bins{ -1 }; double warp{ -1.0 }; double floor{ -1.0 }; size_t nlin{ 0 }; double nyquist{ -1.0 }; int interp{ -1 };
                       bool operator==(const WarpKey& o) const { return scale == o.scale && fmax == o.fmax && bins == o.bins && warp == o.warp && floor == o.floor && nlin == o.nlin && nyquist == o.nyquist && interp == o.interp; } };
    struct WeightKey { int weighting{ -1 }; uint64_t warpVersion{ 0 };
                       bool operator==(const WeightKey& o) const { return weighting == o.weighting && warpVersion == o.warpVersion; } };

    void rebuild(const AnalysisJob& job);
    void updateWindow(const Parameters::Values& p);
    void updateWarp(const Parameters::Values& p);
    void updateWeighting(const Parameters::Values& p);
    // The whole per-channel pipeline (window -> FFT -> magnitude -> warp -> weighting -> dB ->
    // ballistics). Thread-safe: touches only its own DspState plus read-only tables, which is what
    // lets process() fan it out over cores.
    void runChannel(DspState& st, const FFTDSP::AlignedVector& window_in, bool silent, const Parameters::Values& p,
                    float attackCoef, float releaseCoef, float agcDecay, FFTDSP::AlignedVector& out) noexcept;

    FFTDSP::PlanLog* myLog;
    std::unique_ptr<FFTDSP::FFTWEngine> myEngine;
    FFTDSP::PerceptualWarping myWarping;
    FFTDSP::AlignedVector myWeightingCurve;
    FFTDSP::AlignedVector myWindowBuffer;
    std::vector<DspState> myChannels;

    double mySampleRate{ 0.0 };
    size_t myCapacity{ 0 };
    size_t myFFTSize{ 0 };
    size_t myPadStart{ 0 };
    int myPadChoice{ -1 };
    FFTDSP::PlannerPolicy myPlanner{ FFTDSP::PlannerPolicy::Auto };
    const FFTDSP::FftBackendInfo* myBackend{ nullptr };  // null until the first rebuild; see rebuild()
    WindowKey myWindowKey;
    WarpKey myWarpKey;
    WeightKey myWeightKey;
    uint64_t myWarpVersion{ 0 };
    uint64_t myStatusVersion{ 1 };
    bool myLastUpgrading{ false };
    size_t myMagnitudeBins{ 0 };
    bool myLinearGrid{ false };             // last built warp is the identity (see linearGrid())
    double myOutputSampleRate{ 0.0 };       // 2 x top of axis; see outputSampleRate() in FFT.cpp
    double myLastUs{ 0.0 };
    std::atomic<bool> myParallelActive{ false };   // last process() fanned out over channels (see above)
};

#endif // ANALYSIS_PIPELINE_H
