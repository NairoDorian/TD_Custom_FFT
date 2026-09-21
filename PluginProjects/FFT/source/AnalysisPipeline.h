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
 *
 * WHAT THIS FILE IS, IN ONE PARAGRAPH
 * -----------------------------------
 * A CHOP cook hands the plugin a block of audio samples. This file turns that
 * block into a finished magnitude spectrum, and it is the whole DSP chain:
 *
 *     window -> FFT -> magnitude -> warp -> weighting -> dB -> ballistics
 *
 * where each stage is implemented in DSPModules.h and this file decides the
 * ORDER, the parameters, and the caching. Anything that must happen "once when
 * a setting changes" (regenerating the window, rebuilding the warp tables,
 * recomputing the weighting curve) happens in rebuild(), never per cook.
 *
 * HOW TO READ IT
 * --------------
 *   AnalysisJob    - the input: one cooked block, already linearized per channel
 *   AnalysisResult - the output: one spectrum per channel, plus the frame's peak
 *   AnalysisPipeline::process()  - the entry point; does the caching and the fan-out
 *   AnalysisPipeline::runChannel() - the per-channel stage order (the file to read
 *                                    first if you want to understand the DSP)
 * The Status struct and its accessors are telemetry for the Info CHOP/DAT only;
 * nothing in the DSP path reads them.
 *
 * THREADING, IN ONE LINE: exactly one thread owns this object at a time, but a
 * single process() call may fan out over channels, so every per-channel buffer
 * must be per-thread state (DspState) and every shared table must be read-only
 * by the time the fan-out starts. Any change that breaks that rule is a race.
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
// WHAT: everything the pipeline needs to analyse one cooked frame, copied by value so the pipeline
// thread never touches the node's live state. `seq` identifies the frame so a result can be matched
// back to the job that produced it (and so a stale result can be discarded).
//
// WHY THE SAMPLES ARE COPIED IN: the FIFO's newest window has to keep receiving samples while the
// analysis of the previous frame is still running, so the pipeline must not hold a pointer into the
// live ring buffer. The copy is `numChannels` x `winSamples` floats (3175 by default), which is
// small enough to be cheaper than the synchronisation a shared buffer would need.
struct AnalysisJob {
    uint64_t seq{ 0 };
    int numChannels{ 0 };
    double sampleRate{ 44100.0 };
    int winSamples{ 3175 };                       // FIFO window length = padded_signal, NOT the FFT size
    double dtMs{ 1000.0 / 60.0 };                 // frame delta, used to make ballistics frame-rate independent
    bool reset{ false };                          // discard carried state (ballistics, AGC) before analysing
    Parameters::Values p;                         // full parameter snapshot: the pipeline sees no live UI state
    std::vector<FFTDSP::AlignedVector> windows;   // per channel: linearized FIFO (winSamples floats)
    std::vector<uint8_t> silent;                  // per channel: whole window is digital silence
};

// ---------------------------------------------------------------------------------------------
// Result handed from the pipeline to the cook thread (one TripleBuffer slot)
// ---------------------------------------------------------------------------------------------
// WHAT: what the cook thread picks up on a later cook. spectra[n] is channel n's finished spectrum,
// already `p.bins` long and in the units the parameters asked for (linear, dB, or dB normalised).
//
// WHY THE PEAK IS HERE AND NOT COMPUTED BY THE CONSUMER: it is measured on channel 0 only, and only
// so the Info CHOP can report a peak frequency without walking the whole spectrum from the info
// callback. It is telemetry, not part of the audio result.
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
//
// WHAT IT OWNS: the FFT engine (the plan), the three cached tables (window, warp, weighting
// curve), and one DspState per channel. Nothing here is TouchDesigner-aware - a cook hands it an
// AnalysisJob and it fills an AnalysisResult.
//
// THE THREE CACHES are the reason a steady-state cook is cheap. Each is guarded by a key struct
// (WindowKey / WarpKey / WeightKey) holding the parameters that feed it; if the key is unchanged
// the expensive table is not rebuilt. Their one rule: if you add a parameter that feeds a table,
// you MUST add it to that table's key, or the table will silently keep using the old value.
// Rebuilding costs one pass over the bins, not per-cook work, so the caches are about avoiding
// that pass and (for the warp) avoiding a re-plan of the FFT.
//
// WHERE THE TIME GOES, in rough order for a default configuration: the FFT itself, then the
// magnitude kernel, then the warp and the dB conversion. Everything else is noise.
class AnalysisPipeline {
public:
    // The log is not owned; it may be null. It receives the plan messages the engine produces.
    explicit AnalysisPipeline(FFTDSP::PlanLog* log);
    ~AnalysisPipeline();

    // Runs the full analysis for every channel of the job into res (spectra resized to p.bins, peak filled).
    // This is the only entry point that does work; everything else is telemetry. It is not callable
    // concurrently with itself, and it must be called by whichever thread currently owns the object.
    void process(const AnalysisJob& job, AnalysisResult& res);

    // Telemetry (owner thread only). statusVersion() changes whenever status() would.
    //
    // WHAT IT IS FOR: the Info CHOP and Info DAT rows. Every field here is derived from state the
    // pipeline already has; nothing here is used to make a DSP decision. That separation is
    // deliberate - it means displaying diagnostics can never change what the node outputs.
    struct Status {
        std::string plan;                    // the engine's plan description (see FFTWEngine::getPlanStatus)
        std::string backend;                 // engine's own description of the live FFT library, or empty
        bool planFailed{ false };          // prepare() could not produce a plan — surface via getErrorString
        size_t fftSize{ 0 }, capacity{ 0 }, linearBins{ 0 }, magnitudeBins{ 0 };
        double axisRate{ 0.0 };              // 2 x (top of the axis) for the grid that was built; NOT info->sampleRate
        double axisBottom{ 0.0 };            // target_hz[0]: DC for Mel/ERB/Linear, the log floor / 20 Hz / ~13 Hz otherwise
        int outputBins{ 0 };
        bool linearGrid{ false };            // the warp came out as the identity: output == linear FFT bins
        bool planUpgrading{ false };         // a background measurement is running (see FFTWEngine)
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
    // Everything one channel needs that must NOT be shared with another channel. One of these per
    // channel (myChannels), and during the parallel fan-out each worker thread touches only its own.
    // If you add a per-channel buffer to the pipeline, it belongs here and not in a member of
    // AnalysisPipeline itself - a shared scratch buffer would turn the fan-out into a data race that
    // only shows up as wrong numbers on multichannel input.
    struct DspState {
        FFTDSP::AlignedVector padded_frame, rfft_magnitude, prev_spectrum;
        FFTDSP::AlignedComplexVector scratch_complex;
        int prev_loudness_mode{ -1 };         // which dB mode produced the values in prev_spectrum;
                                              // a change of mode invalidates that history (-1 = none yet)
        float agc_peak{ 0.0f };               // the AGC follower's state, per channel on purpose:
                                              // a per-channel reference tracks each channel's own level
    };

    // The three cache keys. Each holds EXACTLY the parameters its table depends on, with a default
    // that no real parameter value can equal (so the first call always rebuilds). They are compared
    // with operator== by the update* functions below.
    //
    // THE RULE THAT MATTERS: a parameter that feeds a table must appear in that table's key. Nothing
    // enforces this at compile time, and forgetting one produces a stale table that only goes wrong
    // when the user changes that single parameter - which is exactly the kind of bug that survives a
    // quick test. When adding a parameter, find the table it feeds and add it here.
    struct WindowKey { int type{ -1 }; double beta{ -1.0 }; size_t len{ 0 }; int norm{ -1 };
                       bool operator==(const WindowKey& o) const { return type == o.type && beta == o.beta && len == o.len && norm == o.norm; } };
    // WarpKey carries nlin and nyquist as well as the user-visible settings: the warp tables are an
    // index mapping into the *linear* grid, so a change in the input sample rate (which moves Nyquist
    // and the linear bin count) invalidates them just as surely as a change of scale.
    // interp is in the key because updateWarp() calls setInterpolation() INSIDE its key-guarded
    // block - it is that method's only call site (the myWarping.setInterpolation(interp) line in
    // AnalysisPipeline::updateWarp). Drop interp from the key and changing the interpolation mode
    // alone would skip the block and never reach the warp at all: the parameter would silently do
    // nothing until some other warp setting was touched.
    struct WarpKey   { int scale{ -1 }; double fmax{ -1.0 }; int bins{ -1 }; double warp{ -1.0 }; double floor{ -1.0 }; size_t nlin{ 0 }; double nyquist{ -1.0 }; int interp{ -1 };
                       bool operator==(const WarpKey& o) const { return scale == o.scale && fmax == o.fmax && bins == o.bins && warp == o.warp && floor == o.floor && nlin == o.nlin && nyquist == o.nyquist && interp == o.interp; } };
    // The weighting curve is a function of the frequency axis (the warp tables) and the curve type,
    // so its key is exactly that: the curve code plus the version counter of the warp tables it was
    // built from. That is why a warp rebuild automatically invalidates the curve without anyone
    // having to remember to do it.
    struct WeightKey { int weighting{ -1 }; uint64_t warpVersion{ 0 };
                       bool operator==(const WeightKey& o) const { return weighting == o.weighting && warpVersion == o.warpVersion; } };

    // Decides what has to be rebuilt for this job and does it: window size/type, FFT plan, warp
    // tables, weighting curve. Called at the top of process().
    void rebuild(const AnalysisJob& job);
    void updateWindow(const Parameters::Values& p);       // regenerate the window if its key changed
    void updateWarp(const Parameters::Values& p);         // rebuild the warp tables if their key changed
    void updateWeighting(const Parameters::Values& p);    // recompute the weighting curve if its key changed
    // The whole per-channel pipeline (window -> FFT -> magnitude -> warp -> weighting -> dB ->
    // ballistics). Thread-safe: touches only its own DspState plus read-only tables, which is what
    // lets process() fan it out over cores.
    // noexcept on purpose: it runs off the cook thread, where an exception could not be reported to
    // anyone. Every buffer it writes is pre-sized by process() for that reason.
    // attackCoef/releaseCoef are per-frame ballistics coefficients (already converted from ms with
    // the job's dtMs); agcDecay is the AGC follower's decay for this frame.
    void runChannel(DspState& st, const FFTDSP::AlignedVector& window_in, bool silent, const Parameters::Values& p,
                    float attackCoef, float releaseCoef, float agcDecay, FFTDSP::AlignedVector& out) noexcept;

    // ---- the engine and the cached tables ---------------------------------
    FFTDSP::PlanLog* myLog;                 // not owned; may be null
    std::unique_ptr<FFTDSP::FFTWEngine> myEngine;   // owns the FFT plan (see DSPModules.h section 9)
    FFTDSP::PerceptualWarping myWarping;    // the warp tables and their identity flag
    FFTDSP::AlignedVector myWeightingCurve; // one gain per output bin (EqualLoudness::computeCurve)
    FFTDSP::AlignedVector myWindowBuffer;   // the window coefficients, length = winSamples
    std::vector<DspState> myChannels;       // per channel; resized when the channel count changes

    // ---- the last-built configuration, and the keys that detect a change ----
    double mySampleRate{ 0.0 };
    size_t myCapacity{ 0 };                 // FIFO window length in samples (the padded signal length)
    size_t myFFTSize{ 0 };                  // >= myCapacity; the FFT is zero-padded up to it
    size_t myPadStart{ 0 };                 // where the padded signal starts inside the FFT input buffer
    int myPadChoice{ -1 };                  // the pad setting the above were computed from
    FFTDSP::PlannerPolicy myPlanner{ FFTDSP::PlannerPolicy::Auto };
    const FFTDSP::FftBackendInfo* myBackend{ nullptr };  // null until the first rebuild; see rebuild()
    WindowKey myWindowKey;
    WarpKey myWarpKey;
    WeightKey myWeightKey;
    uint64_t myWarpVersion{ 0 };            // bumped on every warp rebuild; the weighting key reads it
    uint64_t myStatusVersion{ 1 };          // bumped whenever status() would return something different
    bool myLastUpgrading{ false };          // last reported planUpgrading, so a swap can bump the version
    size_t myMagnitudeBins{ 0 };            // how many linear bins actually need computing (maxLinearIndex)
    bool myLinearGrid{ false };             // last built warp is the identity (see linearGrid())
    double myOutputSampleRate{ 0.0 };       // 2 x top of axis; see outputSampleRate() in FFT.cpp
    double myLastUs{ 0.0 };                 // how long the last process() took, in microseconds
    std::atomic<bool> myParallelActive{ false };   // last process() fanned out over channels (see above)
};

#endif // ANALYSIS_PIPELINE_H
