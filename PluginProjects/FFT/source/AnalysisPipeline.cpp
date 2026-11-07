/*
 * ===========================================================================
 *             ANALYSIS PIPELINE — IMPLEMENTATION
 * ===========================================================================
 * Source File: AnalysisPipeline.cpp
 *
 * TD-free. Linked directly by tests/dsp_tests.cpp and bench/bench.cpp, so the pipeline is tested and
 * measured without TouchDesigner.
 * ===========================================================================
 *
 * WHAT LIVES HERE, AND IN WHAT ORDER TO READ IT
 * ---------------------------------------------
 *   status()          - builds the telemetry struct; reads state, changes nothing
 *   rebuild()         - the layer layout changed: windows, FFT sizes, groups, engines, buffers
 *   prepareGroups()   - plans every group's FFT in one batch (background upgrades held until all
 *                       sizes have their instant plan), and retries a plan waiting for the planner
 *   updateWindow()    - regenerates every layer's window coefficients (gain folded in)
 *   updateWarp()      - rebuilds each group's mapping onto the ONE shared output grid
 *   updateWeighting() - recomputes the equal-loudness curve on that grid
 *   computeLayer()    - window -> FFT -> |X| for one (channel, layer): the parallel unit
 *   finishChannel()   - group sum -> warp -> weighting -> dB -> ballistics, one channel
 *   process()         - the entry point: caching, the fan-out, features, the peak telemetry
 *
 * MULTI-FFT (v2.13), the whole design in one place
 * ------------------------------------------------
 *   * Layer k (k = 0..L-1) windows the newest 1024 * 2^k samples. Its amplitude gain g_k is folded
 *     into its window coefficients: |FFT(x w g)| = g |FFT(x w)|, so the scaling costs nothing per bin.
 *       Coherent Gain: g_k = (1/L) / 2^k   - a sine reads A*w/2, so every layer lands on the 1024
 *                                            layer's scale and the sum is the average (the spec).
 *       Full Scale   : g_k = 1/L           - the layers already read A whatever their length.
 *   * Layers that share an FFT size form a group (with the default 16K Zero-Pad Len: layers 1-5 run at
 *     N 16384, layer 6 at N 32768). A group's magnitudes are summed on its own linear grid, then warped
 *     ONCE onto the shared output grid (computeTargetHzGrid does not depend on N, so every group lands
 *     on exactly the same Hz values) and the groups are summed there. With interpolation this is
 *     identical to warping each layer; with Peak / RMS aggregation it is "the peak / RMS of the
 *     average", which is cheaper and peaks where the layers agree (design decision D5).
 *   * Weighting, dB (0 dBFS = the 1024 layer's coherent gain, 512, or 1 in Full Scale) and ballistics
 *     run once, on the average. Spectral features come from layer 1, the most reactive (D6).
 *   * A layer whose whole window is digital silence contributes exactly 0 and is not computed.
 *   * Hop-Scaled refresh recomputes the long layers every 2nd / 4th / 8th job and re-uses their
 *     previous magnitude in between (RateModel.h, layerRefreshDivisor).
 *   * The (channel, layer) tasks are spread over the LayerPool (Multi-FFT Layer Threads) - helpers at
 *     the worker's priority, balanced longest-first - and the merge runs per channel afterwards.
 *     Each task writes only its own LayerBuf, so the result does not depend on the thread count.
 *
 * NOTHING HERE ALLOCATES IN THE STEADY STATE. process() pre-sizes every buffer the noexcept stages
 * write; a size changes only with a setting.
 * ===========================================================================
 */

#include "AnalysisPipeline.h"
#include "RateModel.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <execution>
#include <numeric>
#include <string>

namespace {

using clk = std::chrono::steady_clock;
inline double usSince(clk::time_point t0) { return std::chrono::duration<double, std::micro>(clk::now() - t0).count(); }

// dst[i] += src[i]. A plain accumulate (addInto is restrict-qualified on all three pointers, and an
// in-place call would alias them).
inline void accumulate(float* dst, const float* src, size_t n) noexcept {
    size_t i = 0;
#if defined(__AVX2__)
    for (; i + 7 < n; i += 8) _mm256_storeu_ps(dst + i, _mm256_add_ps(_mm256_loadu_ps(dst + i), _mm256_loadu_ps(src + i)));
#endif
    for (; i < n; ++i) dst[i] += src[i];
}
// dst[i] = a[i] + b[i] (dst distinct from both).
inline void sumInto(const float* a, const float* b, float* dst, size_t n) noexcept {
    size_t i = 0;
#if defined(__AVX2__)
    for (; i + 7 < n; i += 8) _mm256_storeu_ps(dst + i, _mm256_add_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i)));
#endif
    for (; i < n; ++i) dst[i] = a[i] + b[i];
}

} // namespace

static_assert(static_cast<int>(Parameters::Planner::Auto) == static_cast<int>(FFTDSP::PlannerPolicy::Auto) &&
              static_cast<int>(Parameters::Planner::Fast) == static_cast<int>(FFTDSP::PlannerPolicy::Fast) &&
              static_cast<int>(Parameters::Planner::Measured) == static_cast<int>(FFTDSP::PlannerPolicy::Measured) &&
              static_cast<int>(Parameters::Planner::Patient) == static_cast<int>(FFTDSP::PlannerPolicy::Patient) &&
              static_cast<int>(Parameters::Planner::COUNT) == 4,
              "Parameters::Planner and FFTDSP::PlannerPolicy must keep the same numbering");
static_assert(static_cast<int>(Parameters::WarpAggregate::Off) == 0 && static_cast<int>(Parameters::WarpAggregate::Peak) == 1 &&
              static_cast<int>(Parameters::WarpAggregate::Rms) == 2,
              "WarpAggregate values are PerceptualWarping::setAggregation codes");

AnalysisPipeline::AnalysisPipeline(FFTDSP::PlanLog* log)
    : myLog(log)
{
}

AnalysisPipeline::~AnalysisPipeline()
{
    myPool.resize(0);   // join the helpers before the buffers they could touch go away
}

bool
AnalysisPipeline::planFailed() const noexcept
{
    if (myGroups.empty()) return true;
    for (const Group& g : myGroups)
        if (!g.engine || (!g.engine->hasPlan() && !g.engine->planPending())) return true;
    return false;
}

AnalysisPipeline::Status
AnalysisPipeline::status() const
{
    Status s;
    const Group* g0 = myGroups.empty() ? nullptr : &myGroups.front();
    const FFTDSP::FFTWEngine* e0 = g0 ? g0->engine : nullptr;
    s.plan = e0 ? e0->getPlanStatus() : std::string("Uninitialized");
    for (size_t i = 1; i < myGroups.size(); ++i)
        if (myGroups[i].engine) s.plan += " | " + myGroups[i].engine->getPlanStatus();
    s.backend = e0 ? e0->backendReport() : std::string();
    s.planFailed = planFailed();
    s.fftSize = g0 ? g0->fftSize : 0;
    s.capacity = myCapacity;
    s.linearBins = g0 ? FFTDSP::PerceptualWarping::linearBinCount(g0->fftSize) : 0;
    s.magnitudeBins = g0 ? g0->magnitudeBins : 0;
    s.axisRate = myOutputSampleRate;
    if (g0) {
        const std::vector<double>& hz = g0->warp.targetHz();
        s.axisBottom = hz.empty() ? 0.0 : hz.front();
        s.outputBins = static_cast<int>(g0->warp.outputBins());
        s.aggregation = g0->warp.aggregation();
        s.aggregatedBins = g0->warp.aggregatedBins();
    }
    s.linearGrid = myLinearGrid;
    s.kaiserBeta = myKaiserBeta;
    for (const Group& g : myGroups) {
        if (!g.engine) continue;
        s.planUpgrading = s.planUpgrading || g.engine->upgradeInProgress();
        s.plannerWaiting = s.plannerWaiting || g.engine->planPending();
        s.abandonedMeasurements += g.engine->abandonedMeasurements();
    }
    s.multi = myMulti;
    s.layers = static_cast<int>(myLayers.size());
    s.groups = static_cast<int>(myGroups.size());
    s.layerThreads = myPool.helpers() + 1;
    for (size_t k = 0; k < myLayers.size() && k < static_cast<size_t>(Parameters::kMultiMaxLayers); ++k) {
        s.layerWindow[k] = myLayers[k].window;
        s.layerFft[k] = myLayers[k].fftSize;
        s.layerGain[k] = myLayers[k].gain;
        s.refreshDiv[k] = myLayers[k].refreshDiv;
    }
    return s;
}

/*
rebuild() - the layer layout changed: the sample rate, single <-> Multi-FFT, the window (single) or
the layer count (Multi), the pad, the planner policy or the backend. It lays out the layers, groups
them by FFT size, hands each group an engine and plans them (prepareGroups), and drops every
per-channel buffer so ensureChannels() re-creates them for the new sizes (zeroed: the part of a frame
outside the window must be zero, which is what makes the transform zero-padded without a per-job
memset). The smoothed history and the AGC follower are reset because they belong to another grid.
*/
void
AnalysisPipeline::rebuild(const AnalysisJob& job)
{
    const Parameters::Values& p = job.p;
    mySampleRate = job.sampleRate;
    myMulti = p.multiFft;
    const int L = layerCount(p);
    myLayerTarget = myMulti ? L : std::max(1, job.winSamples);
    myPadChoice = p.zeroPad ? p.padSize : 0;
    myPlanner = static_cast<FFTDSP::PlannerPolicy>(p.planner);
    myBackend.store(&FFTDSP::backendById(static_cast<int>(p.backend)), std::memory_order_release);

    myLayers.assign(static_cast<size_t>(L), Layer{});
    myCapacity = 0;
    for (int k = 0; k < L; ++k) {
        Layer& ly = myLayers[static_cast<size_t>(k)];
        const int w = myMulti ? (Parameters::kMultiBaseWindow << k) : std::max(1, job.winSamples);
        ly.window = std::min<size_t>(static_cast<size_t>(w), FFTDSP::SampleRing::kCapacity / 2);
        ly.fftSize = fftSizeFrom(p, static_cast<int>(ly.window));
        // 8-float aligned centre offset: a shift of < 8 samples only changes phase, never magnitude,
        // and it keeps the FFT input 32-byte aligned for the aligned SIMD loads.
        const size_t centre = (ly.fftSize > ly.window) ? (ly.fftSize - ly.window) / 2 : 0;
        ly.padStart = centre & ~static_cast<size_t>(7);
        myCapacity = std::max(myCapacity, ly.window);
    }

    // Groups: one per distinct FFT size, largest first (the largest defines Auto / Raw output bins).
    std::vector<size_t> sizes;
    for (const Layer& ly : myLayers) if (std::find(sizes.begin(), sizes.end(), ly.fftSize) == sizes.end()) sizes.push_back(ly.fftSize);
    std::sort(sizes.begin(), sizes.end(), std::greater<size_t>());
    myGroups.clear();
    myGroups.resize(sizes.size());
    for (size_t g = 0; g < sizes.size(); ++g) {
        myGroups[g].fftSize = sizes[g];
        for (int k = 0; k < L; ++k)
            if (myLayers[static_cast<size_t>(k)].fftSize == sizes[g]) { myLayers[static_cast<size_t>(k)].group = static_cast<int>(g); myGroups[g].layers.push_back(k); }
    }

    // Engines: re-use one that already holds (or is waiting for) this size, then any spare one. Engines
    // are never destroyed here - an engine's destructor waits for its background measurement, and this
    // may be the cook thread - they are re-pointed by prepare() instead. At most one per group ever.
    std::vector<bool> used(myEngines.size(), false);
    for (Group& G : myGroups) {
        G.engine = nullptr;
        for (size_t i = 0; i < myEngines.size(); ++i) {
            if (used[i]) continue;
            const FFTDSP::FFTWEngine& e = *myEngines[i];
            if (e.fftSize() == G.fftSize || (e.planPending() && e.pendingSize() == G.fftSize)) { G.engine = myEngines[i].get(); used[i] = true; break; }
        }
    }
    for (Group& G : myGroups) {
        if (G.engine) continue;
        size_t i = 0;
        while (i < used.size() && used[i]) ++i;
        if (i == used.size()) { myEngines.push_back(std::make_unique<FFTDSP::FFTWEngine>()); used.push_back(false); }
        G.engine = myEngines[i].get();
        used[i] = true;
    }
    prepareGroups(p);

    for (DspState& ch : myChannels) {
        ch.layers.clear();
        ch.prev_spectrum.clear();
        ch.prev_linear.clear();
        ch.agc_peak = 0.0f;
    }
    ++myLayout;
    ++myStatusVersion;
}

// Plans every group in one batch. Order matters twice:
//   * setBackgroundAllowed(p.async) BEFORE prepare() (audit F5): a node created or loaded with Async
//     off used to start a background measurement on its first job, because the engine's flag starts
//     true and was only handed the toggle after the first plan.
//   * holdBackground() / releaseBackgroundHold() around the batch: every size gets its instant
//     (wisdom or ESTIMATE) plan first, and only then do the MEASURE / PATIENT upgrades start - otherwise
//     the second size's prepare() would find the planner lock taken by the first size's measurement.
// prepare() never blocks (try_lock); a group whose plan is waiting is retried by the next job.
void
AnalysisPipeline::prepareGroups(const Parameters::Values& p)
{
    const FFTDSP::FftBackendInfo* be = myBackend.load(std::memory_order_relaxed);
    for (Group& G : myGroups) {
        G.engine->setBackgroundAllowed(p.async);
        G.engine->holdBackground();
        G.engine->prepare(G.fftSize, myPlanner, myLog, be);
    }
    for (Group& G : myGroups) G.engine->releaseBackgroundHold();
}

// Brings the per-channel buffers in line with the layout. A no-op (size compares) in the steady state.
void
AnalysisPipeline::ensureChannels(size_t n)
{
    if (myChannels.size() != n) myChannels.resize(n);
    const size_t bins = myGroups.empty() ? 0 : myGroups.front().warp.outputBins();
    const size_t nlinMax = myGroups.empty() ? 0 : FFTDSP::PerceptualWarping::linearBinCount(myGroups.front().fftSize);
    for (DspState& st : myChannels) {
        if (st.layers.size() != myLayers.size()) {
            st.layers.clear();
            st.layers.resize(myLayers.size());
            for (size_t k = 0; k < myLayers.size(); ++k) {
                const size_t N = myLayers[k].fftSize, nlin = FFTDSP::PerceptualWarping::linearBinCount(N);
                st.layers[k].frame.assign(N, 0.0f);
                st.layers[k].mag.assign(nlin, 0.0f);
                st.layers[k].scratch.resize(nlin);
            }
        }
        if (myLayers.size() > 1) {
            if (st.groupSum.size() != nlinMax) st.groupSum.assign(nlinMax, 0.0f);
            if (st.warpTmp.size() != bins) st.warpTmp.assign(bins, 0.0f);
        }
        if (!myLayers.empty() && st.featureTime.size() != myLayers.front().window) st.featureTime.assign(myLayers.front().window, 0.0f);
    }
}

// Regenerates every layer's window when the type, the effective Kaiser beta, the normalization or the
// layout changed. The layer gain (RateModel layerGain) is folded into the coefficients here.
void
AnalysisPipeline::updateWindow(const Parameters::Values& p)
{
    const double beta = effectiveKaiserBeta(p);
    WindowKey key{ static_cast<int>(p.window), beta, static_cast<int>(p.magNorm), myLayout };
    if (key == myWindowKey) return;
    myKaiserBeta = (p.window == Parameters::WindowType::Kaiser) ? beta : 0.0;
    const FFTDSP::WindowNorm norm = p.magNorm == Parameters::MagNorm::FullScale ? FFTDSP::WindowNorm::FullScale
                                                                                : FFTDSP::WindowNorm::CoherentGain;
    for (size_t k = 0; k < myLayers.size(); ++k) {
        Layer& ly = myLayers[k];
        FFTDSP::WindowGenerator::generateWindow(static_cast<int>(p.window), beta, ly.window, ly.coef, norm);
        ly.gain = layerGain(p, static_cast<int>(k));
        if (ly.gain != 1.0) FFTDSP::scaleInPlace(ly.coef.data(), ly.coef.size(), static_cast<float>(ly.gain));
    }
    // 0 dBFS: the window's coherent gain (N_win / 2) in Coherent Gain - the 1024 layer's in Multi-FFT,
    // which is the scale every layer was brought to - and 1 in Full Scale, where magnitudes are absolute.
    myDbfsRef = (p.magNorm == Parameters::MagNorm::FullScale || myLayers.empty())
        ? 1.0f : static_cast<float>(myLayers.front().window) * 0.5f;
    for (DspState& st : myChannels) for (LayerBuf& lb : st.layers) lb.valid = false;   // no re-use across a window change
    myWindowKey = key;
    ++myStatusVersion;
}

/*
updateWarp() - rebuilds each group's mapping onto the one output grid, and derives from it which
linear bins are worth computing and the axis rate the node reports (sr_out = 2 * fmax: the last
output bin sits exactly on fmax for every scale - see computeTargetHzGrid and test_identity_grid).

The output count: Fixed = Output Bins; Auto and Raw = N/2+1 of the LARGEST group's FFT (the same
number outputBinCountFrom() declares to TouchDesigner, so the declared width and the built grid
cannot disagree). Raw RFFT Bins is a Linear DC..Nyquist grid with one bin per rfft bin of that FFT:
the identity for the largest group (a memcpy), an interpolation for the smaller ones.
*/
void
AnalysisPipeline::updateWarp(const Parameters::Values& p)
{
    if (myGroups.empty()) return;
    const size_t nlinMax = FFTDSP::PerceptualWarping::linearBinCount(myGroups.front().fftSize);
    const double nyquist = mySampleRate / 2.0;
    const bool   raw = p.rawBins;
    const double fmax = raw ? nyquist : std::min(p.displayMax, nyquist);
    const int    scale = raw ? static_cast<int>(Parameters::Scale::Linear) : static_cast<int>(p.scale);
    const double blend = raw ? 0.0 : p.warp;
    // audit F10: a Log Floor at or above Display Max made the log grid run backwards and collapse onto
    // one frequency. The floor is held below fmax (half of it: at least one octave of axis).
    const double logFloor = std::min(p.logFloor, 0.5 * fmax);
    const size_t n_out = (raw || p.binsMode == Parameters::BinsMode::Auto) ? nlinMax
                                                                            : static_cast<size_t>(outputBinCountFrom(p, mySampleRate));
    const int    interp = raw ? 0 : static_cast<int>(p.warpInterp);
    const int    agg = raw ? 0 : static_cast<int>(p.warpAggregate);

    WarpKey key{ scale, fmax, static_cast<int>(n_out), blend, logFloor, nlinMax, nyquist, interp, agg, myLayout };
    if (key == myWarpKey) return;
    bool allIdentity = true;
    for (Group& G : myGroups) {
        const size_t nlin = FFTDSP::PerceptualWarping::linearBinCount(G.fftSize);
        G.warp.setInterpolation(interp);
        G.warp.setAggregation(agg);
        G.warp.buildWarpTables(scale, fmax, n_out, nyquist, blend, logFloor, nlin);
        G.magnitudeBins = std::min(nlin, G.warp.maxLinearIndex() + 1);
        allIdentity = allIdentity && G.warp.isIdentity();
    }
    // A re-used (Hop-Scaled) magnitude was computed only up to the previous magnitudeBins.
    for (DspState& st : myChannels) for (LayerBuf& lb : st.layers) lb.valid = false;
    myOutputSampleRate = 2.0 * fmax;
    myLinearGrid = allIdentity;
    myWarpKey = key;
    ++myWarpVersion;
    ++myStatusVersion;
}

void
AnalysisPipeline::updateWeighting(const Parameters::Values& p)
{
    if (myGroups.empty()) return;
    const FFTDSP::PerceptualWarping& w = myGroups.front().warp;
    WeightKey key{ static_cast<int>(p.weighting), myWarpVersion };
    if (key == myWeightKey && myWeightingCurve.size() == w.outputBins()) return;
    FFTDSP::EqualLoudness::computeCurve(static_cast<int>(p.weighting), w.targetHz(), myWeightingCurve);
    myWeightKey = key;
}

// The layer threads. Helpers exist only in Multi-FFT with Async on: Async off means "one thread for the
// whole node", and single mode has one transform per channel (its channel fan-out stays on
// std::execution::par, unchanged). The task -> thread assignment is rebuilt only with the layout, the
// channel count or the thread count.
void
AnalysisPipeline::updatePool(const Parameters::Values& p, int channels)
{
    const int L = static_cast<int>(myLayers.size());
    const int tasks = L * std::max(1, channels);
    const int threads = (L > 1 && p.async) ? std::clamp(p.multiThreads, 1, std::max(1, tasks)) : 1;
    myPool.resize(threads - 1);
    if (myBinsLayout == myLayout && myBinsChannels == channels && myBinsThreads == threads) return;
    std::vector<double> cost(static_cast<size_t>(tasks));
    for (int t = 0; t < tasks; ++t) {
        const Layer& ly = myLayers[static_cast<size_t>(t % L)];
        const double N = static_cast<double>(ly.fftSize);
        cost[static_cast<size_t>(t)] = N * std::log2(std::max(2.0, N)) + 2.0 * static_cast<double>(ly.window);
    }
    LayerPool::balance(cost, threads, myLayerBins);
    std::vector<double> chCost(static_cast<size_t>(std::max(1, channels)), 1.0);
    LayerPool::balance(chCost, std::min(threads, std::max(1, channels)), myChannelBins);
    myBinsLayout = myLayout; myBinsChannels = channels; myBinsThreads = threads;
}

bool
AnalysisPipeline::layerSilent(const AnalysisJob& job, int ch, size_t window) const noexcept
{
    if (ch < static_cast<int>(job.silentRun.size())) return job.silentRun[static_cast<size_t>(ch)] >= window;
    return ch < static_cast<int>(job.silent.size()) && job.silent[static_cast<size_t>(ch)] != 0;
}

/*
computeLayer() - window -> FFT -> |X| for one (channel, layer). Thread-safe: it writes only this
channel's LayerBuf for this layer, and reads read-only tables plus the input (a ring the cook only
appends to, or the job's own windows). This is the unit the LayerPool runs in parallel.

  silent   - the layer's whole window is digital zero: its contribution is exactly 0, nothing runs.
  not due  - Hop-Scaled refresh and the layer already holds a frame: re-used as it is.
  torn     - the ring was lapped while it was read: the frame is dropped (process() returns false).
*/
void
AnalysisPipeline::computeLayer(const AnalysisJob& job, int ch, int k) noexcept
{
    DspState& st = myChannels[static_cast<size_t>(ch)];
    LayerBuf& lb = st.layers[static_cast<size_t>(k)];
    const Layer& ly = myLayers[static_cast<size_t>(k)];
    lb.torn = false;
    if (layerSilent(job, ch, ly.window)) { lb.contributes = false; lb.valid = false; return; }
    const bool due = !lb.valid || ly.refreshDiv <= 1 ||
                     (myJobCounter % static_cast<uint64_t>(ly.refreshDiv)) == static_cast<uint64_t>(ly.refreshPhase);
    if (!due) { lb.contributes = true; return; }

    float* dst = lb.frame.data() + ly.padStart;
    bool ok = true;
    if (!job.rings.empty()) {
        const FFTDSP::SampleRing* r = job.rings[static_cast<size_t>(ch)];
        if (r) ok = r->readTail(job.ringEnd[static_cast<size_t>(ch)], ly.window, ly.coef.data(), dst);
        else std::memset(dst, 0, ly.window * sizeof(float));
    } else {
        const FFTDSP::AlignedVector& w = job.windows[static_cast<size_t>(ch)];
        FFTDSP::readLinearTail(w.data(), w.size(), ly.window, ly.coef.data(), dst);
    }
    if (!ok) { lb.torn = true; lb.valid = false; lb.contributes = false; return; }

    const Group& G = myGroups[static_cast<size_t>(ly.group)];
    G.engine->executeRFFT(lb.frame, lb.mag, lb.scratch, G.magnitudeBins);
    // Full Scale: DC and Nyquist have no mirror bin (applied once, here, so a re-used frame is not
    // halved again by a later job).
    if (job.p.magNorm == Parameters::MagNorm::FullScale && lb.mag.size() >= 2) {
        lb.mag.front() *= 0.5f;
        lb.mag.back() *= 0.5f;
    }
    lb.valid = true;
    lb.contributes = true;
}

/*
finishChannel() - everything after the transforms, for one channel:
  1. sum each group's contributing layers on its linear grid, warp the group onto the output grid,
     and sum the groups there (single mode: one layer, one group - the warp reads the layer directly)
  2. weighting, 3. dB with the selected reference, 4. ballistics - once, on the average.
A torn frame leaves the channel untouched (the caller drops the job).
*/
void
AnalysisPipeline::finishChannel(const AnalysisJob& job, int ch, float attackCoef, float releaseCoef, float agcDecay,
                                FFTDSP::AlignedVector& out) noexcept
{
    const Parameters::Values& p = job.p;
    DspState& st = myChannels[static_cast<size_t>(ch)];
    const size_t bins = myGroups.front().warp.outputBins();
    assert(out.size() == bins);

    bool any = false;
    for (const LayerBuf& lb : st.layers) { if (lb.torn) return; any = any || lb.contributes; }

    // Digital silence in linear-magnitude mode with no smoothing: silence out, nothing else to do. (A dB
    // mode still has to publish the floor and ballistics still have to decay, so they run the chain.)
    if (!any && p.loudness == Parameters::Loudness::Off && !p.ballEnable) {
        std::memset(out.data(), 0, bins * sizeof(float));
        st.prev_spectrum.clear();
        return;
    }

    // 1. groups -> output grid
    bool wrote = false;
    for (const Group& G : myGroups) {
        const FFTDSP::AlignedVector* src = nullptr;
        int n = 0;
        const size_t mb = std::min(G.magnitudeBins, st.groupSum.empty() ? G.magnitudeBins : st.groupSum.size());
        for (int k : G.layers) {
            const LayerBuf& lb = st.layers[static_cast<size_t>(k)];
            if (!lb.contributes) continue;
            if (n == 0) src = &lb.mag;
            else if (n == 1) { sumInto(src->data(), lb.mag.data(), st.groupSum.data(), mb); src = &st.groupSum; }
            else accumulate(st.groupSum.data(), lb.mag.data(), mb);
            ++n;
        }
        if (n == 0) continue;
        if (!wrote) { G.warp.applyWarp(*src, out); wrote = true; }
        else { G.warp.applyWarp(*src, st.warpTmp); accumulate(out.data(), st.warpTmp.data(), bins); }
    }
    if (!wrote) std::memset(out.data(), 0, bins * sizeof(float));

    // 2. equal-loudness weighting (measuring the peak in the same pass when a dB reference needs it)
    const int loudness = static_cast<int>(p.loudness);
    const bool needRefPeak = loudness != 0 && p.dbRef != Parameters::DbRef::Dbfs;
    float refPeak = 0.0f;
    bool haveRefPeak = false;
    if (p.weighting != Parameters::Weighting::Off && myWeightingCurve.size() == out.size()) {
        if (needRefPeak) { refPeak = FFTDSP::multiplyInPlaceMax(out.data(), myWeightingCurve.data(), out.size()); haveRefPeak = true; }
        else FFTDSP::multiplyInPlace(out.data(), myWeightingCurve.data(), out.size());
    }

    // 3. dB. Crossing the Off boundary changes the unit, so the smoothing history is dropped there.
    if (loudness != 0) { if (st.prev_loudness_mode == 0) st.prev_spectrum.clear(); }
    else if (st.prev_loudness_mode != 0) st.prev_spectrum.clear();
    st.prev_loudness_mode = loudness;
    if (loudness != 0) {
        const float peak = needRefPeak ? (haveRefPeak ? refPeak : FFTDSP::peakMagnitude(out.data(), out.size())) : 0.0f;
        float ref = 1.0f;
        switch (p.dbRef) {
            case Parameters::DbRef::Dbfs:
                ref = myDbfsRef;   // the window's coherent gain (the 1024 layer's in Multi-FFT), 1 in Full Scale
                break;
            case Parameters::DbRef::Agc:
                st.agc_peak = std::max(peak, st.agc_peak * agcDecay);   // instant attack, smoothed release
                ref = st.agc_peak;
                break;
            case Parameters::DbRef::FramePeak:
            default:
                ref = peak;
                break;
        }
        if (!(ref > 0.0f)) ref = 1.0f;   // silence: the floor, not infinity
        FFTDSP::DecibelConverter::convertToDB(loudness, p.dbRange, 1.0f / ref, out);
    }

    // 4. ballistics (bypassed at 0/0); smooths in place and writes the history in the same pass
    if (attackCoef > 0.0f || releaseCoef > 0.0f) {
        FFTDSP::BallisticsFilter ball;
        ball.applyInPlace(attackCoef, releaseCoef, out, st.prev_spectrum);
    }
}

void
AnalysisPipeline::layerTask(void* ctx, int task) noexcept
{
    AnalysisPipeline& self = *static_cast<AnalysisPipeline*>(ctx);
    const int L = static_cast<int>(self.myLayers.size());
    self.computeLayer(*self.myCurJob, task / L, task % L);
}

void
AnalysisPipeline::channelTask(void* ctx, int ch) noexcept
{
    AnalysisPipeline& self = *static_cast<AnalysisPipeline*>(ctx);
    self.finishChannel(*self.myCurJob, ch, self.myCurAttack, self.myCurRelease, self.myCurAgc,
                       (*self.myCurOut)[static_cast<size_t>(ch)]);
}

/*
process() - THE ENTRY POINT. One call analyses one published job for every channel.
  1. rebuild the layout if it changed; otherwise retry a plan that was waiting for the planner
  2. hand the Async toggle to every engine, collect finished background plans
  3. hold (return false) while a plan is waiting - the previous spectrum stays on the output
  4. per-channel state, reset, the three cached tables, ballistics coefficients, output sizing
  5. the transforms: single mode as before (channel fan-out over std::execution::par); Multi-FFT over
     the LayerPool ((channel x layer) tasks, then the per-channel merge)
  6. a torn ring read drops the job; otherwise features (layer 1, channel 0) and the peak telemetry
*/
bool
AnalysisPipeline::process(const AnalysisJob& job, AnalysisResult& res)
{
    const auto t0 = clk::now();
    const Parameters::Values& p = job.p;
    const int nch = std::max(0, job.numChannels);

    const int target = p.multiFft ? layerCount(p) : std::max(1, job.winSamples);
    const bool rebuild_needed = myGroups.empty()
                             || std::abs(job.sampleRate - mySampleRate) > 1e-3
                             || p.multiFft != myMulti
                             || target != myLayerTarget
                             || (p.zeroPad ? p.padSize : 0) != myPadChoice
                             || static_cast<FFTDSP::PlannerPolicy>(p.planner) != myPlanner
                             // a plan belongs to the library that made it, so a backend switch re-plans
                             || &FFTDSP::backendById(static_cast<int>(p.backend)) != myBackend.load(std::memory_order_relaxed);
    if (rebuild_needed) rebuild(job);
    else {
        bool waiting = false;
        for (const Group& G : myGroups) waiting = waiting || G.engine->planPending();
        if (waiting) prepareGroups(p);   // try_lock again; cheap when the planner is still busy
    }

    bool upgrading = false, waiting = false;
    for (Group& G : myGroups) {
        G.engine->setBackgroundAllowed(p.async);   // every job: a flip of the toggle is never missed
        if (G.engine->pollBackgroundPlan()) ++myStatusVersion;
        upgrading = upgrading || G.engine->upgradeInProgress();
        waiting = waiting || G.engine->planPending();
    }
    if (upgrading != myLastUpgrading) { myLastUpgrading = upgrading; ++myStatusVersion; }
    if (waiting != myLastWaiting) { myLastWaiting = waiting; ++myStatusVersion; }
    if (waiting) {
        myPlannerHeld.fetch_add(1, std::memory_order_relaxed);
        myLastUs = usSince(t0);
        return false;
    }

    ensureChannels(static_cast<size_t>(nch));
    if (job.reset) {
        for (auto& ch : myChannels) {
            ch.prev_spectrum.clear(); ch.prev_linear.clear(); ch.agc_peak = 0.0f;
            for (LayerBuf& lb : ch.layers) lb.valid = false;
        }
    }

    updateWindow(p);
    updateWarp(p);
    updateWeighting(p);
    ensureChannels(static_cast<size_t>(nch));   // warpTmp follows the output count updateWarp just set
    for (size_t k = 0; k < myLayers.size(); ++k) {
        myLayers[k].refreshDiv = layerRefreshDivisor(p, static_cast<int>(k));
        myLayers[k].refreshPhase = layerRefreshPhase(static_cast<int>(k));
    }

    // Ballistics coefficients from the job's AUDIO time (audit F2; see RateModel audioFrameMs).
    float attackCoef = 0.0f, releaseCoef = 0.0f;
    if (p.ballEnable) {
        if (p.ballMode == Parameters::BallisticsMode::Milliseconds) {
            attackCoef  = FFTDSP::BallisticsFilter::coefFromMs(p.attackMs, job.dtMs);
            releaseCoef = FFTDSP::BallisticsFilter::coefFromMs(p.releaseMs, job.dtMs);
        } else {
            attackCoef  = static_cast<float>(p.attack);
            releaseCoef = static_cast<float>(p.release);
        }
    }
    const float agcDecay = FFTDSP::BallisticsFilter::coefFromMs(1500.0, job.dtMs);

    std::vector<FFTDSP::AlignedVector>& out = res.spectra;
    if (out.size() != static_cast<size_t>(nch)) out.resize(static_cast<size_t>(nch));
    const size_t bins = myGroups.front().warp.outputBins();
    for (auto& s : out) if (s.size() != bins) s.resize(bins);

    ++myJobCounter;
    const int L = static_cast<int>(myLayers.size());
    updatePool(p, nch);
    const bool pooled = L > 1 && myPool.helpers() > 0;
    myParallelActive.store(nch > 1 || pooled, std::memory_order_relaxed);

    if (pooled) {
        myCurJob = &job; myCurAttack = attackCoef; myCurRelease = releaseCoef; myCurAgc = agcDecay; myCurOut = &out;
        myPool.run(myLayerBins, &AnalysisPipeline::layerTask, this);
        if (nch > 1) myPool.run(myChannelBins, &AnalysisPipeline::channelTask, this);
        else if (nch == 1) finishChannel(job, 0, attackCoef, releaseCoef, agcDecay, out[0]);
        myCurJob = nullptr; myCurOut = nullptr;
    } else if (nch > 1) {
        // Several channels: the channel loop is embarrassingly parallel (each channel touches only its
        // own DspState). FFTW cannot split one 1-D transform (nthreads > 1 measured 13-51 % slower), so
        // the channel loop is where single-mode parallelism lives.
        if (myChannelIndex.size() != static_cast<size_t>(nch)) {
            myChannelIndex.resize(static_cast<size_t>(nch));
            std::iota(myChannelIndex.begin(), myChannelIndex.end(), 0);
        }
        std::for_each(std::execution::par, myChannelIndex.begin(), myChannelIndex.end(), [&](int ch) {
            for (int k = 0; k < L; ++k) computeLayer(job, ch, k);
            finishChannel(job, ch, attackCoef, releaseCoef, agcDecay, out[static_cast<size_t>(ch)]);
        });
    } else {
        for (int ch = 0; ch < nch; ++ch) {
            for (int k = 0; k < L; ++k) computeLayer(job, ch, k);
            finishChannel(job, ch, attackCoef, releaseCoef, agcDecay, out[static_cast<size_t>(ch)]);
        }
    }

    int computed = 0, silent = 0;
    bool torn = false;
    for (const DspState& st : myChannels)
        for (const LayerBuf& lb : st.layers) { torn = torn || lb.torn; if (lb.contributes) ++computed; else ++silent; }
    myLayersComputed.store(computed, std::memory_order_relaxed);
    myLayersSilent.store(silent, std::memory_order_relaxed);
    if (torn) {
        myRingTorn.fetch_add(1, std::memory_order_relaxed);
        myLastUs = usSince(t0);
        return false;
    }

    // Spectral features (channel 0, optional): from layer 1's LINEAR magnitude, so they do not depend on
    // the display axis and react as fast as the shortest window (D6). In Multi-FFT that layer carries
    // the gain 1/L; the band levels are corrected back to what a single 1024-sample analysis reads.
    res.hasFeatures = p.features && nch > 0 && !myChannels.empty();
    if (res.hasFeatures) {
        DspState& c0 = myChannels[0];
        const LayerBuf& l0 = c0.layers[0];
        const Layer& ly0 = myLayers[0];
        const Group& g0 = myGroups[static_cast<size_t>(ly0.group)];
        const size_t n = std::min(g0.magnitudeBins, l0.mag.size());
        if (!l0.contributes || n == 0) {
            res.features = FFTDSP::SpectralFeatures{};
            c0.prev_linear.clear();
        } else {
            const double binHz = ly0.fftSize ? mySampleRate / static_cast<double>(ly0.fftSize) : 0.0;
            const float* time = nullptr;
            size_t nTime = 0;
            if (!job.rings.empty() && job.rings[0]) {
                job.rings[0]->readTail(job.ringEnd[0], ly0.window, nullptr, c0.featureTime.data());
                time = c0.featureTime.data(); nTime = ly0.window;
            } else if (!job.windows.empty()) {
                const FFTDSP::AlignedVector& w0 = job.windows[0];
                nTime = std::min(w0.size(), ly0.window);
                time = w0.data() + (w0.size() - nTime);
            }
            FFTDSP::computeSpectralFeatures(l0.mag.data(), n, binHz, time, nTime, c0.prev_linear, res.features);
            if (myMulti && ly0.gain > 0.0) {
                const float corr = static_cast<float>(-20.0 * std::log10(ly0.gain));
                auto fix = [corr](float& db) { if (db > -239.0f) db += corr; };
                fix(res.features.bassDb); fix(res.features.midDb); fix(res.features.highDb);
            }
        }
    }

    res.peakMag = 0.0f;
    res.peakHz = 0.0f;
    if (!out.empty() && !out[0].empty()) {
        size_t max_idx = 0;
        res.peakMag = FFTDSP::findPeakWithIndex(out[0].data(), out[0].size(), max_idx);
        const std::vector<double>& hz = myGroups.front().warp.targetHz();
        if (max_idx < hz.size()) res.peakHz = static_cast<float>(hz[max_idx]);
    }
    myLastUs = usSince(t0);
    return true;
}
