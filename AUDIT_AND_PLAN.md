# Plugin_FFT — Audit and plan (v2.12.1)

Current as of 2026-09-23, v2.12.1.

**Scope.** A read of every file in `PluginProjects/FFT/source/`, plus `CMakeLists.txt`, `plugin.json`,
`tests/dsp_tests.cpp`, `bench/bench.cpp` and `bench/perf_baseline.json`, at HEAD `53ab7ab`. Nothing was built
or run for this audit. Every number below is quoted from a named source, and every finding cites `file:line`.
References are to the tree at HEAD, so they go stale on the first edit: search for the symbol named next to each one.

**Companion documents.** History is in [CHANGELOG.md](CHANGELOG.md). Usage and the parameter reference are in
[README.md](README.md). What to port from EssentiaTD is in [ESSENTIATD_LESSONS.md](ESSENTIATD_LESSONS.md), and
packaging and licensing are in [INSTALLER_PLAN.md](INSTALLER_PLAN.md). Where those documents already cover a
topic, this one links to them instead of repeating them.

**Severity.** **High**: wrong output, or blocks a release. **Medium**: a real-time or robustness risk under
conditions a user can reach. **Low**: an edge case, misleading telemetry or hygiene. **Info**: a note, nothing
to fix.

---

## 1. Summary

The plugin is in good shape. The cook thread does ingest, a wait-free publish and a result copy. The DSP runs
on one analysis worker, and every kernel in the chain was A/B-measured in v2.12. The default pipeline costs
17.6 µs per analysis on the i9-13900H (CHANGELOG v2.12.0). Steady state allocates nothing, and a test pins
that. What is still open sits at the edges of the cook model and in the process-wide FFTW planner.

**Top open items:**
1. **Stale cooks still publish a job (F1), and ballistics run on TouchDesigner's frame time rather than on
   audio time (F2).** Both are verified in the code and documented in
   [ESSENTIATD_LESSONS.md §2.1/§2.2](ESSENTIATD_LESSONS.md).
2. **An unbounded `FFTW_PATIENT` measurement holds the process-wide planner lock (F4).** A re-plan, a Worker
   Priority change, switching Async off, or node teardown can wait on it for seconds, and on the cook thread
   when Async is off.
3. **Three small correctness bugs:**
   - the first plan of a node ignores Async off (F5);
   - the backend cache is unsynchronised across nodes (F6);
   - a Quality Preset ignores `dB Range Floor` when Loudness is Off (F7).
4. **The release blocker is licensing (F15).** The repository has no LICENSE file, and FFTW's GPL text is not
   shipped beside the DLL.
5. **The perf gate cannot catch a regression (F13).** Its baseline was recorded under load, so it passes a
   2–3× slowdown.

---

## 2. Current architecture snapshot

### 2.1 Build and identity

| Item | State | Evidence |
|---|---|---|
| Version | 2.12.1, read from `plugin.json` into `FFT_VERSION_*`, then into the popup `Plugin:` line and the Custom OP version | `CMakeLists.txt` (`string(JSON …)`), `FFT.cpp:96-106` |
| Build | PluginBuilder_V2 `TDPlugin.cmake`, with `td_plugin_optimize(FFT AVX2 FAST_MATH LTO)` (`/O2 /arch:AVX2 /fp:fast /GL`) | `CMakeLists.txt` |
| SDK | CHOP API 10, Common API 3 (`setParameterEnableStates`) | `plugin.json`, `FFT.h:211-213` |
| FFT libraries | FFTW 3.3.11 AVX2 (vendored, 3.1 MB, `DYNAMIC`, loaded at run time). oneMKL (`mkl_rt.3.dll`, installed by the user, sequential threading layer forced). Neither is ever unloaded | `FftBackend.h:532-610, 659-748` |
| CPU guard | AVX2, FMA and OS YMM state checked once per process. Without them the node outputs silence and keeps a sticky error | `DSPModules.h:313-337`, `FFT.cpp:703, 819` |

### 2.2 Threads and handoff

| Thread | Runs | Priority |
|---|---|---|
| TouchDesigner cook | `getOutputInfo` (reads parameters once), then `execute`: ingest (mix, EQ, FIFO), `fillJob` and publish, acquire the result, `memcpy` to the output. Then the info callbacks | TD's |
| Analysis worker (`AsyncAnalysis`) | `AnalysisPipeline::process`: owns the pipeline while Async is on. Async off runs it inline on the cook thread | `THREAD_PRIORITY_HIGHEST`, or MMCSS "Pro Audio"; EcoQoS opt-out (`AsyncAnalysis.cpp:25-55`) |
| Background planner (per measurement) | `FFTW_MEASURE`/`FFTW_PATIENT` under the process-wide `plannerMutex()`, then exports wisdom | `ABOVE_NORMAL` (`DSPModules.h:3134-3168`) |
| `std::execution::par` | The channel loop, only when there is more than one channel (All Channels) | pool |

- **Handoff.** Two `TripleBuffer`s carry the work, one for jobs and one for results. Each side is one atomic
  exchange, and the indices are `alignas(64)` (`DSPModules.h:583-651`).
- **Waking the worker.**
  - Wake = Poll: a 2 ms high-resolution waitable timer.
  - Wake = Signal: one `SetEvent` per cook.
  - After 500 ms with no job the worker goes dormant. A Dekker fence pair makes the dormant/publish race safe
    (`AsyncAnalysis.cpp:80-98, 189-220`).

### 2.3 Pipeline (per channel, `AnalysisPipeline::runChannel`)

1. **Window:** multiply into a persistent zero-padded frame.
2. **FFT:** r2c, out of place, input preserved.
3. **|X|:** AVX2 `rsqrt` plus one Newton-Raphson step, only up to `maxLinearIndex()`.
4. **Full Scale:** DC and Nyquist are halved.
5. **Warp:**
   - an identity grid is a `memcpy`;
   - otherwise load + `vpermps` where the eight taps fit one window, and gather elsewhere;
   - linear or Catmull-Rom interpolation;
   - Peak or RMS aggregation on the coarse bins.
6. **Weighting:** A, C or ITU-R 468, fused with the dB reference peak.
7. **dB:** `FastLog2Seg`, table-free.
8. **Ballistics:** `applyInPlace`.

After the chain, channel 0 also gets the spectral features (from the linear magnitude) and the peak search.

**Caches.** Three keyed caches (`WindowKey`, `WarpKey`, `WeightKey`) mean steady state does not rebuild
anything. A rebuild happens only when the rate, window, pad, planner or backend changes
(`AnalysisPipeline.cpp:518-527`).

**Planner.** The policies are Auto, Fast, Measured and Patient (Patient has no time limit since v2.12.1).
- A size or backend change hands an in-flight measurement to a graveyard instead of joining it.
- The measured plan is swapped in with `try_lock`.
- Wisdom is kept per backend in `%LOCALAPPDATA%\TD_Custom_FFT`.

### 2.4 Surface

- **Parameters:** 47 on 5 pages.
  - Spectrum 18, EQ 9, Window & Weighting 5, Loudness & Ballistics 10 (including the Reset pulse),
    Performance 5.
  - Defaults: Mono Mix, Log, Warp 0.963, window 3175 samples, Zero-Padding on at 16384, Output Bins Mode Auto
    (8193 bins), Kaiser Manual β 15, Peak aggregation, Async on, Wake Poll, Priority Highest, FFTW3, Planner
    Auto, Ingest Auto (`Parameters.h:459-530`).
- **Info CHOP:** 36 channels (`FFT.cpp:879-959`).
- **Info DAT:** 23 fixed rows plus at most the 32 newest plan-log rows. They are rendered at most every 250 ms,
  or when the status or the log changes (`FFT.h:498-499`, `FFT.cpp:964-1161`).
- **Popup:** at most 1200 characters, with three tail lines clipped to 72 characters.
- **Tests:** `fft_tests` has 29 test functions. It ran 748 checks, 0 failures at `de8b13a` (CHANGELOG v2.12.0);
  the v2.12.1 entry records no count. It includes:
  - an allocation gate that counts `operator new`;
  - hermetic wisdom (`dsp_tests.cpp:2268`);
  - the real `AnalysisPipeline` and `AsyncAnalysis`.
- **Bench:** `fft_bench` has a per-stage table, `--cook` (the real handoff), `--info` and
  `--gate` (`ctest -L perf`, 6 metrics).

### 2.5 Current measured numbers (i9-13900H, FFTW3 backend unless stated)

| Quantity | Value | Source |
|---|---|---|
| Pipeline, plugin defaults (N 16384, 8193 bins) | **17.60 µs** median | CHANGELOG v2.12.0 A/B |
| Pipeline, full chain (dB + ballistics + features) | 22.20 µs | CHANGELOG v2.12.0 |
| Pipeline, Visual 60 preset | 8.90 µs | CHANGELOG v2.12.0 |
| Cook, synchronous (Async off) | 25.20 µs | CHANGELOG v2.12.0 |
| Raw RFFT Bins pipeline vs default | 11.8 vs 18.7 µs p50 (v2.11 build) | CHANGELOG v2.11.0 |
| Warp stage, 16384 bins, Log: linear / cubic | 2.52 / 4.82 µs | CHANGELOG v2.12.0 |
| dB normalised / peak + index, 16384 bins | 3.58 / 0.94 µs | CHANGELOG v2.12.0 |
| Spectral features, 8193 bins | ~3.5 µs | CHANGELOG v2.12.0 |
| FFT only, FFTW MEASURE vs oneMKL, N 8K / 16K / 32K | 4.71 / 11.55 / 23.65 vs 3.87 / 8.41 / 17.73 µs | CHANGELOG v2.12.0 |
| FFTW PATIENT plan, N 16K | 9.70 µs, planning ~2.7 s at N 32768 | CHANGELOG v2.12.0, v2.12.1 |
| oneMKL first-use initialisation | ~39 ms once per process; 5 DLLs (~177 MB) load | CHANGELOG v2.12.0 |
| Cook, Async on (Wake Poll), plugin defaults | **0.60** (p50) | interleaved A/B gate run on an idle CPU, 3 rounds (README Performance). The committed `bench/perf_baseline.json` value (9.6 µs) was recorded under load, see F13 |

---

## 3. Findings

### F1 — Medium — A stale cook re-analyses the same window
- **What happens:**
  - `ingest()` returns early when the input delivered nothing new (`fresh == 0`, `FFT.cpp:521`).
  - `executeImpl` still calls `fillJob` and `publish()` on every cook (`FFT.cpp:786-787`), so the worker runs the
    full pipeline (~17.6 µs) on an identical window.
- **Effects:**
  - Flux reads 0 on those frames. The mechanism is pinned by `dsp_tests.cpp:1768`.
  - When the source cooks slower than the node, flux alternates between a value and 0.
  - Ballistics and AGC advance by a frame that carries no audio.
- **Fix:**
  - Publish only when `fresh > 0`, when the parameters changed, or on reset. Otherwise a static input could never
    deliver a parameter change.
  - Accumulate the skipped time into the next job (F2).
  - `hold_frames` counts jobs, so it stays correct.

### F2 — Medium — Ballistics and AGC are clocked by `deltaMS`, not by audio time
- **What happens:**
  - `dt_ms` comes from `timeInfo->deltaMS` (`FFT.cpp:752-758`).
  - It feeds `coefFromMs` for attack, release and AGC decay (`AnalysisPipeline.cpp:575-582`).
  - When frames drop, TouchDesigner lengthens the timeslice, so the per-frame coefficient no longer matches the
    audio that arrived. EssentiaTD measured this failure mode ([ESSENTIATD_LESSONS.md §2.2](ESSENTIATD_LESSONS.md)).
- **Fix:** `dtMs = freshSamples / sr · 1000`, summed over any held cooks, with `deltaMS` only as the fallback
  when no audio arrived.

### F3 — Low — `timeInfo->rate` is not sanitised
- `ti->rate > 0.0` accepts `inf` and 1e9 (`FFT.cpp:398, 755-756`).
- The rate then drives `info->sampleRate = bins × rate` (`FFT.cpp:414`), which can become infinite or absurd.
  EssentiaTD saw a bad declared rate make a downstream Trail CHOP attempt huge allocations.
- The `1000 / rate` fallback for `dt` can also become 0.
- **Fix:** one helper that accepts a finite value in [1, 1000] and returns 60 otherwise, used at both reads.

### F4 — Medium — An unbounded PATIENT measurement can make other threads wait
The background measurement holds `plannerMutex()` for its whole duration (`DSPModules.h:3156`), and nothing
bounds that duration since v2.12.1. Four paths take the lock *blocking*:
- `prepare()` (`DSPModules.h:2941`);
- `destroyPlan()` (`:2830`);
- `importWisdomOnce()` (`:2790`);
- teardown (`:2692-2696`).

| Trigger | Thread that waits | Evidence |
|---|---|---|
| Size or backend change on any node while a measurement runs (including this node's own abandoned one) | Worker (Async on): the spectrum holds. Cook thread (Async off): TouchDesigner freezes | `destroyPlan`/`prepare` lock |
| Switching Async off, or changing Worker Priority, while the worker is blocked in `prepare()` | Cook thread, in `stopWorker()` → `join()` | `AsyncAnalysis.cpp:73-77, 148-155` |
| Deleting the node or quitting | TouchDesigner's thread: `~AsyncAnalysis` joins the worker, then `~FFTWEngine` calls `reapGraveyard(true)` | `FFT.cpp:231-234`, `DSPModules.h:2689-2699` |

- Nothing in the Info surfaces says "waiting for the planner".
- When `hold_frames > 3`, the warning blames the load instead: "reduce Zero-Pad Len or Output Bins"
  (`FFT.cpp:1181-1183`).
- **Fix:**
  - `try_lock` in `prepare()` and `destroyPlan()`: keep executing the old plan, report "planner busy", and retry
    on the next job.
  - Send the old plan's destruction to the graveyard as well.
  - Surface the wait in the Info DAT and in the warning text.
  - Teardown has to wait: the planner thread runs code in `FFT.dll`. Document that, and report it.

### F5 — Medium — The first plan ignores Async off
- `process()` calls `rebuild()` → `prepare()` *before* `setBackgroundAllowed(p.async)`
  (`AnalysisPipeline.cpp:527` vs `:533`).
- `FFTWEngine::m_bg_allowed` starts `true` (`DSPModules.h:3273`).
- So a node created, or loaded from a `.toe`, with Async off starts a background MEASURE/PATIENT thread on its
  first job. That contradicts the "Async off = one thread for the whole node" contract, and the CHANGELOG v2.12.1
  statement that no measurement starts while Async is off.
- `test_async_single_thread` sets the flag before `prepare()` (`dsp_tests.cpp:738-739`), so it never exercises
  the pipeline's order.
- **Fix:** hand `p.async` to the engine before `prepare()`. Either pass it through `rebuild()`, or call
  `setBackgroundAllowed` right after the engine is created.

### F6 — Medium — The backend cache is not thread-safe across nodes
- `cachedBackend()` reads and writes a process-wide array, including `std::string` members, with no lock
  (`FftBackend.h:731-748`).
- `prepare()` calls it outside `plannerMutex()` (`DSPModules.h:2896-2918`), on each node's own worker.
- Two FFT nodes planning for the first time in the same frame, which is the normal case when a project loads,
  can race on one slot. The result is a torn `FftBackend` copy.
- **Fix:** a static mutex in `cachedBackend()`, or do the selection under `plannerMutex()`.

### F7 — Low — A Quality Preset ignores `dB Range Floor` when Loudness is Off
- `eval()` reads `Dbrange` only if `loudness != Off || (Kaiser && betaMode == Auto)`, and evaluates that
  *before* `applyPreset()` switches `betaMode` to Auto (`Parameters.cpp:628` vs `:660`).
- `setEnableStates` leaves the parameter enabled in that case (`Parameters.cpp:708`).
- Result: with any preset and Loudness Off, Auto β is always designed for 80 dB, and the enabled field does
  nothing.
- **Fix:** apply the preset's mode overrides before the gated reads.

### F8 — Low — Warnings and resolution reporting
- **`getWarningString` has one source** (`FFT.cpp:1179-1184`). A second warning would overwrite the first.
- **No coverage warning.** When the timeslice is longer than the window, samples go unanalysed silently
  ([ESSENTIATD_LESSONS.md §2.3](ESSENTIATD_LESSONS.md)).
- **The Info DAT's `window_resolution` row understates the resolution.** It is `sr / window`, the Rayleigh
  spacing (`FFT.cpp:1063`), not the window's main-lobe width. `mainLobeHalfWidthBins()` exists
  (`RateModel.h:162`), but only the tests call it.
- **Fix:** warning slots, a coverage warning, and `true_resolution_hz` / `analyzed_fraction` rows (Plan P7).

### F9 — Low — An input that re-cooks without advancing is appended whole
- `IngestCursor::fresh()` returns the whole block when the range did not move but `totalCooks` changed
  (`RateModel.h:228-232`), and the tests pin this as intended (`dsp_tests.cpp:1733`).
- For a non-timesliced input shorter than the window that re-cooks every frame (a Pattern or Lookup CHOP, say),
  the FIFO fills with the same block repeated.
- **Decision needed:** keep and document it, or treat a non-advancing input as "replace the window" (zero-fill
  the rest).

### F10 — Low — Parameter combinations with no validation
- **Log Floor ≥ Display Max is reachable with the sliders alone** (Log Floor 1..500, Display Max from 100). The
  log grid then runs backwards and clamps to `fmax`, so the axis collapses to one frequency
  (`DSPModules.h:1296-1297, 1341`).
- **A typed EQ gain in the thousands of dB overflows the coefficients to `inf`.** `designShelf` never clears
  `z1`/`z2` on a redesign, so the NaN state survives until Reset (`DSPModules.h:975-996`).
- **Fix:** clamp `logFloor < fmax`, clamp the gain to ±60 dB, and reset the biquad state when a coefficient is
  non-finite.

### F11 — Low — `pulsePressed` acts directly on cook-thread state
- It resets the EQ, the ingest cursor and the error text in place (`FFT.cpp:1228-1232`).
- The header says it "only sets a flag" (`FFT.h:206-209`).
- It is safe only while TouchDesigner delivers pulses on the cook thread. **Fix:** make Reset a flag that the
  next cook acts on.

### F12 — Low — Test gaps
- **No harness drives `FFT::execute()`.** Everything in `FFT.cpp` is untested: ingest, `fillJob`, stale cooks,
  the `dt` source, the error-latch lifecycle, and whether the declared width matches the published width.
- **No frame-rate invariance test** for ballistics.
- **Timing assertions sit in the correctness suite** and can fail on a loaded machine:
  - `p95 < 6 ms` (`dsp_tests.cpp:1156`);
  - `worstWake < 10 ms` (`:1895`);
  - Signal median `< 1.5 ms` (`:1913`);
  - `prepare() < 250 ms` (`:558, 580, 1941`).
- `test_planner_graveyard` removes a wisdom file it never uses (`:1930`).

### F13 — Medium — The perf gate cannot catch a regression
- The limit is `baseline × 1.30 + 2 µs` (`bench.cpp:451`), and the baseline was recorded under load
  (CHANGELOG v2.12.0 Tests).
- The ratios below are indicative: the gate scores the minimum of 5 medians, while the reference column comes
  from interleaved A/B medians.

| Metric | Baseline | Gate limit | Current reference | Slowdown the gate lets through |
|---|---|---|---|---|
| `pipeline_default_p50_us` | 24.9 | 34.4 | 17.60 | 1.95× |
| `pipeline_fullchain_p50_us` | 56.3 | 75.2 | 22.20 | 3.4× |
| `pipeline_visual60_p50_us` | 13.7 | 19.8 | 8.90 | 2.2× |
| `cook_sync_p50_us` | 43.0 | 57.9 | 25.20 | 2.3× |
| `pipeline_rawbins_p50_us` | 19.2 | 27.0 | 11.8 (v2.11) | 2.3× |

### F14 — Low — The bench measures some things that no longer exist
- **`--info` models a retired info chain:** 21 CHOP channels, 276 DAT rows, and a popup rebuilt on every cook
  (`bench.cpp:152-154`). The real `renderInfoCache()` (`FFT.cpp:1019-1161`) is never measured.
- **The bench defaults to Fixed 16384 bins**, not the plugin's Auto 8193 (`bench.cpp:86-87`), although its
  comment says the defaults are the plugin's.

### F15 — High (release blocker) — Licensing
- The repository has no LICENSE file.
- FFTW's GPL `COPYING` is shipped neither in `3rdParty/fftw3/bin` nor in `__Plugins__/FFT`.
- `RateModel.h:1-3` points to "the project's own terms", and none exist.
- The obligations and file list are in [INSTALLER_PLAN.md §6](INSTALLER_PLAN.md).

### F16 — Low — Dead code
Nothing in the plugin calls any of these:
- `PerceptualWarping::targetHzAt` and `topSlopeHzPerFrac` (`DSPModules.h:1345-1375`); only the tests reach them;
- `isAligned32` (`:199`), `PI_F` (`:193`), `TripleBuffer::hasNew` (`:627`);
- `PlanLog::entry`, `size` and `clear` (`:545-559`);
- `resetBackendCache` (`FftBackend.h:715`);
- `Status::abandonedMeasurements`, which is filled (`AnalysisPipeline.cpp:125`) and never displayed.

### F17 — Low — Comments that contradict the code

| Location | Says | Code |
|---|---|---|
| `FFT.cpp:91-100` | "current release … v2.9.1"; fallback literals 2.10.0 | 2.12.1 from `plugin.json` |
| `FFT.h:55-58`, `FFT.cpp:258-259` | cook cost "11 us mean / 17 us p99 (v2.4.0)" | not re-measured at v2.12 (§2.5) |
| `FFT.h:380-382`, `FFT.cpp:849-850` | "21 Info CHOP channels, 276 Info DAT rows" | 36 channels; 23 + ≤ 32 rows |
| `FFT.cpp:328`, `RateModel.h:271, 307` | defaults "16384 bins → 983 040" | Auto 8193 bins → 491 580 at 60 fps |
| `RateModel.h:114-138` | `outputBinCountFrom` "passed straight through from Output Bins"; "Auto bin count" | Auto/Raw = `rfftBinCount` |
| `RateModel.h:32`, `:153` | include "for `topSlopeHzPerFrac`"; β 15 is "the pre-2.10 default" | unused; β 15 is the current default |
| `Parameters.h:171-177, 475`, `Parameters.cpp:218-221` | "nothing asserts" Planner == PlannerPolicy | `static_assert`, `AnalysisPipeline.cpp:68-73` |
| `Parameters.cpp:512-513` | "20 getPar* calls at the shipped defaults" | 30, counted from `eval()` |
| `Parameters.h:521`, `Parameters.cpp:469-470` | features "~2-4 us" | ~3.5 µs (v2.12) |
| `DSPModules.h:14-27, 52-53` | dB = "single-gather 2048-entry LUT", warp = "AVX2 gather" | dB = `FastLog2Seg`; warp = load+permute/gather |
| `DSPModules.h:631, 676-679` | "see `FFT::startWorker`", "FFT.cpp's worker loop / myWorkerDormant" | `AsyncAnalysis` |
| `DSPModules.h:2515-2526, 2672-2679` | prepare/poll run on "the cooking thread" | the pipeline owner (the worker when Async is on) |
| `DSPModules.h:2351, 2362` | ballistics clamp "0.99" | `coefFromMs`/`apply` clamp 0.999 (`eval` clamps 0.99) |
| `DSPModules.h:932-933` | `BiquadSection::reset` runs when EQ is enabled or the rate changes | only Reset and a deactivated shelf |
| `AnalysisPipeline.cpp:500`, `.h:164` | "`FFT::runJob` in FFT.cpp" | `AsyncAnalysis::runJob` |
| `AnalysisPipeline.cpp:612-618` | index vector "built here … could hoist it into a member" | already a member (next sentence) |
| `FftBackend.h:537-540` | MKL finds `mkl_intel_thread`/`mkl_tbb_thread` beside itself | sequential layer forced; those DLLs are not deployed |
| `dsp_tests.cpp:40, 48`, `CMakeLists.txt` | "607 checks" | 748 at v2.12.0 |
| `bench.cpp:11-13, 22-28, 94-95` | includes only `DSPModules.h`; the worker is "simulated"; the defaults are "the README's N 32768" | links `AsyncAnalysis`; defaults N 16384 |

### F18 — Low — DLL search falls back to PATH
- If a backend DLL is not found in the plugin folder, `LoadLibraryExA(dllName)` searches `PATH`
  (`FftBackend.h:542-543`).
- A foreign `libfftw3f-3.dll` or `mkl_rt.dll` on `PATH` then gets loaded. A version mismatch only produces a
  warning.
- **Fix:** load only from the plugin folder, or log the `PATH` hit at error level.

### F19 — Info
- **The channel-0 peak search runs on every job**, whether or not anything reads it
  (`AnalysisPipeline.cpp:663-674`). It costs ~0.5 µs at 8193 bins.
- **`renderInfoCache` still allocates, at ≤ 4 Hz** (`std::string` concatenation for the backend and engine lines,
  and `clipLine`).
- **While the pipeline is failing, `getErrorString` builds one `std::string` per cook** (`FFT.cpp:1199`).
- All three are bounded and acceptable as they are.

---

## 4. What is solid (keep)

- **The cook-thread discipline.** Steady state has no lock, no kernel call and no allocation. Parameters are read
  once per cook, in `getOutputInfo`. FTZ/DAZ is set on both threads, and wait-free triple buffers run in both
  directions.
- **One `AsyncAnalysis`, TD-free, driven by the plugin, the tests and the bench alike.** Wake policy, dormancy
  handshake, MMCSS and pickup telemetry (p50, p99, late) are all in it.
- **Keyed table caches.** The warp version feeds the weighting key, and `interp` and `agg` are in `WarpKey` for a
  documented reason (`AnalysisPipeline.h:201-210`).
- **The planner design.** Wisdom lets the first frame plan instantly, measurement happens in the background, the
  graveyard never joins on a size change, the swap uses `try_lock`, and wisdom is kept per backend.
- **The runtime backend table.** No link-time FFTW, a fallback to FFTW3, no `FreeLibrary`, and `static_assert`s
  that pin the menu to the registry.
- **The v2.12 kernels.** Each one is checked against a scalar reference (`test_v212_simd_kernels`) and was
  measured before it was kept.
- **Honest telemetry.** Errors are live conditions, not latches (except the CPU guard). The popup's length is
  bounded, and `info_callback_calls` records how often each callback was entered.
- **The allocation gate** covers 1 and 4 channels, with features on and off (`test_allocation_gate`).

---

## 5. Measured facts not to re-optimise

Each of these is in CHANGELOG v2.12.0 unless another source is named.

| Fact | Numbers |
|---|---|
| oneMKL runs the FFT faster than FFTW | 18–27 % (8K / 16K / 32K). An FFTW PATIENT plan narrows the gap to ~13 %. FFTW stays the default because it is small, vendored and wisdom-cached |
| Preserve-input, out-of-place r2c is FFTW's fastest mode | `DESTROY_INPUT` + re-zeroing: 1–5 % slower. In-place + re-zeroing: 10–20 % slower |
| PATIENT executes faster than MEASURE, but stays opt-in | 10–15 % at 16K/32K (9.70 vs 10.97 µs at 16K). Every new pipeline, and the test suite, would pay the planning, and teardown waits for it |
| FFTW threads do not help a single 1-D transform | 13–51 % slower. `fftwf_plan_with_nthreads` is process-global sticky state and crashed TouchDesigner (`Parameters.cpp:478-482`) |
| `rsqrt` + Newton-Raphson beats hardware `vsqrtps` | 820 vs 1185 ns at 8193 bins (45 %) |
| dB log: `FastLog2Seg` wins in the dB stage, the gather table wins in the features loop | dB normalised 15 % faster with `FastLog2Seg`. Features 4 % slower with it. Polynomial log2 is 30 % slower than the table |
| One fused dB + ballistics + peak kernel | 15 % slower (~23 live vectors spill onto AVX2's 16 registers); 0–4 % without the peak. Not kept |
| Unrolling the features loop ×2 | 12 % slower (spills) |
| Four accumulators for every reduction | `peakMagnitude` 2× (0.41 → 0.21 µs at 8193 bins). Weighting + reference max in one pass: 1.0 → 0.57 µs |
| `cmp(s, d)` vs `cmp(diff, 0)` in ballistics | no change |
| Aligned and unaligned loads | MSVC emits `vmovups` for both. The alignment is a contract (an aligned load on an unaligned buffer faults), not a speed-up (`DSPModules.h:29-35`) |
| Wake = Poll is the default | A `SetEvent` costs 4–5 µs median and 16–18 µs p99 on the signalling thread (`DSPModules.h:660-664`) |
| Update-rate divider and parameter polling every N cooks | Removed in v2.7.0: they saved nothing on the cook thread and added latency (`Parameters.cpp:472-477`) |
| `cookEveryFrame = true` | Load-bearing. Without it an unpulled node never reaches the info or error callbacks (`FFT.cpp:239-263`) |
| The silence short-circuit only applies to linear magnitude with ballistics off | A dB mode must publish its floor, and ballistics must keep decaying (`AnalysisPipeline.cpp:376`) |
| "Magnitude only up to the warp's max bin" saves nothing at the defaults | Display Max 24000 is above the 22.05 kHz Nyquist at 44.1 kHz (`AnalysisPipeline.cpp:307`) |
| Output count semantics belong to the user | Auto = N/2+1 of the FFT that was run; Fixed = exactly Output Bins (CHANGELOG v2.11.0). Never change them for performance |

---

## 6. Plan (open work only, in order)

Effort: **S** ≤ ½ day, **M** ≤ 2 days, **L** > 2 days.

| # | Item | Fixes | Effort | Accept when |
|---|---|---|---|---|
| P1 | **Hold on a stale cook.** Publish only when `fresh > 0`, the parameters changed, or on reset; accumulate the skipped audio time | F1 | S | Headless test: with the input cooking every 2nd frame, jobs published = input cooks, flux never reads 0 on a held frame, and the ballistics state is unchanged on held frames |
| P2 | **Audio-time `dt`:** `freshSamples / sr` (summed over held cooks), `deltaMS` as the fallback, for attack, release and AGC | F2 | S | The same attack/release trajectory at 735, 882 and 1470-sample timeslices, within 1 % |
| P3 | **Sanitise `timeInfo->rate`** (finite, 1..1000, otherwise 60) in one helper used by `getOutputInfo` and `executeImpl` | F3 | S | Unit test: NaN, inf, 0 and 1e9 all give 60; `info->sampleRate` is finite |
| P4 | **Async off applies to the first plan:** set the engine's background flag before `prepare()` | F5 | S | Pipeline-level test: `process()` with `async = false` and Auto/Patient at an N not in wisdom leaves `upgradeInProgress() == false` after the first job |
| P5 | **Lock `cachedBackend()`** | F6 | S | 8 threads × 1000 rounds of first-time selection on a reset cache: every thread gets the same `module`, no crash |
| P6 | **Preset before the gated reads in `eval()`** | F7 | S | Visual 60 + Loudness Off + dB Range 60: `kaiser_beta` = `kaiserBetaForSidelobeDb(60)` ≈ 8.16, not the 80 dB value |
| P7 | **Warning slots + coverage warning + `true_resolution_hz` and `analyzed_fraction` rows**; the `hold_frames > 3` text names the planner wait when that is the cause | F8, F4 | S | Two warnings coexist; the coverage warning fires at 1600 samples per cook with a 1024 window and not at 800; β 15 / 3175 samples / 44.1 kHz reports ≈ 67.8 Hz |
| P8 | **Planner-wait containment:** `try_lock` in `prepare()` and `destroyPlan()` (keep the old plan and retry on the next job), destruction through the graveyard, a "planner busy" status row. Document the teardown wait | F4 | M | With a PATIENT measurement at N 65536 running on node A: node B's size change with Async off has cook max < 1 ms; switching A's Async off has cook max < 1 ms; the Info DAT shows the wait |
| P9 | **Headless `execute()` harness** (the TDStubs pattern, [ESSENTIATD_LESSONS.md §5](ESSENTIATD_LESSONS.md)) | F12 | M | P1–P3, P6 and P7 are tested through the shipped `getOutputInfo`/`execute`, plus the error-latch lifecycle and declared width == published width |
| P10 | **Test and gate hygiene:** move the timing checks to the `perf` label; bench defaults = plugin defaults (Auto bins); rebuild `--info` on the real render code (TD-free) or drop it; regenerate the baseline on an idle machine; tighten the limit to 1.15× + 1 µs | F12, F13, F14 | S | 20 consecutive `fft_tests` runs with 0 failures during a parallel build; a deliberate +20 % in the default pipeline fails `ctest -L perf` |
| P11 | **Licensing:** a LICENSE (GPL-compatible, because of FFTW), FFTW `COPYING` beside the DLL in `3rdParty` and in the deploy, fix the `RateModel.h` header | F15 | S | The files are present in the repository and in `__Plugins__/FFT`; the [INSTALLER_PLAN.md §6](INSTALLER_PLAN.md) checklist is complete |
| P12 | **Comment and dead-code sweep**, plus the small hardening items | F16, F17, F18, F10, F11 | S | A comment-only diff proven by the strip-comments projection; check count unchanged; `grep -E "607 checks|983 ?040|21 Info CHOP|FFT::runJob|nothing asserts"` returns nothing |
| P13 | **Does TouchDesigner load every DLL in the Plugins tree at start-up?** (14 oneMKL DLLs, ~456 MB) | — | S | TD cold start measured with and without the oneMKL set (3 runs each) and the loaded-module list checked; if they load at start-up, oneMKL moves to a subfolder and is loaded by full path |
| P14 | **Installer** ([INSTALLER_PLAN.md §8](INSTALLER_PLAN.md)) | — | M | Windows Sandbox: install → the node loads → uninstall leaves nothing behind |
| P15 | **HFC, mel band energies (`mel{i}_{lo}_{hi}`), MFCC-13.** HFC is the Σ pw·k that `featureSums` already accumulates | — | M | Each within 1e-3 relative of an independent reference; allocation gate green |
| P16 | **SuperFlux novelty** on a short sub-window, plus an optional time-based adaptive onset pulse ([ESSENTIATD_LESSONS.md §1.3](ESSENTIATD_LESSONS.md)) | — | M | Onset F1 on a click track ≥ plain flux; no onset on a ±50-cent vibrato tone |
| P17 | **dB-domain interpolation option**, with 10·log10 \|X\|² skipping the `sqrt` when dB is on. Default unchanged | — | S | dB-mode pipeline ≥ 1 µs faster at 16384 bins (interleaved A/B median); log-axis screenshot A/B |
| P18 | **Dormancy stress test** (`perf` label) | — | S | 10⁵ hot publishes plus 200 crossings of the 500 ms threshold: 0 lost jobs, worst dormant wake < 10 ms |
| P19 | **Batched `fftwf_plan_many_dft_r2c` for All Channels** (single-threaded, built under the planner mutex). A probe measured 86.3 → 29.9 µs per transform at `howmany` = 4 (CHANGELOG v2.7.0) | — | M | 4-channel All Channels ≥ 20 % faster than the `std::execution::par` fan-out, with bit-identical spectra |
| P20 | **Multi-resolution analysis:** 2–3 FFT sizes, each owning an octave range and stitched on the output grid, or a sparse-kernel CQT | — | L | Two tones 2 Hz apart at 60 Hz are resolved; a 5 ms click's onset latency ≤ that of the short window; worker cost ≤ 1.5× the default pipeline |
| P21 | **Phase / complex output (optional)** | — | M | The phase of a known sine is within 1e-3 rad of the analytic value; zero cost when off |
| P22 | **GPU / TOP output:** a companion TOP with a 1×N texture or a spectrogram ring, from the same `AnalysisPipeline` | — | L | The CHOP→TOP hop is gone from a reference network; the ring write allocates nothing per frame; TOP cook ≤ CHOP cook |

**Parked, and why:**
- **Peak search on demand** (F19): ~0.5 µs to gain.
- **HPCP chroma, K-weighted LUFS, `wav_reader` in the bench, batch mode**: [ESSENTIATD_LESSONS.md §8](ESSENTIATD_LESSONS.md), "later".
- **GPU FFT for the transform itself**: upload and sync would dominate below ~16 channels (estimated, not measured).
- **pffft**: no case for a third backend unless removing the GPL dependency becomes a goal.

---

## 7. Measurement protocol

1. **Correctness.** `fft_tests`: state the check count and explain any change in it. A comment-only change is
   proven by stripping the comments from HEAD and from the tree and diffing the two.
2. **Interleaved A/B only.** Build both binaries, run them alternately in random order, and kill leftover
   processes between rounds. Back-to-back runs of the same binary favour whichever runs second, which has
   inverted a backend comparison before (README, "Which FFT library is faster").
3. **Pin and name everything:**
   - one P-core at high priority;
   - `--backend fftw3|mkl`, by name: an unknown value falls back to FFTW3 with only a printed note;
   - `--planner fast|measured|patient` spelled out, because an unknown value silently becomes Auto
     (`bench.cpp:496-508`);
   - the private wisdom file, which is the default.
4. **Measure both surfaces.** Report the worker (per-stage table, `dsp_time_us`) and the cook (`--cook`,
   `cook_time_us`) together, because a change can move work from one to the other.
5. **Gate.** Run `ctest -L perf` against `bench/perf_baseline.json`. Regenerate the baseline only on an idle
   machine: `fft_bench --gate bench/perf_baseline.json --update 1`.
6. **In TouchDesigner.**
   - Read `cook_time_us`, `dsp_time_us`, `hold_frames` and `pickup_p99_us` from the Info CHOP, and the Performance
     Monitor for the downstream network.
   - Before trusting a TouchDesigner reading, confirm `__Plugins__/FFT/FFT.dll` is byte-identical to the build.
   - Name the machine next to every number.
7. **Silence check.** Feed digital silence and watch for cook-time spikes. A spike means some code runs outside
   `DenormalGuard`. Fix that coverage; do not add a second FTZ.
