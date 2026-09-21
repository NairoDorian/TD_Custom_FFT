# FFT Plugin — Real-Time Performance Analysis & Optimization Plan

> **What this document is.** A *measured* performance analysis of the plugin: a commit-by-commit
> sweep of a 15-commit window, the state of the code at the end of that window, and the ranked
> optimization plan that came out of it — with the status of every item in that plan, as of today.
>
> **Who it is for.** A developer who has to make this plugin faster, or who wants to know why a
> given stage costs what it costs. If you are *using* the node rather than changing it, read
> `README.md` instead.
>
> **How it relates to the other markdown files.**
>
> | File | What it holds |
> |---|---|
> | `README.md` | The user-facing description: what the node does, every parameter, how to build it, and the **current** performance numbers |
> | `CHANGELOG.md` | The version history, newest first, one entry per release |
> | **This file** | The performance analysis: a commit-by-commit sweep, the state at its end, and the optimization plan derived from it |
> | [`FFT_REALTIME_PERFORMANCE_ROADMAP.md`](FFT_REALTIME_PERFORMANCE_ROADMAP.md) | Forward-looking ideas that are **not** implemented |
>
> **Measurements vs. plans.** Sections 1 and 2 are *analysis of a specific revision* — they describe
> what was measured and when. Section 3 onward is a *plan*: proposals, each with an estimated saving.
> Every numbered priority carries a status marker: **DONE** (with the commit that did it and the
> symbol that implements it), **OBSOLETE** (superseded by later work), or **OPEN** (still true).
>
> **Which revision, and how stale is this?** The sweep covers commits `492b549` (2026-07-29) through
> `e9985c0` (2026-08-17). The current revision is **v2.9.1** (2026-09-21). A great deal has changed in
> between — most of all the extraction of the DSP core out of `FFT.cpp` into `source/DSPModules.h` and
> `source/AnalysisPipeline.h/.cpp`, the async worker thread (v2.3.0), the selectable FFT backend
> (v2.9.0), and the removal of several parameters. **Every statement below that is no longer true has
> been kept and explicitly marked obsolete**, with the reason and where to check — nothing has been
> silently deleted, because the reasoning is still a useful record of how the design got here.

> **Which machine, and how were the numbers taken?** The numbers in this file come from
> `bench/bench.cpp` (per-stage timing, interleaved runs, minimum per stage) on the **original
> development machine** — the one `README.md` calls "i7-class desktop". They are the same vintage and
> the same bench as the historical block in `README.md` § *Performance*, which names the machine
> there. The current machine is the **i9-13900H** (Raptor Lake, AVX2 + FMA, no AVX-512); every number
> in `README.md` marked i9 was re-taken there and is **not** comparable to the numbers below.
> **Flagged as unverified:** this document does not itself record the machine, the FFT size or the
> plan behind each figure — only the per-stage µs. Treat the *ratios* as the durable part and the
> absolute microseconds as a snapshot. Re-take any number with
> `build/bin/Release/fft_bench.exe --channels 1` before acting on it.

---

## 1. Commit-by-Commit Performance Analysis (Oldest → Newest)

> Commit hashes and subjects below were checked against `git log` and the sweep corrected where the
> original draft had the wrong hash on a description — see the **correction** notes. Two commits that
> the original draft missed entirely are now entries 6 and 11.

### 1. `492b549` — feat: Add explicit console logging for FFT plan generation, benchmarking, and FFTW_EXHAUSTIVE status
**Impact: NEUTRAL**

- Added `printf`/`fflush` logging for each plan-creation step (EXHAUSTIVE, PATIENT, MEASURE, ESTIMATE).
- Inserted `FFTW_PATIENT` as an intermediate fallback between `FFTW_EXHAUSTIVE` and `FFTW_MEASURE`.
- **Note**: `FFTW_EXHAUSTIVE` was already the pre-existing primary planner before this commit (not introduced here). This commit only made the fallback chain more verbose and added logging.
- The `printf`/`fflush` calls run **only during plan creation** (inside the plan-rebuild path, an infrequent operation), not in the per-frame hot path. Console logging itself is **not** a real-time regression.
- The actual problem was the pre-existing `FFTW_EXHAUSTIVE` primary planner (32 s stalls for N=16,384), which was later corrected by `cd94d9d` (→ `FFTW_MEASURE`) and `2daf9f1` (→ `FFTW_ESTIMATE`).
  - **Unverified**: the "32 s" figure is a historical observation, not reproduced in this document. The plan-time problem is real; the number is not checkable from the current tree.

### 2. `a9653da` — feat: Expose dynamic FFT plan status (FFTW_EXHAUSTIVE) directly to TouchDesigner Node Middle-Click
**Impact: NEUTRAL**

- Added the `getPlanStatus()` virtual method, implemented per engine.
  - **Obsolete detail**: the original draft named three implementations — `IFFTEngine` / `FFTWEngine` / `MKLEngine`. `MKLEngine` **no longer exists**: it was a byte-for-byte copy of the FFTW engine and was removed in v2.2.0 (see `CHANGELOG.md`, v2.2.0 → *Removed*). Today there is one implementation, `FFTWEngine`, which plans against whichever FFTW3-ABI library is selected at run time — see `IFFTEngine::getPlanStatus()` and `FFTWEngine::getPlanStatus()` in `source/DSPModules.h`.
- Pure diagnostic — no runtime cost during the transform.

### 3. `8debd78` — feat: Add high-resolution timer benchmarking and persistent file logging (fft_plan_log.txt)
**Impact: NEUTRAL at runtime, MIXED at plan-creation**

- Added `chrono::high_resolution_clock` timing around the plan call.
- Added per-plan `fft_plan_log.txt` file I/O via `std::ofstream` append.
- File I/O during plan creation adds latency, but plan creation only happens on rebuild (infrequent).
- The timing itself is useful for diagnostics.

### 4. `9432d24` — feat: Replace printf/fflush with direct Python print method execution in TouchDesigner Textport
**Impact: NEUTRAL**

> **Correction.** The original draft labelled this hash "Fix: Acquire Python GIL before PyRun_SimpleString".
> That is the *next* commit. `9432d24` is the printf-to-Python switch.

- Log lines were routed into the Textport by calling Python directly, instead of writing to a stdout
  TouchDesigner does not show.
- No perf impact on the audio processing path.

### 5. `260c009` — fix: Acquire Python GIL (PyGILState_Ensure) before calling PyRun_SimpleString to execute Python print
**Impact: NEUTRAL**

> **Correction.** The original draft labelled this hash "Fix: Absolute file path for fft_plan_log.txt,
> export history to Info DAT" — that is commit `baeb875` (entry 6). `260c009` is the thread-safety fix.

- Fixed a thread-safety bug: `PyRun_SimpleString` was called without holding the GIL.
- Correctly acquires `PyGILState_Ensure()` / `PyGILState_Release()` around Python calls.
- No perf impact on the audio processing path.
- **Still true today, and still load-bearing**: the same discipline is in
  `python_logger::writeToTextport()` in `source/DSPModules.h`, and it is why the plan log
  is buffered on the worker and flushed from the cook thread (`PlanLog::flushToTextport()`).

### 6. `baeb875` — fix: Set absolute file path for fft_plan_log.txt and export complete plan log history to Info DAT
**Impact: NEUTRAL**

> **Correction.** This commit was **missing entirely** from the original draft; its description had
> been attached to `260c009`.

- Hardcoded absolute path for the log file.
- Added an in-memory plan-log history vector (**256-entry cap**) for Info DAT export. Log history is
  accumulated in-memory — minor memory overhead, no runtime cost.
- **Still true today**: the 256-entry cap survives as `FFTDSP::kMaxPlanLogEntries`, now owned by the
  `PlanLog` class (`source/DSPModules.h`). The accessors have been
  renamed: what the commit called `getPlanLogHistory()` is now `PlanLog::snapshot()` /
  `PlanLog::snapshotTail(n, out)` / `PlanLog::entry()`. The class comment records why `snapshot()` is
  the wrong call in a per-cook path (it copies and allocates per line).

### 7. `c0e6537` — refactor: Remove file logging completely and route Python log commands via sys.stdout.write
**Impact: MIXED**

- **Removed file I/O** (`fft_plan_log.txt`) — eliminates disk writes during plan creation. **Positive.**
- Switched to `sys.stdout.write()` with `try/except/fallback` — more robust but slightly more Python code to execute per log call.
- Still uses `FFTW_EXHAUSTIVE` as primary planner (inherited from `492b549`).
- **Obsolete detail**: the `sys.stdout.write()` route was itself replaced in v2.2.0 by
  `PySys_WriteStdout`, resolved once from the loaded Python DLL. The commit message for v2.2.0 records
  why: the generated script had an escaping bug (`sanitizeForPython` stripped `"` but the script used
  `'`), and `PySys_WriteStdout` needs no script, no escaping, and made the plan log per-instance
  instead of process-global. Current code: `python_logger::writeToTextport()`,
  `source/DSPModules.h`.

### 8. `c85c5e0` — refactor: Set FFTW_PATIENT as primary planner flag (removing FFTW_EXHAUSTIVE) with FFTW_MEASURE and FFTW_ESTIMATE fallbacks
**Impact: POSITIVE (major) — planner stalls removed**

> **Correction.** The original draft gave this hash the title "Fix(EQ): Resolve critical EQ bug + fix
> AVX2 permute build break". Neither belongs to `c85c5e0`. The EQ fix is `d60b7e3` (entry 9) and the
> AVX2 permute workaround is `2daf9f1` (entry 14).

- Changed the primary planner: `FFTW_EXHAUSTIVE` → `FFTW_PATIENT`, with `FFTW_MEASURE` and
  `FFTW_ESTIMATE` as fallbacks.
- `FFTW_MEASURE` benchmarks a limited set of codelets — produces well-optimized plans with
  sub-millisecond creation time (vs the EXHAUSTIVE stalls).
- **Tradeoff**: MEASURE plans are slightly less optimal than PATIENT/EXHAUSTIVE, but the near-instant
  creation makes it viable for real-time.

### 9. `d60b7e3` — fix(EQ): Resolve critical bug where EQ filter coefficients were never updated because hasActiveFilter was evaluated before the design call
**Impact: MIXED (correctness win, minor perf regression)**

> **Correction.** The original draft gave this hash the title "perf: Upgrade computeMagnitudeAVX2_FMA
> to 2x unrolled 16-bin SIMD". That is `ee6169e` (entry 11).

- **CRITICAL FIX**: EQ filters were never activating. `hasActiveFilter()` was evaluated *before* the
  filter design call, so coefficients were always stale/inactive. The EQ was effectively a no-op
  (fast but wrong). Verified in the commit diff: `st.eq.hasActiveFilter(amount)` was replaced by
  `st.eq.updateAndCheckActive(...)`, which designs first and reports afterwards.
- **Obsolete detail**: `hasActiveFilter()` no longer exists anywhere in the tree. Its replacement is
  `BiquadEQ::updateAndCheckActive()` in `source/DSPModules.h`, which designs the shelves
  from a `std::tuple` cache key and *then* returns
  `(m_high_shelf.active || m_low_shelf.active) && (amount > 0.0)`.
- **New branching**: `processAudio` checks the shelf state per-sample inside the processing loop.
  **Obsolete — see Priority 5.** `BiquadSection::process()` (in `source/DSPModules.h`) now
  returns its input unchanged when the shelf is inactive, so the call is unconditional and there is no
  branch to mispredict. The plugin also no longer calls `processAudio` at all: it filters new samples
  at ingest with continuous IIR state via `BiquadEQ::processBlockInPlace()`
  (in `source/DSPModules.h`). `processAudio` survives only as the reference the tests compare
  against.
- Split `processAudio` from `updateAndCheckActive` — lazy redesign via parameter tuple hashing.
  **Positive** for avoiding redundant trig calculations. Still true: the key is
  `std::tuple<double,double,double,double,double>` of the five design parameters, and `amount` is
  deliberately *not* in it (it is an output blend, not a coefficient).

### 10. `cd94d9d` — perf: Remove FFTW_PATIENT from both FFTW and MKL engines, enforce FFTW_MEASURE as ultra-fast primary
**Impact: POSITIVE (major)**

- Then: `FFTW_PATIENT` → `FFTW_MEASURE` (removed PATIENT) as the primary planner.
- `FFTW_MEASURE` benchmarks a limited set of codelets — produces well-optimized plans with
  sub-millisecond creation time (vs the EXHAUSTIVE stalls).
- **Tradeoff**: MEASURE plans are slightly less optimal than PATIENT/EXHAUSTIVE, but the near-instant
  creation makes it viable for real-time.
- **Obsolete detail**: `FFTW_PATIENT` is back, as an *option* rather than a default — the
  `FFT Planner` menu has `Auto` / `Fast` / `Measured` / `Patient`, where `Patient` runs PATIENT on the
  **background** planner thread and never on a TouchDesigner thread. See `PlannerPolicy` in
  `source/DSPModules.h` and the `Parameters::Planner` enum in `source/Parameters.h` — the two are the
  same numbering and nothing in the source asserts that they stay in step, so a change to one has to be
  mirrored by hand in the other. `README.md` § *Parameters* measures Patient
  at −12 % FFT time at N = 32768 for ~3 s of once-per-size-per-machine planning.

### 11. `ee6169e` — perf: Upgrade computeMagnitudeAVX2_FMA to 2x unrolled 16-bin SIMD and add TouchDesigner getWarningString health telemetry
**Impact: POSITIVE (major)**

> **Correction.** This commit was **missing entirely** from the original draft; its description had
> been attached to `d60b7e3`.

- Upgraded `computeMagnitudeAVX2_FMA` from 8-bin SIMD to **16-bin SIMD** (2x unrolled, processes 16
  complex bins / 32 floats per loop iteration).
- This is one of the most significant real-time performance improvements in the history: the magnitude
  spectrum calculation is the post-FFT hot path, and doubling throughput here directly reduces
  per-frame CPU time. It is the change the current `README.md` features table summarises as
  "AVX2 / FMA everywhere it pays".
- Added `getWarningString` / `getErrorString` for health telemetry — no runtime cost per cook (they
  run inside a cook, but only produce a short string).
- Fixed a bug where `info->sampleRate` was set to `bins` (output bin count) instead of the input
  CHOP's actual sample rate in the no-input case. **Correctness + avoids downstream resampling in
  TouchDesigner.** Verified in the commit diff:
  `info->sampleRate = bins` → `info->sampleRate = cinput->sampleRate`.
  - **Obsolete detail**: the output sample rate reported to TouchDesigner is now
    `output bins × me.time.rate`, not the input rate — see `README.md` § *Output sample rate* and
    v2.6.0 in `CHANGELOG.md`. The input rate is still reported separately, on the spectrum's own axis.
- **Still true today**: `computeMagnitudeAVX2_FMA()` is in `source/DSPModules.h`, still
  16-wide 2x unrolled (`for (; i < n_vec16 * 16; i += 16)` with two 8-lane blocks), with 8-wide and
  scalar tails.
- **Note on the AVX2 permute workaround.** This commit used `_mm256_permute4x64_ps`, which the
  compiler rejects (there is no such intrinsic — the `pd`/`epi64` variants are the qword permutes).
  The fix landed in `2daf9f1` (entry 14), not in `c85c5e0` as the original draft claimed.

### 12. `ec3a9bf` — chore: sync Plugin_FFT.toe workspace file
**Impact: NEUTRAL**

- Binary workspace sync only.

### 13. `2daf9f1` — fix: recover standalone dedup changes + crash-guard + log scale fix
**Impact: MIXED (major positive for real-time reliability, minor negative for raw execution speed)**

- **FFTW_ESTIMATE primary** (MEASURE fallback): **Major positive.** FFTW_ESTIMATE creates plans
  instantly with zero benchmark overhead — no stalls during rebuilds. This is the correct choice for
  real-time. Tradeoff: ESTIMATE plans are less CPU-optimized than MEASURE plans, so the transform runs
  slightly slower per call. The tradeoff favours ESTIMATE for real-time (consistent, predictable frame
  times).
  - **Obsolete**: that tradeoff is now a **user choice**, not a hard-coded policy — `FFT Planner =
    Auto` (the default) plans instantly *and* upgrades to a measured plan on a background thread,
    cached in wisdom so only the first run of a size on a machine pays. See section 2.
- **AVX2 permute build break fixed**: replaced `_mm256_permute4x64_ps` with a `reorderLanes` lambda
  using `_mm256_castpd_ps` / `_mm256_permute4x64_pd` / `_mm256_castps_pd` — a free bit-pattern cast
  workaround. This restored the 16-bin SIMD magnitude pipeline. **Verified**: `git log -S` puts the
  first appearance of `reorderLanes` here, and the commit message leads with
  "Fix AVX2 `_mm256_permute4x64_ps` build break (reorderLanes lambda)".
  - **Still true today**: `reorderLanes` is in `source/DSPModules.h`, unchanged in shape.
- **Crash-guard**: `try/catch` around the cook and the pipeline. Exception handling has zero cost when
  no exceptions are thrown (modern C++ / MSVC zero-cost EH). Positive for robustness.
  - **Still true today**: `FFT::execute()` (in `source/FFT.cpp`) is the try/catch wrapper and
    `FFT::executeImpl()` is the work; `AnalysisPipeline::runChannel()` is `noexcept` on
    purpose because it runs off the cook thread, where an exception could not be reported to anyone.
- **Defensive clamps**: null checks, sample-rate clamp, channel-count clamp. Negligible cost.
  - **Still true, and the numbers have names now**: `kMinSampleRate` / `kMaxSampleRate` (1 … 384000)
    and `kMaxChannels` (64) in `source/Parameters.h`. The sample-rate clamp is applied
    in `FFT::executeImpl()` via `std::clamp`. **Correction**: the original draft says the clamp is
    `[1, 192000]`; the hard clamp today is **1 … 384000 Hz**, i.e. 192 kHz Nyquist — the upper end the
    `Display Max` slider range is chosen around. The channel clamp is `[0, 64]` in the original draft;
    the constant is `kMaxChannels = 64`, and the pipeline clamps to at least 1 channel.
- **Stage markers** (`myExecStage`): negligible overhead. Still true — declared in `source/FFT.h`, set to
  1, 2, 3 and 4 before the first four cook steps (`myExecStage` is left at 0 for the fifth, the deferred
  log flush, so an error there reports as "outside the cook"), and read by `FFT::execute()` to build the
  `"exception at stage N"` error string.
- **Extracted window and weighting helpers**: code quality improvement, no perf change.
  - **Obsolete names**: the helpers are no longer called `applyWindow` / `applyWeightingCurve`. They
    are the free functions `FFTDSP::multiplyInto()` (in `source/DSPModules.h`) and
    `FFTDSP::multiplyInPlace()`. Both are 16-wide 2x unrolled with 8-wide and scalar tails,
    and both document their alignment contract (32-byte-aligned inputs, unaligned outputs fine).
- **DecibelConverter**: Replaced the SIMD dB conversion loop with a **scalar** loop (peak search still
  SIMD). This is a **performance regression** — `std::log10` was called per-bin in a scalar loop. The
  comment acknowledged it: "log10 has no SIMD instruction; peak search above is vectorized, this pass
  is compute-bound on log10."
  - **OBSOLETE — this is the single biggest thing in this document that is no longer true.**
    `DecibelConverter::convertToDB()` (in `source/DSPModules.h`) is now fully vectorized: a fast
    `20·log10` from a 2048-entry mantissa LUT (`FastLog10`, in the same file) evaluated with
    `_mm256_i32gather_ps`, 16 bins per iteration with an 8-bin tail and a scalar remainder. It is no
    longer a hot spot, and Priority 1 below is DONE. See section 4, Priority 1.
- **Fixed scale case labels**: Log was `case 1` → `case 0`, etc. Correctness fix.
  - **Deeper root cause, still worth knowing**: `Scale = Linear` is `case 5` of
    `PerceptualWarping::computeTargetHzGrid()` (in `source/DSPModules.h`) and the `default:`
    arm is the linear grid — so any forgotten scale code silently produces a linear axis rather than
    failing. The `Scale` enum in `source/Parameters.h` says the same thing.
- **`ChannelState::initBuffers()`**: consolidated buffer init. No perf change.
  - **Obsolete name**: there is no `ChannelState` and no `initBuffers()`. The per-channel buffers now
    live in `AnalysisPipeline::DspState` (in `source/AnalysisPipeline.h`) and are sized by
    `AnalysisPipeline::rebuild()` (in `source/AnalysisPipeline.h`), which is called at the top of
    `AnalysisPipeline::process()`; `DspState` also grew `prev_loudness_mode` and `agc_peak` since.
- **Python logger**: moved to a `python_logger` namespace, 256-entry history cap. No runtime cost.
  - **Still true in substance**: the namespace and cap survive as `python_logger` and
    `kMaxPlanLogEntries` in `source/DSPModules.h`. But the logger is now **deferred**: worker-thread
    log lines are queued and flushed to the Textport by the cook thread
    (`PlanLog::log(msg, echoToTextport)`, `setDeferred`, `flushToTextport`, `hasPending`), so the
    steady state costs one relaxed atomic load per cook and no lock (in `source/FFT.cpp`).
- **CMake relocatability**: `${CMAKE_CURRENT_SOURCE_DIR}/../../../PluginBuilder_V2` instead of a
  hardcoded path. No runtime impact. Still true — `PluginProjects/FFT/CMakeLists.txt` still builds the
  plugin-builder path out of `${CMAKE_CURRENT_SOURCE_DIR}` rather than a literal absolute path.
- **CMake MSVC flags**: `/O2 /Oi /Ot /fp:fast /arch:AVX2` — all correct for real-time.
  - **Correction / addition**: the flags are no longer written in `CMakeLists.txt` itself. That file
    calls `td_plugin_optimize(FFT AVX2 FAST_MATH LTO)`, and the flags live in
    `PluginBuilder_V2/cmake/TDPlugin.cmake`. The full set today is
    **`/O2 /Oi /Ot /Gy /arch:AVX2 /fp:fast` plus `/GL` + `/LTCG`** (whole-program optimisation). See
    section 5.

### 14. `e9985c0` — fix: honor Logfloor parameter in Log and Melog frequency scales
**Impact: NEUTRAL**

- Corrected the `fmin` calculation in `computeTargetHzGrid` (was clamped to `max(1, fmax*0.1)`; now
  uses the floor directly).
- Pure correctness fix, no runtime performance impact.
- **Confirmed against the current source**: `PerceptualWarping::computeTargetHzGrid()`
  (in `source/DSPModules.h`) computes `log_min = std::log(std::max(1.0, log_floor_hz))` for
  both the Log (`case 0`) and Melog (`case 6`) arms — i.e. the floor is used directly, with only a
  1 Hz guard against `log(0)`. Note the floor is deliberately **ignored** by Chroma (`case 4`), which
  uses a fixed 20 Hz floor of its own; the same note is in `source/Parameters.h`.

### The window in one line

`492b549` → `e9985c0` is exactly **15 commits**, which is what the original draft's "last 15 commits"
claimed — it just listed 13 of them. The two it missed are `baeb875` (entry 6) and `ee6169e`
(entry 11).

---

## 2. State at the End of the Sweep (`e9985c0`), and How It Looks Today

> The left column below is the analysis as written at `e9985c0`. The right column says what is true
> now and where to check it. Nothing in the left column has been removed.

### 2.1 What was already optimized at `e9985c0`

| Area | Strategy at `e9985c0` | Status then | Where it is now |
|---|---|---|---|
| Plan creation | `FFTW_ESTIMATE` primary, `FFTW_MEASURE` fallback | Optimal for real-time — zero stalls | Superseded: `FFT Planner` menu (`Auto` / `Fast` / `Measured` / `Patient`) + wisdom caching. `PlannerPolicy`, `source/DSPModules.h` |
| FFT execution | Pre-built plans reused | Zero overhead at runtime | Still true — `IFFTEngine::executeRFFT()`, `source/DSPModules.h` |
| Magnitude spectrum | 16-bin AVX2 FMA SIMD, 2x unrolled | Optimal | Still true — `computeMagnitudeAVX2_FMA()`, `source/DSPModules.h` |
| Windowing | 16-float AVX2 SIMD, 2x unrolled | Optimal | Still true — `FFTDSP::multiplyInto()`, `source/DSPModules.h` |
| Weighting curve | 16-float AVX2 SIMD, 2x unrolled | Optimal | Still true — `FFTDSP::multiplyInPlace()`, `source/DSPModules.h` |
| Ballistics | 16-bin AVX2 FMA, 2x unrolled, bypass when disabled | Optimal | Still true — `BallisticsFilter::apply()`, `source/DSPModules.h` (branchless attack/release select via `blendv`) |
| EQ | Bypass when inactive, lazy redesign via tuple hash | Good (per-sample branching when active) | Improved: no per-sample branch (`BiquadSection::process()` returns its input when inactive), and it now runs at **ingest** on new samples only, with continuous IIR state |
| Zero-padding | Persisted, zero-filled once at rebuild | Optimal | Still true — `runChannel()` writes only the windowed region of the persistent frame; the rest stays zero |
| Output copy | `std::memcpy` to TD memory | Optimal | Still true — `FFT::copyResultsToOutput()`, `source/FFT.cpp` (called from `FFT::executeImpl()`) |
| Crash safety | `try/catch` wrappers, null clamping | Robust | Still true — `FFT::execute()` / `executeImpl()`, plus `noexcept` on the pipeline's hot path |
| Build flags | `/O2 /Oi /Ot /fp:fast /arch:AVX2` | Optimal | Extended: `+ /Gy`, `+ /GL /LTCG` (LTO), via `td_plugin_optimize(FFT AVX2 FAST_MATH LTO)` |

Whole sections that did not exist at `e9985c0` and therefore are not in this table: the async worker
thread (`FFT` ⇄ `AnalysisPipeline` handoff, v2.3.0/v2.4.0), the lock-free
`FFTDSP::TripleBuffer`, `FFTDSP::DenormalGuard` (FTZ/DAZ), the per-channel `std::execution::par`
fan-out, the runtime FFT backend registry (`source/FftBackend.h`), and the sample-rate/axis model in
`source/RateModel.h`.

### 2.2 Remaining CPU hot spots, as claimed at `e9985c0`

Kept verbatim, with today's verdict. Every row except the last has since been addressed.

| # | Hot Spot (as of `e9985c0`) | Current Implementation then | Estimated % of per-frame time (rough) | Verdict today |
|---|---|---|---|---|
| 1 | **Decibel (dB) conversion** | Scalar `std::log10` per-bin loop | **~15–30 %** (largest non-FFT cost) | **OBSOLETE** — now a 2048-entry mantissa LUT evaluated with `_mm256_i32gather_ps`, 16 bins/iteration. `DecibelConverter::convertToDB()` and `FastLog10`, both in `source/DSPModules.h` |
| 2 | **Peak frequency tracking** | `std::max_element` (scalar) in channel-0 telemetry | ~1–3 % (only on channel 0) | **OBSOLETE** — `FFTDSP::findPeakWithIndex()` (in `source/DSPModules.h`) is AVX2 movemask-gated, and since v2.4.0 the peak search runs on the **worker**, not the cook thread. Called from `AnalysisPipeline::process()`, `source/AnalysisPipeline.cpp` |
| 3 | **Warping interpolation** | 4-bin AVX2 (`i += 4`) — not 2x unrolled | ~5–10 % | **OBSOLETE** — `PerceptualWarping::applyWarp()` runs 8 output bins per iteration (`for (; i + 7 < n_out; i += 8)`, `source/DSPModules.h`). Note it is 8-wide, **not** 2x-unrolled to 16 like the magnitude kernel; that is a real (small) remaining difference, not an oversight in this table |
| 4 | **FFTW plan quality** | `FFTW_ESTIMATE` plans are sub-optimal vs `FFTW_MEASURE` | ~5–10 % (mitigated by instant creation) | **OBSOLETE** — wisdom caching plus the background `Auto` upgrade. Measured plans are ~40 % faster than estimated ones (`README.md` § *Features*) |
| 5 | **Python logging** | `GetModuleHandle` + `GetProcAddress` calls per log event | ~0 % (only during rebuild, not the hot path) | **OBSOLETE** — the CPython handles are resolved **once** per process in `python_logger::api()` (a function-local static backed by the `python_logger::Api` struct), and log lines are deferred and flushed by the cook thread |
| 6 | **EQ per-sample branching** | `if (active)` checks inside the sample loop | ~0–5 % (only when EQ active) | **OBSOLETE** — `BiquadSection::process()` returns `x` when inactive; the call is unconditional. `source/DSPModules.h` |

---

## 3. Real-Time Optimization Plan

> **Status of every item, in one place.** This section is the *plan*; the status column reflects work
> done after it was written. The full implementation record is in section 6.
>
> | Priority | Title | Status |
> |---|---|---|
> | 1 | Replace scalar `log10` in dB conversion | **DONE** (`adc8945`) |
> | 2 | AVX2 peak frequency tracking | **DONE** (`adc8945`, from stash `f1cb0d0`) |
> | 3 | 2x unroll warping interpolation | **DONE** (`adc8945`) |
> | 4 | FFTW wisdom persistence / "Performance Mode" | **DONE** (later than this list) — see 3.4 |
> | 5 | EQ branch elimination | **DONE** (`adc8945`) |
> | 6 | AVX2 dB conversion for normalized mode | **DONE** (`adc8945`, folded into Priority 1) |
> | 7 | Eliminate Python DLL resolution per log call | **DONE** (`adc8945`), by a different mechanism than proposed |

### 3.1 Priority 1 — Replace Scalar log10 in dB Conversion (Highest ROI)

**Problem (as written)**: `DecibelConverter::convertToDB()` loops over all bins calling `std::log10`
scalar. For 16,384 bins at 60 FPS this is ~1M `log10` calls/second.

**Option A: Lookup table + linear interpolation (recommended)**
- Pre-compute a `log10f` lookup table at startup (covering the dB range).
- Use `_mm256_i32gather_ps` or scalar lookup with a table + interpolation.
- Claimed 5–10x speedup over `std::log10`. **Estimate, not a measurement.**

**Option B: Fast log10 approximation (bit manipulation)**
- Use the IEEE 754 exponent trick: extract exponent, approximate mantissa log via polynomial.
- No memory overhead, claimed ~3–5x speedup. **Estimate.**
- Less accurate than table lookup but sufficient for dB display.

**Implementation (as written)**: Create a `FastLog10` utility with a static lookup table initialized
lazily on first call. Replace the scalar loop in `convertToDB`.

**Estimated savings**: 15–30 % reduction in per-frame CPU time for the dB conversion stage.

**What actually happened — DONE.** Both ideas landed together, as the *hybrid* the option list did not
propose: a table **indexed by bit manipulation**. `FastLog10` (in `source/DSPModules.h`) stores
`20·log10(1 + (i+0.5)/2048)` for the 2048 mantissa buckets of an 11-bit index and adds
`exponent × 20·log10(2)`, so there is no interpolation and one gather per 8 lanes. The table size is
**11 bits / 2048 entries / 8 KB**, and the class comment records that this is the only knob
(12 bits would halve the error for another 8 KB of the L1 the rest of the pipeline wants).
Worst-case error is 0.002 dB (`README.md` § *Features*); v2.2.1 in `CHANGELOG.md` records the
measured dB-stage cost at ~5 µs/channel. The speedup claim in the implementation log
("~15–20x over `std::log10`") is an **estimate** — it is not reproduced in this document and the
current `README.md` does not repeat it.

### 3.2 Priority 2 — AVX2 Peak Frequency Tracking

**Problem (as written)**: `std::max_element` over the warped spectrum (up to 16,384 bins) is scalar.

**Solution (as written)**: Use `_mm256_max_ps` reduction (as already used in `DecibelConverter` peak
search). Extract max + index via AVX2 + scalar fallback for the remainder.

**Estimated savings**: ~1–3 % per-channel reduction (only runs on channel 0).

**What actually happened — DONE.** `FFTDSP::findPeakWithIndex()` (in `source/DSPModules.h`)
does the vector pass as a *filter* (one compare and a movemask per 8 bins), and only spills a chunk to
memory and rescans it scalar when some lane beats the running max — because SIMD gives the value but
not the index. It uses `>` rather than `>=` so the **first** bin wins a tie, which keeps the reported
peak index stable frame to frame; the comment warns not to change it. Since v2.4.0 the whole search
runs on the **worker** (which owns the Hz table), so the cook thread copies two floats instead of
scanning a spectrum.

### 3.3 Priority 3 — 2x Unroll Warping Interpolation

**Problem (as written)**: `PerceptualWarping::applyWarp` processes 4 bins per AVX2 iteration
(`i += 4`). All other SIMD helpers use 2x unrolling (16 or 8 bins).

**Solution (as written)**: Unroll to 8 bins per iteration (`i += 8`), matching the pattern in
`applyWindow`, `applyWeightingCurve`, and `computeMagnitudeAVX2_FMA`.

**Estimated savings**: ~5 % reduction in warping stage time.

**What actually happened — DONE.** `PerceptualWarping::applyWarp()` is 8 bins per iteration, with a documented guard
(`linear_magnitude.size() >= m_nlin`) so a caller bug becomes a scalar read rather than a fault inside
a vector instruction. The named comparison targets have been renamed: `multiplyInto` and
`multiplyInPlace` (in `source/DSPModules.h`). There is also a cubic
Catmull-Rom path, `PerceptualWarping::applyWarpCubic()`, also 8-wide, selected by
`Warp Interpolation = Cubic`. The ~5 % figure remains an **estimate**.

### 3.4 Priority 4 — FFTW Wisdom Persistence (Optional)

**Problem (as written)**: `FFTW_ESTIMATE` produces generic plans. `FFTW_MEASURE` would produce faster
plans but costs ~1 ms per size at rebuild.

**Solution (as written)**: Use `fftwf_export_wisdom_to_file` / `fftwf_import_wisdom_from_file` to
persist measured plans between sessions. On startup, try to import wisdom; fall back to `FFTW_MEASURE`
with a timeout, then cache to disk.

**Alternative (as written)**: Add a user-toggleable "Performance Mode" parameter:
- **Real-Time** (default): `FFTW_ESTIMATE` — zero setup cost, consistent frames.
- **Benchmark** (one-time): `FFTW_MEASURE` — slightly faster execute, one-time setup stall. User manually applies.

**Estimated savings**: 5–10 % in FFT execution speed (if measured plans are used), at the cost of
one-time setup latency.

**What actually happened — DONE, and better than either proposal.** The wisdom route was taken
(`%LOCALAPPDATA%\TD_Custom_FFT\fftwf_wisdom.txt`, imported once per process under the planner mutex,
exported after every MEASURE plan), and the "Performance Mode" toggle became a four-entry **`FFT
Planner` menu**: `Auto` (instant plan now, measured upgrade on a background thread — the default and
the one to pick if you are unsure), `Fast` (ESTIMATE only), `Measured` (blocking, once per size),
`Patient` (like Auto but the background upgrade is `FFTW_PATIENT`). Nothing stalls a TouchDesigner
thread. Note the wisdom file is **FFTW3-specific**: a backend that reports no wisdom support is
described as such in the log rather than silently re-planning every run — the wisdom import/export
lives in `FFTWEngine` (`source/DSPModules.h`), and the capability it consults is
`FftBackendInfo::hasWisdom()` in `source/FftBackend.h`.

### 3.5 Priority 5 — EQ Branch Elimination

**Problem (as written)**: When EQ is active, the EQ path checks the shelf state inside the per-sample
loop, causing branch mispredictions.

**Solution (as written)**: Restructure to separate loops, or use branchless blending:
`filtered = x + amount * (shelf.process(x) - x)` where `shelf.process` returns `x` when inactive.

**Estimated savings**: ~2–5 % when EQ is active (negligible when inactive, since already bypassed).

**What actually happened — DONE, exactly as the second suggestion.** `BiquadSection::process()` returns
`x` immediately when `active` is false, so the branch is one perfectly-predicted early-out at most, and
the blend is the branchless form written above — see `BiquadEQ::processBlockInPlace()` and
`processAudio()`, `source/DSPModules.h`. Separately, the EQ moved from the analysis
window to **ingest** (v2.2.2): each sample is filtered exactly once, in time order, with continuous IIR
state, which is both cheaper and correct (the old per-window re-filter restarted the filter from a
stale state every frame). `README.md` measures the EQ at ~3.6 µs/channel at the default shelf
settings, and re-measured it at **0.01 µs** on the i9 — treat the EQ as free at default settings.

### 3.6 Priority 6 — AVX2-Accelerated dB Conversion for Normalized Mode

**Problem (as written)**: `mode == 2` (normalized dB) did `std::max(0.0f, std::min(1.0f, ...))` per
bin. These min/max calls can be SIMD-vectorized.

**Solution (as written)**: After computing `db` with the fast log10, use `_mm256_max_ps` /
`_mm256_min_ps` for clamping when in normalized mode. This combines with Priority 1.

**What actually happened — DONE.** `DecibelConverter::convertToDB()` has a dedicated `mode == 2` vector
path (in `source/DSPModules.h`) that clamps the magnitude at `1e-12`, applies the LUT,
floors at `-top_db`, rescales, and clamps to `[0, 1]` — all in `_mm256_max_ps` / `_mm256_min_ps`, 16
bins at a time. Note the mode codes: **0 = pass through, 1 = dB, 2 = normalized dB** (the enum's
comments in `source/Parameters.h` warn that a new entry silently becomes another plain dB
mode, because `convertToDB` tests for 0 and 2 explicitly).

### 3.7 Priority 7 — Eliminate Python DLL Resolution on Every Log Call

**Problem (as written)**: the Python logger resolved the Python module on each log event. Only runs
during plan creation (not per-frame), so low priority.

**Solution (as written)**: Cache the `HMODULE` and resolved function pointers after first call.

**Estimated savings**: Negligible for the real-time path (plan creation only).

**What actually happened — DONE, by a different route.** There is no `writeToPythonConsole` and no
`python_logger::resolvePythonModule()` any more. The logger is `python_logger::writeToTextport()`
(in `source/DSPModules.h`), which resolves `PyGILState_Ensure` / `PyGILState_Release` /
`PySys_WriteStdout` **once per process** into a function-local static `Api` struct —
which is the caching this priority asked for. It also gained two properties the priority did not ask
for: it never *loads* a second Python into the process (it only looks for one TouchDesigner already
loaded, via `GetModuleHandleA`), and it refuses to run unless all three pointers resolved, because
releasing a GIL state that was never acquired is worse than staying silent.

---

## 4. Build Configuration Review

The capture below has been updated to the current state; the original text is quoted under it.

**Current**: `PluginProjects/FFT/CMakeLists.txt` calls
`td_plugin_optimize(FFT AVX2 FAST_MATH LTO)`. The flags themselves live in
`PluginBuilder_V2/cmake/TDPlugin.cmake`, function `td_plugin_optimize`, which for MSVC Release applies:

| Flag | What it does | Present |
|---|---|---|
| `/O2` | maximize speed | ✓ |
| `/Oi` | enable intrinsics | ✓ |
| `/Ot` | favor fast code | ✓ |
| `/Gy` | function-level linking (what makes LTO worthwhile) | ✓ |
| `/arch:AVX2` | generates AVX2 + FMA instructions | ✓ |
| `/fp:fast` | fast floating-point (enables FMA contraction, reassociation) | ✓ |
| `/GL` + `/LTCG` | whole-program optimisation across `FFT.cpp` / `AnalysisPipeline.cpp` / `Parameters.cpp` | ✓ |

> **Correction.** The original text of this section said: *"The CMakeLists.txt at
> `PluginProjects/FFT/CMakeLists.txt` applies: `/O2` / `/Oi` / `/Ot` / `/fp:fast` / `/arch:AVX2`."*
> That was accurate as a list of the speed flags but wrong about the file, and incomplete. The
> `CMakeLists.txt` in this project is a **thin declaration**: it locates PluginBuilder, calls
> `td_add_plugin` and `td_plugin_optimize`, and registers the tests and benches. The optimisation
> flags are set by `td_plugin_optimize` in the *shared* module, so any plugin built with
> PluginBuilder gets the same set. `/Gy` and LTO were added afterwards. The LTO part is not cosmetic:
> the plugin's own comment records that the AVX2 intrinsics in `DSPModules.h` are only worth what the
> inliner does with them, and `/GL` is what lets the inliner work across translation units instead of
> stopping at each one.

**Recommendation (as written)**: Add `/fp:contract` explicitly (though `/fp:fast` enables it by
default) and ensure FFTW is linked with the AVX2-capable library.

**Status of that recommendation:**
- `/fp:contract` — still not written explicitly, and still unnecessary: `/fp:fast` implies it.
  **OPEN but low value.**
- "Ensure FFTW is linked with the AVX2-capable library" — **DONE, and it went further than that.**
  The build vendors FFTW **3.3.11 built from source with SSE2 + AVX + AVX2(FMA) codelets**
  (`PluginProjects/FFT/3rdParty/fftw3/VERSION` records the source URL, the tarball MD5, the built
  DLL's SHA-256, and the one upstream patch — upstream's CMake build still stamps 3.3.10). The build
  is registered by `td_plugin_use_fftw3(FFT VERSION 3.3.11-avx2 DYNAMIC)`. Note **DYNAMIC**: the
  plugin does **not** link the import library at all — `fftwf_*` is resolved at run time, because the
  `FFT Backend` toggle can also select Intel oneMKL's FFTW3 interface, which exports the same symbol
  names. See `source/FftBackend.h`.

---

## 5. Summary: What Makes Sense vs. What Doesn't

### 5.1 Commits that make sense for real-time use

- **FFTW_ESTIMATE as primary planner** (`2daf9f1`): Correct — eliminates all setup stalls.
  *(Since superseded by the `Auto` policy, which gets the measured plan without the stall.)*
- **16-bin 2x unrolled SIMD magnitude** (`ee6169e` — **hash corrected**, the original draft said
  `d60b7e3`): Correct — maximizes post-FFT throughput.
- **Crash guards** (`2daf9f1`): Correct — zero cost when no exceptions.
- **Zero-padding persistence** (`2daf9f1`): Correct — eliminates millions of zero-writes.
- **AVX2 windowing/weighting** (`2daf9f1`): Correct — full SIMD utilization.
- **AVX2 permute workaround** (`2daf9f1`, `reorderLanes`): Correct — restores the 16-bin magnitude
  pipeline after the build break introduced by `ee6169e`. The original draft attributed this to
  `c85c5e0`, which does not touch it.

### 5.2 Commits that were problematic (and how they were corrected)

- **FFTW_EXHAUSTIVE as primary planner** (pre-existing before `492b549`, corrected by `cd94d9d` and
  `2daf9f1`): Caused plan-creation stalls for N=16,384 — corrected to MEASURE → ESTIMATE, and now to
  the `Auto` policy. The console logging in `492b549` itself was not a regression; it only runs
  during infrequent plan creation. *(The "32 s" figure remains unverified — see entry 1.)*
- **Scalar dB conversion replacing SIMD** (`2daf9f1`): The original SIMD dB loop was slower than
  scalar due to the load/store round-trip through `alignas(32)` temp arrays. The scalar replacement
  was actually **faster than the original SIMD version** (which was inefficient).
  - **This conclusion has been overtaken.** The scalar version was itself the bottleneck, and
    Priority 1 removed it: the dB stage is now a vectorized LUT at ~5 µs/channel, so neither the
    original SIMD loop nor the scalar replacement is the right answer. Kept because the *reasoning*
    ("the SIMD version was inefficient, not because SIMD is wrong") is what pointed at the LUT fix.
- **Per-sample EQ branching** (`d60b7e3`): Minor regression when EQ active, corrected by Priority 5.
  Note the same commit is the one that **fixed** the EQ being dead, which had silently made the
  default 6 dB high shelf cost nothing — see v2.2.2 in `CHANGELOG.md`, which measures that fix at
  +16 µs/channel per cook.
- **Planner choice flip-flop**: `FFTW_EXHAUSTIVE` → `PATIENT` (`c85c5e0`) → `MEASURE` (`cd94d9d`) →
  `ESTIMATE` (`2daf9f1`) → selectable (`Auto` default). Each step was defensible at the time; the end
  state, "instant plan now, measured plan in the background, cached in wisdom", is the one that
  removes the tradeoff instead of picking a side of it.

### 5.3 The trajectory makes sense

The project evolved from:

> EXHAUSTIVE (plan-creation stalls) → PATIENT → MEASURE → ESTIMATE (instant)

with progressively more SIMD vectorization (8 → 16 bins), and then made the planner a *policy* rather
than a hard-coded choice. The remaining opportunity identified here — the scalar `log10` in dB
conversion as the biggest non-FFT CPU bottleneck — **was taken**, and the dB stage is no longer a hot
spot.

---

## 6. Implementation Log

### 6.1 Commit `adc8945` — perf(FFT): optimize real-time pipeline

All per-frame priorities implemented. **Verified against the current source**; where the original note
and today's code disagree, the note is kept and the correction follows it.

1. **Priority 1 (DONE)**: `FastLog10` class added with a lookup table using IEEE 754 bit manipulation.
   - **Correction**: the table is **2048 entries (11 bits, 8 KB)**, not the "256-entry" the original
     note says. `FastLog10::kTableBits = 11` and `FastLog10::kTableSize = 1 << kTableBits`, in
     `source/DSPModules.h`. The class comment records that the size is a deliberate
     accuracy/L1 tradeoff and is the only knob.
   - Scalar path replaces `std::log10` in the offset computation and the scalar tail loop — still
     true.
   - AVX2 path (`FastLog10::scaledVec`) processes 8 floats per call via `_mm256_i32gather_ps` — still
     true (same file).
   - **Superseded in part**: the note says "Estimated ~15–20x speedup over `std::log10` for the dB
     conversion stage." That is an **estimate**, unverified here, and it is no longer the headline:
     `DecibelConverter::convertToDB()` is now vectorized 16 bins at a time with an 8-bin tail
     (in `source/DSPModules.h`), which is a change of *shape*, not just of constant.
2. **Priority 2 (DONE)**: `findPeakWithIndex` AVX2 function with movemask-gated reduction.
   (Originally from stash commit `f1cb0d0`.) — Verified: `source/DSPModules.h`.
3. **Priority 3 (DONE)**: `PerceptualWarping::applyWarp` unrolled from 4 to 8 bins per iteration. —
   Verified: `source/DSPModules.h`.
4. **Priority 5 (DONE)**: EQ per-sample branching eliminated. `BiquadSection::process()` returns `x`
   when inactive, so calling unconditionally is safe and removes branch mispredictions. — Verified:
   `source/DSPModules.h`.
5. **Priority 6 (DONE)**: Integrated with Priority 1 — normalized-mode clamping (`mode == 2`) handled
   via AVX2 `_mm256_min_ps` / `_mm256_max_ps` in the same loop. — Verified:
   `source/DSPModules.h`.
6. **Priority 7 (DONE)**: the Python-logging cache.
   - **Correction**: the original note names `writeToPythonConsole`, which **does not exist**. The
     current implementation is `python_logger::writeToTextport()` with the CPython pointers resolved
     once into a function-local static (`python_logger::api()`, `source/DSPModules.h`), and
     log lines are deferred and flushed from the cook thread by `PlanLog::flushToTextport()`. The
     *goal* — no per-log DLL resolution on the hot path — is met.

### 6.2 Priority 4

- **Priority 4 (was TODO)**: FFTW wisdom persistence or "Performance Mode" toggle.
  - **NOW DONE.** Wisdom is persisted at `%LOCALAPPDATA%\TD_Custom_FFT\fftwf_wisdom.txt`
    (imported once per process under the planner mutex, exported after every MEASURE plan), and the
    "Performance Mode" toggle became the four-entry `FFT Planner` menu. See 3.4.
  - **Correction**: the note adds "The adaptive planner (MEASURE for small, ESTIMATE for large) is
    already in place." That is **wrong today and was an odd description then** — there is no
    size-adaptive planner anywhere in the tree. What exists is a policy the *user* selects
    (`PlannerPolicy` in `source/DSPModules.h`: `Auto` / `Fast` / `Measured` / `Patient`)
    plus a background upgrade. If you go looking for a size threshold, you will not find one.

---

## 7. Next Steps

The original list, with the outcome of each item. Only one is still open.

1. ~~Consider **Priority 4**~~ (FFTW wisdom or Perf Mode toggle). — **DONE.** The `FFT Planner` menu
   plus wisdom caching; see 3.4.
2. ~~Explore aligned memory (32-byte) for `std::vector` buffers to enable aligned AVX2 loads/stores
   (`_mm256_load_ps` vs `_mm256_loadu_ps`).~~ — **DONE.** `FFTDSP::AlignedAllocator`
   (in `source/DSPModules.h`) defaults to 32-byte alignment, and the SIMD helpers use
   `_mm256_load_ps` on their inputs and `storeu` on their outputs. The alignment contract is written
   down in the section comment at `source/DSPModules.h`: **unaligned input buffers fault,
   they do not merely run slowly** — that is the thing to remember when adding a kernel.
   - **Correction to the premise**: on MSVC, aligned vs. unaligned loads generate *identical* code
     (`vmovups`), as the roadmap's own "does not need changing" list notes. The alignment work was
     therefore worth doing for **portability and for the explicit contract**, not for MSVC speed.
3. ~~Explore `_mm256_rsqrt_ps` approximation for magnitude `sqrt` (accuracy tradeoff).~~ — **DONE.**
   `computeMagnitudeAVX2_FMA()` uses a `fast_sqrt_ps` lambda: `_mm256_rsqrt_ps` plus one
   Newton-Raphson step, documented as ~23-bit accuracy (2.2e-7 relative error in
   `README.md`) at about half the latency of `_mm256_sqrt_ps` (in `source/DSPModules.h`).
   The scalar tail uses `std::sqrt`, so the two paths agree to float precision but not bit-for-bit —
   which is why the vectorization tests compare with a tolerance.
4. **Track progress in this file or a dedicated task list.** — **Still the right advice.** This file
   is the analysis record; the forward-looking list is
   [`FFT_REALTIME_PERFORMANCE_ROADMAP.md`](FFT_REALTIME_PERFORMANCE_ROADMAP.md), and the per-release
   history is `CHANGELOG.md`.

### 7.1 What is genuinely left, if you want a project

Reading the current source rather than the `e9985c0` snapshot, the honest remaining items are:

- **`applyWarp` is 8-wide, not 2x-unrolled to 16.** `computeMagnitudeAVX2_FMA`, `multiplyInto` and
  `multiplyInPlace` all do two 8-lane blocks per iteration; `applyWarp` and `applyWarpCubic` do one.
  On a gather-limited loop this may buy nothing, but it is the one structural difference that is
  measurable rather than stylistic. **Unmeasured idea** — take a `fft_bench --interp 0/1` pair before
  and after.
- **In-place r2c to drop the 128 KB scratch buffer** and shrink the per-channel cache footprint. This
  is a roadmap item (`FFT_REALTIME_PERFORMANCE_ROADMAP.md` § 1.10), not implemented. **Unmeasured.**
- **Everything else in this document's plan is done.** If you are looking for real-time work, the
  roadmap's Tier 1 items (parameter polling, output-bin sizing, the cache footprint) and the
  now-implemented async pipeline are the better places to look than this file.
