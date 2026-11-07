#ifndef ANALYSIS_PIPELINE_H
#define ANALYSIS_PIPELINE_H

/*
 * ===========================================================================
 *             ANALYSIS PIPELINE — HEADLESS DSP CORE
 * ===========================================================================
 * Header File: AnalysisPipeline.h
 *
 * TD-free. Depends only on DSPModules.h (FFTDSP namespace), Parameters.h and LayerPool.h.
 * No TouchDesigner C-ABI types, no CHOP_CPlusPlusBase — the pipeline is
 * unit-testable without TouchDesigner or a worker thread.
 *
 * Everything after the input history. Owned by exactly one thread at a time:
 * the worker thread when Async is on, the cook thread when it is off.
 * Inside one process() the work may fan out (channels over std::execution::par,
 * or Multi-FFT layers over the LayerPool), and each task then touches only its
 * own buffers plus read-only tables.
 * ===========================================================================
 *
 * WHAT THIS FILE IS, IN ONE PARAGRAPH
 * -----------------------------------
 * A CHOP cook hands the plugin a block of audio samples. This file turns the newest
 * window of them into a finished magnitude spectrum, and it is the whole DSP chain:
 *
 *     window -> FFT -> magnitude -> warp -> weighting -> dB -> ballistics
 *
 * where each stage is implemented in DSPModules.h and this file decides the ORDER, the
 * parameters, and the caching. Anything that must happen "once when a setting changes"
 * (regenerating windows, re-planning FFTs, rebuilding warp tables, recomputing the
 * weighting curve) happens in rebuild() / the update* functions, never per cook.
 *
 * LAYERS (v2.13). The chain runs over a list of analysis LAYERS. Single mode is one layer
 * (the Window Sampling window, gain 1) and costs exactly what the pre-2.13 pipeline did.
 * Multi-FFT is 2..6 layers with windows 1024 * 2^k; each layer's amplitude gain is folded
 * into its window coefficients, layers that share an FFT size form a GROUP whose
 * magnitudes are summed on the group's linear grid, and each group is warped once onto
 * the one shared output grid, where the groups are summed. Weighting, dB and ballistics
 * then run once, on the average. See process() in AnalysisPipeline.cpp, "Multi-FFT".
 *
 * HOW TO READ IT
 * --------------
 *   AnalysisJob    - the input: per channel, either a linear window or a SampleRing + end position
 *   AnalysisResult - the output: one spectrum per channel, plus the frame's peak
 *   AnalysisPipeline::process()      - the entry point; caching, layer fan-out, merge
 *   AnalysisPipeline::computeLayer() - window -> FFT -> |X| for one (channel, layer)
 *   AnalysisPipeline::finishChannel()- group sum -> warp -> weighting -> dB -> ballistics
 * The Status struct and its accessors are telemetry for the Info CHOP/DAT only;
 * nothing in the DSP path reads them.
 * ===========================================================================
 */

#include "DSPModules.h"
#include "Parameters.h"
#include "LayerPool.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------------------------
// Snapshot handed from the cook thread to the pipeline (one TripleBuffer slot)
// ---------------------------------------------------------------------------------------------
// WHAT: everything the pipeline needs to analyse one cooked frame. `seq` identifies the frame so a
// result can be matched back to the job that produced it.
//
// THE SAMPLES, two ways (v2.13):
//   * rings non-empty (the plugin): per channel a SampleRing the cook keeps writing, plus the ring
//     position `ringEnd` this job ends at. The worker windows each layer straight out of the ring;
//     the cook copies nothing but the new samples (see SampleRing in DSPModules.h).
//   * rings empty (tests, bench): `windows`, one linear window per channel, newest sample last, at
//     least winSamples long (shorter = still filling, zero-padded at the front).
struct AnalysisJob {
    uint64_t seq{ 0 };
    int numChannels{ 0 };
    double sampleRate{ 44100.0 };
    int winSamples{ 3175 };                       // the history one job reads: the window (single) or the largest layer (Multi)
    double dtMs{ 1000.0 / 60.0 };                 // audio time since the previous job (ballistics, AGC; RateModel audioFrameMs)
    bool reset{ false };                          // discard carried state (ballistics, AGC) before analysing
    Parameters::Values p;                         // full parameter snapshot: the pipeline sees no live UI state
    std::vector<FFTDSP::AlignedVector> windows;   // per channel: linear history (tests / bench path)
    std::vector<uint8_t> silent;                  // per channel: the whole winSamples history is digital silence
    std::vector<const FFTDSP::SampleRing*> rings; // per channel (plugin path); not owned, outlive every job
    std::vector<uint64_t> ringEnd;                // per channel: ring position this job ends at
    std::vector<uint32_t> silentRun;              // per channel: trailing digital-silence samples (per-layer skip); empty = use `silent`
    int64_t publishedNs{ 0 };                     // steady_clock ns at publish; the worker turns it into pickup latency
};

// ---------------------------------------------------------------------------------------------
// Result handed from the pipeline to the cook thread (one TripleBuffer slot)
// ---------------------------------------------------------------------------------------------
struct AnalysisResult {
    uint64_t seq{ 0 };                            // job this result belongs to (0 = none yet)
    float peakHz{ 0.0f };                         // channel 0 spectral peak (computed by the pipeline owner)
    float peakMag{ 0.0f };
    std::vector<FFTDSP::AlignedVector> spectra;   // per channel: outputBinCountFrom(p, sampleRate) floats
    FFTDSP::SpectralFeatures features;            // channel 0, when p.features is on (see DSPModules.h 6d)
    bool hasFeatures{ false };
};

class AnalysisPipeline {
public:
    // The log is not owned; it may be null. It receives the plan messages the engines produce.
    explicit AnalysisPipeline(FFTDSP::PlanLog* log);
    ~AnalysisPipeline();

    // Runs the full analysis for every channel of the job into res. Returns false when there is no
    // result for this job and the caller must keep the previous one: an FFT plan is waiting for the
    // process-wide planner (another measurement holds it), or the input ring was lapped while the
    // frame was read (a stalled worker). Not callable concurrently with itself.
    bool process(const AnalysisJob& job, AnalysisResult& res);

    struct Status {
        std::string plan;                    // the primary (largest) engine's plan description, plus a summary of the others
        std::string backend;                 // engine's own description of the live FFT library, or empty
        bool planFailed{ false };            // no usable plan (not merely waiting) — surface via getErrorString
        bool plannerWaiting{ false };        // a plan is waiting for the process-wide FFT planner (nothing blocks)
        size_t fftSize{ 0 }, capacity{ 0 }, linearBins{ 0 }, magnitudeBins{ 0 };
        double axisRate{ 0.0 };              // 2 x (top of the axis) for the grid that was built; NOT info->sampleRate
        double axisBottom{ 0.0 };            // target_hz[0]
        int outputBins{ 0 };
        bool linearGrid{ false };            // every group's warp is the identity: output == linear FFT bins
        bool planUpgrading{ false };         // a background measurement is running (any engine)
        double kaiserBeta{ 0.0 };            // the beta the windows were built with (Auto or Manual), 0 for other windows
        int aggregation{ 0 };                // 0 off, 1 peak, 2 rms (Warp Aggregation)
        size_t aggregatedBins{ 0 };          // output bins that aggregate >= 2 FFT bins (largest group)
        size_t abandonedMeasurements{ 0 };   // background plans abandoned by a size/backend change, still running
        // v2.13 Multi-FFT
        bool multi{ false };
        int layers{ 1 }, groups{ 1 }, layerThreads{ 1 };
        size_t layerWindow[Parameters::kMultiMaxLayers]{};
        size_t layerFft[Parameters::kMultiMaxLayers]{};
        double layerGain[Parameters::kMultiMaxLayers]{};
        int refreshDiv[Parameters::kMultiMaxLayers]{};
    };
    Status status() const;
    uint64_t statusVersion() const noexcept { return myStatusVersion; }
    double lastUs() const noexcept { return myLastUs; }
    double outputSampleRate() const noexcept { return myOutputSampleRate; }
    bool linearGrid() const noexcept { return myLinearGrid; }
    bool parallelActive() const noexcept { return myParallelActive.load(std::memory_order_relaxed); }
    // True if a group has no plan at all and is not merely waiting for the planner.
    bool planFailed() const noexcept;
    const FFTDSP::FftBackendInfo& backendInfo() const noexcept {
        const FFTDSP::FftBackendInfo* b = myBackend.load(std::memory_order_acquire);
        return b ? *b : FFTDSP::defaultBackend();
    }
    // v2.13 per-job telemetry (any thread, relaxed)
    int layersComputed() const noexcept { return myLayersComputed.load(std::memory_order_relaxed); }
    int layersSilent() const noexcept { return myLayersSilent.load(std::memory_order_relaxed); }
    uint64_t ringTornDrops() const noexcept { return myRingTorn.load(std::memory_order_relaxed); }
    uint64_t plannerHeldJobs() const noexcept { return myPlannerHeld.load(std::memory_order_relaxed); }

private:
    // One layer's per-channel buffers. Every (channel, layer) task writes only its own LayerBuf, which is
    // what makes the LayerPool fan-out race-free and its results independent of the thread count.
    struct LayerBuf {
        FFTDSP::AlignedVector frame, mag;     // zero-padded input frame (N), linear magnitude (N/2+1)
        FFTDSP::AlignedComplexVector scratch; // the r2c output (N/2+1)
        bool valid{ false };                  // mag holds a computed frame (Hop-Scaled reuse)
        bool contributes{ false };            // this job: mag is part of the sum (false = silent, exactly 0)
        bool torn{ false };                   // this job: the ring was lapped while reading
    };
    struct DspState {
        std::vector<LayerBuf> layers;
        FFTDSP::AlignedVector groupSum, warpTmp;   // Multi-FFT merge scratch
        FFTDSP::AlignedVector prev_spectrum;
        FFTDSP::AlignedVector prev_linear;    // features: previous frame's layer-1 magnitude (flux)
        FFTDSP::AlignedVector featureTime;    // features: layer-1 time samples (ring path)
        int prev_loudness_mode{ -1 };
        float agc_peak{ 0.0f };
    };
    struct Layer {
        size_t window{ 0 }, fftSize{ 0 }, padStart{ 0 };
        int group{ 0 };
        double gain{ 1.0 };
        int refreshDiv{ 1 }, refreshPhase{ 0 };
        FFTDSP::AlignedVector coef;           // window coefficients x gain (length = window)
    };
    struct Group {
        size_t fftSize{ 0 };
        FFTDSP::FFTWEngine* engine{ nullptr };   // owned by myEngines
        FFTDSP::PerceptualWarping warp;
        size_t magnitudeBins{ 0 };
        std::vector<int> layers;
    };

    struct WindowKey { int type{ -1 }; double beta{ -1.0 }; int norm{ -1 }; uint64_t layout{ 0 };
                       bool operator==(const WindowKey& o) const { return type == o.type && beta == o.beta && norm == o.norm && layout == o.layout; } };
    struct WarpKey   { int scale{ -1 }; double fmax{ -1.0 }; int bins{ -1 }; double warp{ -1.0 }; double floor{ -1.0 }; size_t nlin{ 0 }; double nyquist{ -1.0 }; int interp{ -1 }; int agg{ -1 }; uint64_t layout{ 0 };
                       bool operator==(const WarpKey& o) const { return scale == o.scale && fmax == o.fmax && bins == o.bins && warp == o.warp && floor == o.floor && nlin == o.nlin && nyquist == o.nyquist && interp == o.interp && agg == o.agg && layout == o.layout; } };
    struct WeightKey { int weighting{ -1 }; uint64_t warpVersion{ 0 };
                       bool operator==(const WeightKey& o) const { return weighting == o.weighting && warpVersion == o.warpVersion; } };

    void rebuild(const AnalysisJob& job);
    void prepareGroups(const Parameters::Values& p);  // (re)plan every group's engine in one batch; retries a waiting plan
    void ensureChannels(size_t n);
    void updateWindow(const Parameters::Values& p);
    void updateWarp(const Parameters::Values& p);
    void updateWeighting(const Parameters::Values& p);
    void updatePool(const Parameters::Values& p, int channels);
    bool layerSilent(const AnalysisJob& job, int ch, size_t window) const noexcept;
    void computeLayer(const AnalysisJob& job, int ch, int k) noexcept;
    void finishChannel(const AnalysisJob& job, int ch, float attackCoef, float releaseCoef, float agcDecay,
                       FFTDSP::AlignedVector& out) noexcept;
    static void layerTask(void* ctx, int task) noexcept;
    static void channelTask(void* ctx, int task) noexcept;

    // ---- engines and tables ---------------------------------------------
    FFTDSP::PlanLog* myLog;
    std::vector<std::unique_ptr<FFTDSP::FFTWEngine>> myEngines;   // one per FFT size in use; never destroyed mid-run (re-pointed instead)
    std::vector<Layer> myLayers;
    std::vector<Group> myGroups;               // ordered by fftSize, largest first
    FFTDSP::AlignedVector myWeightingCurve;
    std::vector<DspState> myChannels;
    LayerPool myPool;
    std::vector<std::vector<int>> myLayerBins, myChannelBins;   // LayerPool assignment, rebuilt with the layout
    int myBinsChannels{ -1 }, myBinsThreads{ -1 };
    uint64_t myBinsLayout{ 0 };

    // ---- the last-built configuration -------------------------------------
    double mySampleRate{ 0.0 };
    size_t myCapacity{ 0 };                 // the history one job reads (largest layer window)
    int myPadChoice{ -1 };
    bool myMulti{ false };
    int myLayerTarget{ 0 };                 // single: the window; multi: the layer count
    FFTDSP::PlannerPolicy myPlanner{ FFTDSP::PlannerPolicy::Auto };
    std::atomic<const FFTDSP::FftBackendInfo*> myBackend{ nullptr };
    WindowKey myWindowKey;
    WarpKey myWarpKey;
    WeightKey myWeightKey;
    uint64_t myLayout{ 0 };                 // bumped by every rebuild: invalidates all keys and layer buffers
    uint64_t myWarpVersion{ 0 };
    uint64_t myStatusVersion{ 1 };
    uint64_t myJobCounter{ 0 };             // Hop-Scaled refresh schedule
    bool myLastUpgrading{ false };
    bool myLastWaiting{ false };
    bool myLinearGrid{ false };
    double myOutputSampleRate{ 0.0 };
    double myLastUs{ 0.0 };
    double myKaiserBeta{ 0.0 };
    float myDbfsRef{ 1.0f };
    std::atomic<bool> myParallelActive{ false };
    std::vector<int> myChannelIndex;
    // per-job state handed to the pool trampolines (owner thread writes before run(), tasks read)
    const AnalysisJob* myCurJob{ nullptr };
    float myCurAttack{ 0.0f }, myCurRelease{ 0.0f }, myCurAgc{ 0.0f };
    std::vector<FFTDSP::AlignedVector>* myCurOut{ nullptr };
    std::atomic<int> myLayersComputed{ 0 }, myLayersSilent{ 0 };
    std::atomic<uint64_t> myRingTorn{ 0 }, myPlannerHeld{ 0 };
};

#endif // ANALYSIS_PIPELINE_H
