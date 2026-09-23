# Plugin_FFT — Audit, Current State & Progression Plan (2026-09-23)

> **Read this first.** This audit is dated 2026-09-23 and describes the **pre-v2.10 state** (v2.9.1);
> §0 and §1 are kept as that record, not updated. For what has shipped since, see the Status addendum
> at the end of §4 and `CHANGELOG.md` v2.10.0–v2.12.0. The two retired performance docs
> (`FFT_REALTIME_OPTIMIZATION_ANALYSIS.md`, `FFT_REALTIME_PERFORMANCE_ROADMAP.md`) are gone; what
> they said that is still valid is in **Appendix B**, and their full text is in git history.

> **What this is.** A full-read review of `Plugin_FFT` (v2.9.1, HEAD `71c13b8` + an uncommitted
> comment-only working tree), ending in a phased plan whose centre of gravity is **real-time cost and
> real-time quality**. No code was changed.
>
> **How it was produced.** Every file in `PluginProjects/FFT/source/` was read in full. The tests, the
> bench, the probes, the NumPy reference and the pending diff were read by a second reviewer, and its
> claims were spot-checked. Numbers were measured today on the **i9-13900H** (AC power, *High
> performance* plan, TouchDesigner closed) with the **existing** `build/bin/Release` binaries. Nothing
> was rebuilt, so nothing was deployed into `__Plugins__/`. A throwaway timer probe was compiled in the
> session scratchpad, outside the repo.
>
> **Conventions.** `file:line` refers to the working tree as of today. Each finding is labelled
> **measured** (a number from a run today), **verified** (read in the code or reproduced), or
> **inferred** (a reasoned consequence that was not run). Severity: 🔴 wrong behaviour · 🟠 real-time
> or quality cost · 🟡 hygiene / latent · ⚪ note.

---

## 0. TL;DR

The node is in good shape. The async architecture is sound, and the cook thread costs about **12 µs**
at the defaults (measured). The remaining work falls into three areas, in this order:

1. **Truth and hygiene: small fixes to things that are wrong today.**
   - The "607 checks, 0 failures" baseline is flaky: 2 of 5 runs failed today.
   - The tests import the user's real wisdom file.
   - The no-AVX2 error message erases itself on the first cook.
   - The Info DAT does a filesystem syscall on every cook.
   - Versions and defaults have drifted between `plugin.json`, the README and the code.
2. **Output-side real-time cost, which is now bigger than the FFT.**
   - At the defaults, the node emits 16 384 bins, and **62 %** of them sit less than a quarter of an
     FFT bin apart. They are interpolation of points that already exist.
   - The output copy is 7.6 µs of the 12 µs cook, and every downstream TouchDesigner op pays for all
     16 384 samples.
   - Cutting bins is the biggest lever in the whole project. It is only *correct* once the warp
     aggregates, because it currently point-samples: at 2 048 bins, half the axis skips FFT bins,
     up to 27× decimation.
3. **DSP quality at equal or lower cost.**
   - The default Kaiser β = 15 gives a ~135 Hz-wide main lobe, far more sidelobe rejection than the
     80 dB display range needs, and a blurred bass end.
   - A β tied to the dB range, a band-aggregating warp and a "quality preset" would make the node
     both cheaper and sharper.

Everything else (multi-resolution/CQT, GPU output, phase and features) is Phase 3+.

---

## 1. Current state snapshot

| Item | State | Evidence |
|---|---|---|
| Version | **2.9.1** in code (`FFT.cpp:96-103`), **2.9.0** in `PluginProjects/FFT/plugin.json` | verified: drift |
| Git | `main` = `origin/main` at `71c13b8`. Working tree: 13 modified files, all comment/doc; the `.lib` differs only by timestamps; the `.toe` grew 86 → 120 KB | verified (diff + comment-stripped scan) |
| Build | vcvars64 + CMake + Ninja via `../PluginBuilder_V2/cmake/TDPlugin.cmake`; `/O2 /arch:AVX2 /fp:fast /GL /LTCG` | `CMakeLists.txt:48-53` |
| SDK | Compiled against API 10 / **Common 2** headers from TD 33070; the machine now runs **TD 2025.33230 (Common 3)**. It still loads, but `setParameterEnableStates` is unavailable until PluginBuilder_V2 resyncs | V2 plan §2.3 |
| Tests | 607 checks. **Today: 3 × `0 failures`, 2 × `1 failure`** (`dsp_tests.cpp:1107`, timer bound) | measured, 5 runs |
| FFT library | Vendored FFTW 3.3.11 AVX2 (runtime `LoadLibrary`), optional oneMKL (521 MB, user-installed) | `FftBackend.h` |
| Architecture | Cook = ingest + publish + copy (wait-free triple buffers); worker = window → FFT → \|X\| → warp → weighting → dB → ballistics → peak | `FFT.h:33-75` |
| Defaults | Mono Mix, Log, blend 0.963, window **3175** samples, pad **16384**, **16384 bins**, Kaiser **β 15**, linear warp, Async on | `Parameters.h:413-461` |

### 1.1 Measured cost today (i9-13900H, existing Release binaries)

| Configuration (1 channel, Log, 44.1 kHz) | Worker DSP / job | Cook thread (Async on) |
|---|---|---|
| **Defaults**: N 16384, 16384 bins, linear warp | **35.6 µs** (fft+mag 22.5 · warp 9.4 · **peak 2.8**) | **12.1 µs mean / 18.6 p99** (copy 7.6) |
| N 16384, **2048 bins** | 23.9 µs (warp 1.2, peak 0.4) | **5.6 µs mean / 10.4 p99** (copy 1.1) |
| **N 8192, 2048 bins, cubic** | **10.4 µs** | ≈ 5.6 µs (the copy is sized by bins) |
| N 4096, 2048 bins, cubic | 5.3 µs | ≈ 5.6 µs |
| Everything on (dB, A-weighting, ballistics), defaults otherwise | 47.6 µs (dB 6.0, ballistics 2.3, weighting 1.7) | – |
| Async **off** (inline, cache-evicted), defaults | – | **71.7 µs mean / 117 p99 / 149 max** |
| Worker pickup after publish (2 ms poll) | median 1.1–1.3 ms, max 2.7–2.8 ms (bench); **rare ~19 ms excursions** (test, 2 of 5 runs) | – |
| Info-chain access (memoized) | 0.6 µs/cook, against 179 µs for the un-memoized pattern | – |

Commands: `fft_bench --channels 1 --fft 16384 --bins {16384|2048} --db 0 --weight 0 --ball 0 --iters 2000`,
`--cook 300`, and `--info 2000`, all run from the scratchpad against `build/bin/Release`.

**Reading the table.** The FFT is no longer the story. At the defaults, the warp and peak search
(12.2 µs) are more than half the FFT's cost (22.5 µs). They scale with *output bins*, not with
information. The cook-thread cost is dominated by the output copy, which also scales with bins.

---

## 2. Findings

### 2.1 🔴 Wrong behaviour (fix first; each is small)

**F1 — The no-AVX2 error clears itself on the first cook.** Verified in the code.
- The constructor sets `myErrorText` on a CPU without AVX2 (`FFT.cpp:237-240`).
- `executeImpl` then zeroes the output and **returns normally** (`FFT.cpp:837`).
- The success path of `execute()` clears any latched error (`FFT.cpp:975-978`).

Result: on exactly the machines this guard exists for, the node outputs silence with **no error
badge**, and logs a misleading "recovered" line. *Fix:* check `!myCpuOk` in `getErrorString` directly,
or skip the clear when the CPU check failed.

**F2 — The tests import the user's real wisdom cache.** Verified.
- `test_v23_helpers` runs 9th and calls `prepare()` (`dsp_tests.cpp:817`).
- That triggers the once-per-process `importWisdomOnce` (`DSPModules.h:2263-2295`) with the default
  `%LOCALAPPDATA%` path.
- The private override is only set later, in `test_background_plan` (`dsp_tests.cpp:508`), which
  runs 13th.

So "measure in background" paths may be skipped depending on what the user's machine has cached, and
the test result depends on machine state. The bench exports into the same live file. *Fix:* set the
override as the first line of `main()` in the tests, bench and probes.

**F3 — The documented baseline is not reproducible.** Measured. `CHECK(worst_ms < 6.0)`
(`dsp_tests.cpp:1107`) failed in 2 of 5 runs (worst 18.86 ms against 2.5–3.4 ms in the passing runs),
so "607 checks, 0 failures" is a flaky gate.

The same behaviour matters beyond the test (see F9): the worker's hot poll *can* overshoot by more
than one 60 fps frame. The scratch probe (1 000 waits, same primitive) gave p50 2.5 / p99 2.9–3.1 /
max 3.9–4.6 ms, so the excursions are rare but real.

**F4 — Version and defaults drift.** Verified.
- `plugin.json` says `"version": "2.9.0"`; the code says 2.9.1.
- `README.md:203` says Zero-Pad default is **32768**; the code opens on index 4 = **16384**
  (`Parameters.cpp:344`, `Parameters.h:280`).
- The README performance header is still "N = 32768".
- Test comments still say "32768 = the default pad" (`dsp_tests.cpp:1122,1128,1196`).
- `dsp_tests.cpp:1079` `CHECK(tb2.acquire() || true)` is a tautology that inflates the count.

**F5 — Ingest assumes every cook delivers only new samples.** Verified in code; failure mode inferred.
- `ingest()` pushes the newest `min(numSamples, capacity)` samples of each cook's block into the
  FIFO (`FFT.cpp:520-571`).
- That is right for a timesliced audio input. It is wrong for a non-timesliced input that re-delivers
  an overlapping or identical buffer each cook, such as a fixed-length Audio File In, a Trail, or a
  Lookup-driven buffer. The same samples get appended again, and the "window" becomes a stutter of
  repeated blocks.
- `OP_CHOPInput` exposes `startIndex` and `totalCooks` (`CPlusPlus_Common.h:1480,1507`).

*Fix:* track `startIndex + numSamples` per cook and ingest only the delta (clamped to `[0, n]`).
Handle discontinuities with a FIFO reset, and treat a non-advancing input as "replace window".

**F6 — `getErrorString` has a dead branch.** Verified. `mySampleRate <= 0.0` (`FFT.cpp:1454`) can
never be true, because the rate is clamped to ≥ 1.0 (`FFT.cpp:876`, `Parameters.h:315`). "No usable
sample rate" is therefore never reported. *Fix:* keep the *raw* rate and report on that.

### 2.2 🟠 Real-time cost

**R1 — The output is massively oversampled at the defaults.** Measured and derived.
- The window is 3175 samples: 13.9 Hz Rayleigh resolution, and a Kaiser-15 main lobe ~135 Hz wide.
- It is zero-padded to 16384, which gives 8193 linear bins.
- It is then warped to **16384** output bins.

Computed on the actual grid formula (`DSPModules.h:1282-1337`, Log, blend 0.963, 44.1 kHz):

| N / Output Bins | bins < ¼ FFT-bin apart (pure interpolation) | bins coarser than the FFT grid (skipping) | max decimation |
|---|---|---|---|
| 16384 / **16384** (default) | **61.8 %** | 17.6 % (above 6.85 kHz) | 3.4× |
| 16384 / 4096 | 38.0 % | 38.2 % (above 1.96 kHz) | 13.6× |
| 16384 / 2048 | 20.4 % | 49.3 % (above 1.08 kHz) | 27.1× |
| 8192 / 2048 | 38.1 % | 38.3 % | 13.5× |

The cost of the oversampling is measured in §1.1:
- On the worker: warp 9.4 µs and peak 2.8 µs.
- On the cook: the 7.6 µs copy.
- On everything downstream in TouchDesigner: CHOP ops, CHOP to TOP uploads, viewers. This is the
  largest term and is not measured here. It is paid on TD's main thread for every sample.

**The biggest real-time lever in the project is Output Bins**, and the roadmap has left it open since
v2.3 (item 1.4, "Bins = Auto"; the roadmap is now retired, see Appendix B / git history). It needs R2
to be correct.

**R2 — The warp point-samples; it never aggregates.** Verified in code; the consequence is inferred.
- `buildWarpTables` gives each output bin an `(i0, w)` pair (`DSPModules.h:1357-1393`), and
  `applyWarp` does a 2- or 4-tap interpolation (`:1399-1506`).
- Where the output spacing exceeds the FFT spacing, the linear bins between two taps are **skipped**.
- At the defaults this is mostly harmless: 3.4× decimation against a ~50-bin-wide main lobe.
- At the bin counts you would want for real-time (1–4 K), decimation reaches 13–54×. A narrow partial
  whose lobe falls between taps then loses level and flickers as it moves, which is visible on
  high-frequency content.

*Fix:* per output bin, precompute `[lo, hi]`, the range of linear bins it covers. Where `hi - lo > 1`,
output `max` (peak-hold, the right choice for display) or power-mean (the right choice for energy)
over the range. Otherwise interpolate as today. The cost is one pass over ≤ `nlin` floats (~1–2 µs
AVX2), and it makes low bin counts both cheaper *and* more faithful.

**R3 — The Info DAT path allocates and does a filesystem syscall on every cook.** Verified in code;
the cost is not measured.
- Row 15 calls `FFTWEngine::wisdomPathFor(be)` → `wisdomPath()`, which runs `GetEnvironmentVariableA`
  **and `CreateDirectoryA`** (`FFT.cpp:1206-1210`, `DSPModules.h:2228-2242`). That is a filesystem
  syscall on the cook thread, every cook, whenever the Info DAT is evaluated.
- Every fixed row builds `std::string`s with `std::to_string` and concatenation. Row 19 alone
  concatenates about 10 pieces (`FFT.cpp:1265-1269`).
- The plan-log rows add up to **256** extra `getInfoDATEntries` calls per cook
  (`DSPModules.h:361`, `FFT.cpp:1114`).
- The popup string is rebuilt from scratch every cook, about 15 allocations (`FFT.cpp:1306-1401`).

The `--info` bench only models the status/log *access*, not this formatting, so the "0.6 µs" figure
does not cover it.

*Fix:*
- Cache the wisdom path once per backend.
- Render the fixed rows into a reused `char[20][256]` table only when the status version or a
  telemetry tick (e.g. 4 Hz) changes.
- Cap the plan-log rows shown in the DAT (e.g. the last 32).
- Rebuild the popup only when its inputs change.
- Then extend `fft_bench --info` to measure the real rendering functions.

**R4 — The peak search runs on every job even when nobody reads it.** Measured: 2.8 µs at 16384 bins,
8 % of the DSP (`AnalysisPipeline.cpp:603-614`). The value only feeds Info CHOP channels 5/6 and the
DAT/popup. *Fix:* let the info callbacks set an atomic "peak wanted" flag and skip the search
otherwise, or search the linear magnitude once (8193 bins) instead of the warped output.

**R5 — Worker pickup depends on a timer, not on the cook.** Measured.
- The hot worker polls every 2 ms (`FFT.cpp:748`, `FFT.h:383`). Median pickup is ~1.2 ms, max
  ~2.8 ms in the bench, with rare ~19 ms excursions (F3).
- A `SetEvent` from the signalling thread costs **6.8 µs p50 / 25.7 µs p99** and wakes a blocked
  HIGHEST-priority consumer in **~0.01–0.17 ms** (scratch probe).
- The design deliberately keeps that 7–26 µs off the cook. The price is up to ~2.8 ms of pickup
  latency plus the rare frame-sized excursion, and 500 timer wake-ups per second while hot.

At 60 fps this is harmless. At 120–240 fps (4–8 ms frames) the tail starts to matter.

*Options, to benchmark with `--cook`:*
- (a) Keep polling, but register the worker with **MMCSS** ("Pro Audio"/"Games") and opt the process
  out of power-throttling timer coalescing (`PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION`).
- (b) A hybrid: signal only when the previous job was picked up late.
- (c) A `Low-latency pickup` toggle that signals every cook (~7 µs).

Whichever is chosen, report the pickup p99 in the Info DAT.

**R6 — Blocking joins on configuration changes.** Verified in code.
- `prepare()` → `destroyPlan()` → `joinBackground()` (`DSPModules.h:2322-2332, 2677-2689`) blocks
  the pipeline owner until an in-flight MEASURE/PATIENT plan finishes. That is up to ~0.8 s
  (N = 65536 MEASURE) or ~2.7 s (PATIENT).
- With **Async off**, the owner is the **cook thread**, so a pad or rate change during a background
  measurement freezes TouchDesigner for that long.
- `pollBackgroundPlan()` also takes the process-wide planner mutex (`DSPModules.h:2632-2641`). Another
  FFT node's 2.7 s PATIENT run can therefore block this node's owner thread at swap time.

*Fix:*
- Never join on the owner thread. Move the finished-or-running measurement into a "graveyard" that is
  reaped when `m_bg_running` is false.
- Use `try_lock` in `pollBackgroundPlan` (retry next job).
- Bound PATIENT with `fftwf_set_timelimit` where the backend supports it.

**R7 — Minor hot-path items.** Verified in code; each is small.
- Allocation per job with more than one channel: `std::vector<int> idx` (`AnalysisPipeline.cpp:580`),
  plus whatever `std::execution::par` allocates. This breaks the "no allocation" invariant for All
  Channels.
- `TripleBuffer` keeps `m_mid` (shared), `m_back` (producer) and `m_front` (consumer) on one cache
  line (`DSPModules.h:642-645`): false sharing between cook and worker. `alignas(64)` each.
- `IngestState::eq.setSampleRate` is called per state per cook (`FFT.cpp:513`). It is cheap, but the
  EQ could track the rate once.
- `/fp:fast` (`TDPlugin.cmake:267-268`) is fine on MSVC. Under a future Clang/GCC build,
  `-ffast-math` would fold the `!(x > 0)` and `std::isfinite` guards (`DSPModules.h:1848`,
  `AnalysisPipeline.cpp:429`) to no-ops.

### 2.3 🟠 DSP quality (improving it can also lower cost)

**Q1 — The default Kaiser β = 15 is far more window than the display needs.** Derived. β = 15 on
3175 samples at 44.1 kHz gives:
- a main-lobe half-width of ~68 Hz (null-to-null ~135 Hz);
- sidelobes of roughly −110 dB or better.

The default dB range is **80 dB**. β ≈ 10.7 would give ~−80 dB sidelobes with a ~49 Hz half-width
(**27 % sharper**), and β ≈ 8 gives ~38 Hz. On a Log axis that spends a third of its bins below
~200 Hz, the wider lobe is the dominant bass blur.

*Option:* `Kaiser Beta = Auto`, derived from `dB Range Floor` with the Kaiser design relation
(β ≈ 0.1244 · (A + 6.3)), keeping the manual override.

**Q2 — One window length for every frequency.** A 72 ms window is too short for the bass (13.9 Hz
resolution, and a log axis wants ~1–3 Hz at 30–60 Hz) and too long for transients in the treble
(~36 ms of centre latency).

The standard fixes are:
- a **multi-resolution FFT**: 2–3 window lengths, each owning an octave range, stitched on the output
  grid;
- a **constant-Q transform**, e.g. Brown–Puckette sparse kernel on one FFT.

The cost is small at these sizes: an N = 4096 transform is ~3 µs, and an N = 8192 one is 7.5 µs
(§1.1). This is the one change that would visibly upgrade the node for music visuals. See Phase 3.

**Q3 — Interpolation happens on linear magnitude.**
- Interpolating before the dB stage makes log-axis curves "sag" between peaks.
- Interpolating dB values (or interpolating power and then taking 10·log10) is visually smoother. In
  dB mode it can also drop the `sqrt`: take 10·log10 of |X|², which saves the rsqrt/Newton step in
  `computeMagnitudeAVX2_FMA` (`DSPModules.h:2103-2147`).

This is a design choice to expose, not a bug.

**Q4 — The NumPy reference is not a usable oracle as is.** Verified by the second reviewer and
spot-checked.
- `FFT_REFERENCE/Audio_Live2.py` still carries the **old, wrong Bark inverse** (py:314 against
  `DSPModules.h:1256-1257`).
- It caps the log floor differently.
- It makes silence read 0 dB.
- It uses a whole-window FIR "EQ".

Fix the Bark inverse there and generate golden vectors offline into a committed header.

> **Update (2026-09-23, later).** The Bark inverse was fixed in the v2.10 work (commit `8e18b41`).
> `Audio_Live2.py:316` now reads `(z + 4.422) / 1.22`, matching the C++. The `FFT_REFERENCE/`
> folder was then removed from the repository on 2026-09-23. It is kept locally and gitignored
> (`.gitignore:77`), so the golden-vector idea would need its source vendored back in first.

### 2.4 🟡 Threading and memory model (latent, all currently benign on x64/MSVC)

- **T1.** `AnalysisPipeline::backendInfo()` reads `myBackend`, a plain pointer, from the Info thread
  while the owner rewrites it (`AnalysisPipeline.h:163-165`, `AnalysisPipeline.cpp:142`). Make it
  `std::atomic<const FftBackendInfo*>`.
- **T2.** Comments that contradict the code:
  - `FFT.h:387-388` says a non-trivially-copyable payload invalidates the triple buffer. It does not:
    slots are rotated, never copied, and `AnalysisJob` already holds `std::vector`s.
  - `DSPModules.h:676-678` says a signal "is lost if nobody is waiting". The Win32 auto-reset event
    (`:688`) latches it; that behaviour is exactly why dormancy works.
- **T3.** The Info callbacks and `pulsePressed` run on TD's main thread, the same thread as the cook,
  so `myIngest[..].eq.reset()` in `pulsePressed` (`FFT.cpp:1480`) is safe today. It becomes a race if
  TouchDesigner ever cooks this CHOP off the main thread. Keep the "flag, don't act" rule for the EQ
  too.
- **T4.** `Planner` ↔ `FFTDSP::PlannerPolicy` index alignment is unasserted (`Parameters.h:425`,
  cast at `AnalysisPipeline.cpp:137`). Add a `static_assert` like the backend ones.

### 2.5 🟡 Tests and bench (condensed from the second reviewer; the key claims were verified)

- **Coverage holes:**
  - All of `FFT.cpp`: ingest, `fillJob`, hold, worker lifecycle, the dormancy Dekker handshake.
  - `DbRef` Dbfs/Agc; FullScale DC/Nyquist halving; weighting/dB/ballistics *inside* `process()`;
    silence with dB on.
  - Backend/pad rebuild through `process()`; the EQ frequency response.
  - **The warp's decimated regime (R2).**
- **Weak assertions:**
  - Every FFT check uses an impulse (|X| = 1 everywhere), so bin permutation or ordering bugs pass,
    and the backend-equivalence test is nearly vacuous.
  - Window *shape* is never checked.
  - "Golden" A-weighting is one anchor point plus inequalities.
- **249 of the 607 checks are per-element loop checks**, and several branches are machine-dependent,
  so the count signal is weaker than the README implies.
- **The bench measures a replica.**
  - `fft_bench` does not link `AnalysisPipeline.cpp` (`CMakeLists.txt:73` against `:70`).
  - `--cook`'s `runJob` ignores `--db/--weight/--ball/--eq`, skips `setBackgroundAllowed` /
    `pollBackgroundPlan` / the `update*` keys, and does not model dormancy.
  - Its defaults (N 32768, dB/weighting/ballistics on) are not the plugin's defaults.
  - p99 from n = 300 is the 3rd-worst sample.
  - The `findPeakWithIndex` results are discarded, which is a dead-code-elimination risk under `/GL`.
- **Nothing is a gate.** Every figure is printf. No perf regression can fail a build.

### 2.6 🟡 Repository and documentation

- **Documentation entropy.** There are five overlapping markdown files, roughly 270 KB. The two perf
  documents are now mostly "Correction to the correction" blocks and history, and the source is about
  60–70 % comments by volume, a lot of it version history. (Both perf documents were retired later the
  same day; their still-valid content is in Appendix B, the rest is in git history.)
  - The *content* is honest and valuable.
  - The *shape* makes the real contract hard to find, and every doc pass risks drift. F4 is exactly
    that.
  - Consolidate: one README (use and contract), one ARCHITECTURE (threading and RT invariants), and
    one PERF log (dated measurement table), with history moved to the CHANGELOG. Code comments should
    say *why this is shaped this way*, not *what version N did*.
- **`RateModel.h:1-17` carries Derivative's "Shared Use License" header** on original code that has no
  Derivative content. The FFT.cpp/Parameters.cpp headers are inherited from the template, which is
  fine. Decide the project license deliberately; FFTW makes distribution GPL anyway (README §License).
- **Configure dirties the tree.** It regenerates `3rdParty/fftw3/lib/libfftw3f-3.3.11-avx2.lib` in the
  source tree (`TDPlugin.cmake:438-459`), so `git status` is dirty after every configure. This is a
  builder-side fix (see the PluginBuilder_V2 plan).
- **Build artefacts:**
  - The 521 MB oneMKL runtime is present in `build/bin/Release`. That is what makes PluginBuilder's
    hot-reload hash ~550 MB per reload (see the V2 plan, R-B1). It should live only in `__Plugins__/FFT`.
  - `__Plugins__/FFT/FFT.dll.old` is a stale leftover.
- **Dev config.** `launch.vs.json` has three configurations with the same name. `.vscode/*` has
  machine-absolute paths. There are no test or bench presets.

---

## 3. What is genuinely good (keep it)

- **The cook-thread discipline is right and measured.** Wait-free triple buffers both ways, no mutex
  or syscall or allocation in steady state (except R3's info path), parameters polled once in
  `getOutputInfo`, FTZ/DAZ on both threads.
- **Table caches keyed on exactly their inputs** (`WindowKey` / `WarpKey` / `WeightKey`), with warp
  versions propagating into weighting.
- **The runtime backend table.** No link-time FFTW, a clean fallback, no `FreeLibrary`, and MKL
  forced to its sequential layer so there is only one OpenMP runtime.
- **Honest telemetry.** Latches clear on success (except F1), the popup length is bounded, and pickup,
  hold and drop counters are reported.
- **A TD-free core** (`DSPModules.h`, `AnalysisPipeline`, `RateModel.h`) that tests and bench can drive
  headlessly. This is what makes everything below cheap to verify.

---

## 4. The plan

Every item has an **acceptance criterion that is a number or a check**, because that is what this
project already does well. Effort: S ≤ ½ day, M ≤ 2 days, L > 2 days.

### Phase 0 — Truth and hygiene (≈ 1 day, no behaviour change except the bug fixes)

| # | Item | Effort | Accept when |
|---|---|---|---|
| 0.1 | F1: the no-AVX2 error survives the cook | S | A unit test of the error path; node shows error on a `myCpuOk=false` build flag |
| 0.2 | F2: `wisdomPathOverride()` first in `main()` of tests, bench and both probes | S | Tests pass with `%LOCALAPPDATA%\TD_Custom_FFT` renamed away; the bench never writes the user's wisdom |
| 0.3 | F3: move timing checks behind a `perf` ctest label, or assert p95 over 200 waits instead of max over 20 | S | 20 consecutive `fft_tests` runs report 0 failures |
| 0.4 | F4: sync `plugin.json` 2.9.1; fix README pad default/header; fix test comments; remove the tautology at `dsp_tests.cpp:1079` | S | A grep for `32768 = the default` returns nothing; the check count changes by exactly −1 |
| 0.5 | F6: report a raw input rate ≤ 0 | S | An input with rate 0 shows the error |
| 0.6 | T1/T4: atomic `myBackend`; `static_assert` Planner == PlannerPolicy | S | Builds; the assert fires if an enum is reordered |
| 0.7 | Commit the pending comment-only working tree (after 0.4 fixes its wrong comments; see second reviewer §D: `FFT::getInputInfo`, "C++ guards NaN", py↔C++ claims) | S | The tree is clean; the comment-strip diff is empty |

### Phase 1 — Cook and info path to "zero" (≈ 2–3 days)

| # | Item | Effort | Accept when |
|---|---|---|---|
| 1.1 | R3: cache the wisdom path; pre-render fixed Info DAT rows at ≤ 4 Hz or on version change; cap plan-log rows (32); rebuild the popup only on change | M | New `fft_bench --info` mode that calls the *real* render functions: < 2 µs/cook steady state, 0 allocations (counting `operator new`) |
| 1.2 | R4: peak search only when an info consumer asked this frame (atomic flag) | S | Default DSP −2.8 µs at 16384 bins; peak still correct when the Info CHOP is attached |
| 1.3 | R7: `alignas(64)` triple-buffer indices; hoist `idx` into a member; EQ rate tracked once | S | `--cook` p99 not worse; allocation gate green with 4 channels |
| 1.4 | **Allocation gate**: replace global `operator new/delete` in `fft_tests` with counters; assert 0 allocations over 1000 steady-state `process()` + cook-simulation cycles | M | Gate green at 1 and 4 channels |
| 1.5 | **Bench the real code**: link `AnalysisPipeline.cpp` into `fft_bench`; move the worker/handshake out of `FFT.cpp` into a TD-free `AsyncAnalysis.h` shared by the plugin and the bench; bench defaults = plugin defaults | M | `--cook` numbers come from the same functions TD runs; `--db/--ball/--eq` take effect |
| 1.6 | **Perf gate**: `fft_bench --gate` (pinned affinity, interleaved, n ≥ 5000, p50/p99/p99.9/max, JSON baseline ± tolerance) under a `perf` ctest label | M | A deliberate 10 % slowdown in the warp fails the gate |

### Phase 2 — Output-side cost and quality (≈ 1 week). This is the real-time win.

| # | Item | Effort | Accept when |
|---|---|---|---|
| 2.1 | **R2: an aggregating warp.** Per output bin `[lo,hi]`; `max` (default) or power-mean where `hi-lo > 1`, interpolation elsewhere; AVX2 | M | New test: a tone swept across 200 positions at 1024 and 2048 bins has peak-level ripple < 0.5 dB (point-sampling today: expect several dB); warp ≤ 2 µs at 2048 bins |
| 2.2 | **`Output Bins = Auto`** (retired roadmap 1.4; see Appendix B / git history). Choose `n_out` so the output spacing ≈ ½ main-lobe width at every frequency, capped by a user maximum | M | Default output ≤ 2048–4096 bins with no visible loss against 16384 (A/B screenshot + peak test); cook ≤ 6 µs mean |
| 2.3 | **Q1: `Kaiser Beta = Auto`** from `dB Range Floor` (β ≈ 0.1244·(A+6.3)); keep the manual override | S | At 80 dB: β ≈ 10.7, main lobe −27 %; a sidelobe test shows the floor still below range |
| 2.4 | **Quality preset menu** (`Custom / Visual 60 fps / Visual 120 fps / Analysis`) that sets pad, bins, interpolation and β together (e.g. Visual = N 8192, cubic, Auto bins, Auto β) | S | Visual preset: worker ≤ 12 µs, cook ≤ 6 µs, and a 2-bin-separated test tone pair still resolved at 200 Hz |
| 2.5 | **Q3: dB-domain interpolation option** (and \|X\|² → 10·log10 skipping sqrt when dB is on) | S | dB-mode DSP −1–2 µs; visual A/B |
| 2.6 | **F5: `startIndex`-aware ingest** (delta ingest; discontinuity → reset; non-advancing input → window replace) | M | Tests with a timesliced input, a static buffer, an overlapping sliding buffer, and a gap |
| 2.7 | **R6: no blocking joins on the owner thread** (graveyard thread + `try_lock` in `pollBackgroundPlan` + `fftwf_set_timelimit` for PATIENT) | M | A pad change during a PATIENT measurement with Async off: cook max < 1 ms (today: seconds) |
| 2.8 | **Grey out inactive parameters with `setParameterEnableStates`** (Common API 3, installed TD 2025.33230; needs the PluginBuilder_V2 SDK resync, V2 plan 1.6). EQ sub-parameters when EQ is off, dB Reference/Range when Loudness = Off, attack/release in the unused unit, Kaiser Beta for non-Kaiser windows. The SDK calls it outside the cook, so it costs the cook nothing | S | Parameters grey out; `param_reads` unchanged |

### Phase 3 — Latency and the worker (≈ 1 week, measure-driven)

| # | Item | Effort | Accept when |
|---|---|---|---|
| 3.1 | **R5: pickup policy.** Benchmark (a) MMCSS + throttling opt-out, (b) hybrid late-signal, (c) always-signal; expose the winner as `Pickup = Poll / Signal` if they differ materially | M | Pickup p99 < 0.5 ms (Signal) or < 3 ms with no > 8 ms excursion over 100 k jobs (Poll) |
| 3.2 | Report pickup p50/p99 and "late pickups" in the Info DAT (not in the popup) | S | Visible in TD; constant popup length preserved |
| 3.3 | **Latency accounting.** Report the window-centre delay (win/2), the async frame and the pickup together as one `analysis_latency_ms` Info channel; optionally an asymmetric (e.g. Kaiser-left / short-right) low-latency window | S–M | A number users can compensate visuals against |
| 3.4 | Dormancy/lost-wake stress test around the 500 ms threshold (random publish gaps) | S | 10⁶ publishes, every job picked up within poll + ε |

### Phase 4 — Capabilities (pick based on use; each is L)

| # | Item | Why it matters |
|---|---|---|
| 4.1 | **Multi-resolution analysis** (Q2): 2–3 FFT sizes (e.g. 8192 / 2048 / 512 at 44.1 kHz) each owning an octave range, stitched on the output grid; or CQT via a sparse spectral kernel | Sharp bass *and* fast treble for music visuals; total cost ≈ 10–15 µs (measured per-size costs in §1.1) |
| 4.2 | **Spectral features** as extra channels, computed on the worker for free-ish: flux / onset strength, centroid, rolloff, RMS/peak, band energies (bass/mid/high), optional chroma-12 | These are what visuals actually drive; today users rebuild them downstream in Python/CHOPs at far higher cost |
| 4.3 | **GPU-side output**: a companion TOP (CPU-memory upload of a 1×N or history texture: spectrogram) built from the same `AnalysisPipeline` | Skips the CHOP→TOP hop; a spectrogram is a ring-buffered texture write |
| 4.4 | Phase / complex output (optional), useful for reassignment or phase-vocoder style effects | Requested in the EssentiaTD comparison §9.1 |
| 4.5 | Consolidate the docs (§2.6) and move the version string to one generated header from `plugin.json` | Stops F4-class drift permanently |

### Phase map (expected real-time outcome)

| After phase | Cook (Async on, default) | Worker DSP (default) | Downstream TD samples/frame |
|---|---|---|---|
| Today | 12.1 µs mean / 18.6 p99 | 35.6 µs | 16 384 |
| 1 | ≈ 12 µs (info path cleaned; cost moves off the cook) | ≈ 33 µs | 16 384 |
| 2 (Visual preset / Auto bins) | **≈ 5–6 µs** | **≈ 10–12 µs** | **≈ 2 048–4 096** |
| 3 | ≈ 5–6 µs (+7 µs if Signal pickup is chosen) | same | same, with pickup p99 bounded |

The phase-2 figures come from the measured 2048-bin and N 8192 cubic rows in §1.1. The aggregating
warp adds ~1–2 µs, which is inferred.

### Status addendum (end of 2026-09-23): what shipped, and Phase 5

Shipped in **v2.10.0–v2.12.0** (details in `CHANGELOG.md`):
- **Phases 0–1** in full.
- **2.1** (aggregating warp), **2.3** (Auto β), **2.4** (quality presets), **2.6** (`IngestCursor`),
  **2.7** (planner graveyard + `try_lock` + PATIENT time limit), **2.8** (enable states).
- **3.1** (Worker Wake Poll/Signal + MMCSS), **3.3** (`analysis_latency` Info row).
- **4.2** (spectral features).
- **2.2** was redefined by the user in v2.11. Output Bins Mode **Auto = N/2+1 of the (zero-padded) FFT**,
  and it is the default. **Fixed** = exactly Output Bins (upsampling allowed). The Raw RFFT Bins and
  Zero-Padding toggles were added.
- The resolution-derived count described in 2.2 above is gone. Output-count semantics are the user's
  call; never change them for performance.
- **v2.12** added the AVX2/FMA pass (A/B-measured, full chain 1.64×) and corrected the FFTW vs oneMKL
  figures: **oneMKL is 18–27 % faster**.

**Still open:** 2.5 (dB-domain interpolation), 3.2/3.4, 4.1 (multi-resolution), 4.3 (TOP), 4.4 (phase),
4.5.

**Phase 5: from the EssentiaTD review and the installer plan (2026-09-23)**

Sources: `ESSENTIATD_LESSONS_FOR_PLUGIN_FFT_2026-09-23.md` (§ numbers below refer to it) and
`INSTALLER_PLAN_2026-09-23.md`.

| # | Item | Effort | Accept when |
|---|---|---|---|
| 5.1 | **Don't publish a job on a stale cook** (no fresh samples): hold the last result (lessons §2.1) | S | Test: the input cooks every 2nd frame, so flux never reads 0 on held frames and ballistics don't advance on held frames |
| 5.2 | **Ballistics dt from audio time** (fresh samples / sr), not `deltaMS` (lessons §2.2) | S | Same attack/release trajectory at 735 / 882 / 1470-sample timeslices, within 1 % |
| 5.3 | Sanitize `timeInfo->rate` (finite, 1..1000, else 60) wherever it is read | XS | A unit test with NaN / inf / 1e9 |
| 5.4 | **Warning slots** + **coverage warning** (window < samples per cook) + `analyzed_fraction` and `true_resolution_hz` Info rows (lessons §3, §2.3, §1.1) | S | Two warnings coexist; the coverage warning fires at 1600 > 1024 and not at 800 < 1024 |
| 5.5 | **Headless `FFT::execute()` harness** (EssentiaTD `TDStubs.h` pattern) and cook-path tests | M | 5.1–5.4 are tested through the shipped `execute()` |
| 5.6 | HFC (nearly free in `featureSums`), mel band energies, MFCC-13 | M | Each against an independent reference; the allocation gate stays green |
| 5.7 | SuperFlux novelty on a short sub-window + optional adaptive onset pulse (time-based) | M | Onset F1 on a click track ≥ plain flux; no vibrato false positives on a test tone |
| 5.8 | **Installer** (Inno Setup, per-user, core + optional oneMKL component, TD-running / AVX2 / VC++ runtime checks); add a LICENSE and ship FFTW's COPYING first | M | Windows Sandbox install → node loads → uninstall clean (installer plan §8) |
| 5.9 | Check whether TD `LoadLibrary`s every DLL in the Plugins tree at start-up (the 14 oneMKL DLLs: 3 core + 5 CPU kernels + 6 VML); relocate oneMKL if so | S | TD cold-start time with and without the oneMKL set, measured |

---

## 5. Things measured before that should not be re-optimised

These are from the roadmap (now retired; see Appendix B / git history) and CHANGELOG and remain valid.
Appendix B.1 adds the other measured facts the retired docs held:
- FFTW threads (13–51 % slower on a single 1-D transform).
- `FFTW_DESTROY_INPUT` (slower).
- PATIENT as the default (−12 % execute for 2.7 s of planning, so it stays opt-in).
- Parameter polling every N cooks (removed: latency for ~20 µs).
- Update-rate divider (removed).
- Per-cook branches (< 0.1 µs).
- The interpolated two-gather dB LUT (2× slower, no visible gain).

---

## 6. Measurement protocol, applied to every change

1. `fft_tests` must show 0 failures in 20 consecutive runs, with the check count stated and its delta
   explained.
2. `fft_bench --gate` against the committed baseline: interleaved, pinned, n ≥ 5000, p99.9 reported.
3. `fft_bench --cook 2000` at 16384 and 2048 bins, in both Async modes, with pickup stats.
4. `fft_bench --info` on the *real* render functions (after 1.1).
5. In TouchDesigner: Info CHOP `cook_time_us` / `dsp_time_us` / `hold_frames`, plus the Performance
   Monitor for the downstream network. The downstream cost is the one this plan expects to move most,
   and no headless bench can see it.

---

## Appendix — Evidence log (2026-09-23)

- **`fft_tests.exe`, 5 runs.**
  - Runs 1–2: `607 checks, 1 failures` at `dsp_tests.cpp:1107`, `waitFor(2 ms): worst 18.86 ms`.
  - Runs 3–5: `0 failures`, worst 2.53 / 3.36 / 2.94 ms.
- **Scratch timer probe** (`CreateWaitableTimerExW(HIGH_RESOLUTION)` + `WaitForMultipleObjects`,
  1000 × 2 ms):
  - default: p50 2.49 / p99 2.91 / max 4.62 ms;
  - throttling opt-out: max 3.92 ms;
  - `SetEvent`: 6.8 µs p50 / 25.7 µs p99 on the signalling thread, wake 0.01–0.17 ms.
- **`fft_bench`** rows as in §1.1. Plan `FFTW_MEASURE (from wisdom)`, `fftw-3.3.11-sse2-avx-avx2-avx2_128`.
- **Grid statistics** (§2.2 R1) were computed from the exact `computeTargetHzGrid` formula for
  Log / blend 0.963 / floor 20 Hz.
- **Kaiser figures** use the main-lobe half-width `√(1+(β/π)²)·fs/L` and the Kaiser design relation
  for attenuation (approximate for a window's peak sidelobe).

---

## Appendix B — Carried over from the retired performance docs (2026-09-23)

`FFT_REALTIME_OPTIMIZATION_ANALYSIS.md` (**ANA**) and `FFT_REALTIME_PERFORMANCE_ROADMAP.md` (**RM**)
were deleted on 2026-09-23. Most of their content was either history (already in `CHANGELOG.md`) or
plan items that have since shipped (§4 addendum). This appendix keeps only what is **still true at
v2.12.0** and **not already said** in this audit or the CHANGELOG. Each item was re-checked against
`PluginProjects/FFT/source/` today. Full text: `git show 8e18b41:FFT_REALTIME_PERFORMANCE_ROADMAP.md`
(same for the other file).

Caveat on old numbers: unless marked i9, the figures from those docs were taken on the original
i7-class development machine at N = 32768. Trust the ratios; the absolute µs cannot be compared with §1.1.

### B.1 Measured or verified facts (adds to §5; do not re-optimise these)

- **FTZ/DAZ is already in place, on both threads.** `FFTDSP::DenormalGuard` sets MXCSR `0x8040`
  (`DSPModules.h:224-226`). If the cook time spikes on digital silence, a new float path is running
  outside a guard. That is a coverage regression: fix the coverage, do not add a second FTZ.
  (RM §1.2, §4 step 5)
- **Aligned and unaligned loads compile to the same code on MSVC (`vmovups`).** The 32-byte
  `AlignedAllocator` is there for portability and as an explicit contract, not for speed. The
  contract is still real: an unaligned input to a `_mm256_load_ps` kernel **faults**, it does not
  merely run slower (`DSPModules.h:1915`). New kernels take `AlignedVector` inputs.
  (RM §3; ANA §7 item 2)
- **The silence short-circuit is narrow on purpose.** It only fires when
  `silent && Loudness == Off && !ballEnable` (`AnalysisPipeline.cpp:376`): a dB mode must still publish
  its floor, and ballistics must still decay. Widening the condition would be a correctness bug, not a
  speed-up. (RM §1.5)
- **"Magnitude only up to the warp's max bin" saves nothing at the defaults.** It exists
  (`AnalysisPipeline.cpp:307`), but Display Max defaults to 24000 Hz (`Parameters.h:465`), which is
  above the 22.05 kHz Nyquist at 44.1 kHz. The saving only appears when Display Max sits below Nyquist,
  so do not count it in a default-configuration budget. (RM §1.9)
- **A slow SIMD loop is not proof that SIMD is wrong.** The pre-`adc8945` SIMD dB loop measured slower
  than scalar because every vector took a store/reload round trip through `alignas(32)` stack
  temporaries. The fix was a better vector kernel. The v2.12 register-spill results are the same
  lesson: read the assembly before concluding anything. (ANA §5.2)

### B.2 Rejected or closed ideas, and why (so nobody tries them again)

- **Update-rate divider / hop control** (`Update Every N Cooks`, and the `Min Hop ms` variant that was
  never built). §5 lists it as removed; here is why. The divider only skipped *worker* jobs. The cook
  thread pays for ingest and the copy on every frame either way, so it halved the spectrum rate and
  bought no frame-budget saving (CHANGELOG v2.7.0). It is now doubly wrong, because it would also
  change what the flux and onset features mean. If a node's total CPU ever matters, 5.1 (don't publish
  on a stale cook) is the principled version of the same idea. (RM §1.6)
- **`cookEveryFrameIfAsked` to save idle cooks.** RM §3 said it cost nothing and only avoided unused
  cooks. That is **superseded**. With it, an unpulled node never reaches the Info, warning or error
  callbacks, and the popup comes up empty (CHANGELOG v2.9.0, `FFT.cpp:239-263`). `cookEveryFrame =
  true` is load-bearing. (RM §3)
- **In-place r2c to shrink the cache footprint.** Both docs listed this as the open footprint idea.
  v2.12 measured in-place execution with the per-frame re-zero at 10–20 % slower (CHANGELOG v2.12,
  "FFT backend and plan flags"), so it is closed.
  - Footprint for reference (derived from `DspState`, `AnalysisPipeline.h:182-184`, not measured):
    at N 16384 with Auto bins, one channel is ≈ 64 KB padded frame, 64 KB complex scratch, 32 KB each
    for magnitude, previous spectrum, previous linear (features only) and the result, plus the shared
    warp tables. That is roughly 0.25–0.3 MB, well inside a P-core L2.
  (RM §1.10, ANA §7.1)
- **FFTW_EXHAUSTIVE, or a size-adaptive planner.** EXHAUSTIVE caused multi-second plan stalls (the
  "32 s at N 16384" figure was never re-verified). There is no size threshold anywhere in the tree,
  so do not go looking for one. The planner is a user policy (`Auto`/`Fast`/`Measured`/`Patient`)
  plus a background upgrade and wisdom. That combination removed the stall/quality trade-off
  instead of choosing a side of it. (ANA §1 entries 1, 8, 10, 13; §6.2)
- **Sliding or recursive DFT on the log grid:** O(bins) work per *sample* (16384 × 735 per frame),
  orders of magnitude worse than one FFT. The arithmetic settles it. (RM Tier 3)
- **GPU FFT (cuFFT / VkFFT) for the transform itself.** The estimate: per-frame upload and download
  (~192 KB per channel at N 32768) plus sync dominate below ~16 channels. It was never measured, and
  it is a different thing from 4.3, which is GPU *output*. (RM Tier 3)
- **pffft:** never tried and never ruled out. With the vendored AVX2 FFTW and oneMKL already
  available (oneMKL 18–27 % faster, §4 addendum), there is no case for a third backend unless GPL
  removal becomes a goal. (RM §1.8 row 3)
- **`/fp:contract` spelled out explicitly:** redundant, because `/fp:fast` implies it. (ANA §4)

### B.3 Still-open ideas, and where they would fit

| Idea | Source | Where it fits | Note |
|---|---|---|---|
| Batched `fftwf_plan_many_dft_r2c` for `Channels = All Channels` | RM §1.8 row 5, §6 | Phase 4 (new item, after 4.1) | Probe: `howmany = 4` gave 86.3 → 29.9 µs per transform (CHANGELOG v2.7.0). It only helps All Channels; Mono Mix (default) issues one transform. The v2.8.0 path was removed because `fftwf_plan_with_nthreads` mutated global FFTW state. A retry must be single-threaded `plan_many`, built under the planner mutex, and checked against the existing `std::execution::par` fan-out (−38 % at 2 channels). |
| Unroll `applyWarp` / `applyWarpCubic` to 2 × 8 lanes per iteration | ANA §7.1 | Phase 1.6 perf gate, as a candidate A/B | Both still run one 8-lane block (`DSPModules.h:1476-1530`, `:1656`). After the v2.12 load+permute rewrite, and given that unrolling features ×2 measured 12 % slower (spills), expect nothing. Unmeasured, low priority. |
| GPU FFT for ≥ 16-channel instances | RM Tier 3 | Phase 4, after 4.3 (it would share the GPU plumbing) | Only if a many-channel use case appears. See B.2. |

### B.4 Measurement protocol additions (for §6)

- **Measure both surfaces.** With Async on, a change can move work between the cook thread
  (`--cook`, `cook_time_us`) and the worker (per-stage bench, `dsp_time_us`) without changing either
  total. Report both. (RM §4)
- **Pin the plan and name the backend.** Pass `--planner fast|measured|patient` so a plan difference
  cannot pass for a code difference. Check the echoed configuration: an unknown flag name is skipped
  silently, and an unknown `--planner` value silently falls back to Auto (`bench.cpp:463-465, 496-499`).
  Only `--backend` warns on an unknown value, and it takes names (`mkl`, `fftw3`). Passing `--backend 1`
  is how the v2.12 "FFTW faster than oneMKL" mistake happened. (RM §4 step 1)
- **Null-plugin floor.** A CHOP that outputs zeros at the same bins × channels measures what
  TouchDesigner itself charges for the output. Nothing in this plugin can go below that. With the DSP
  on the worker, this floor plus the result copy is most of the cook-thread budget.
  **TD cook time − `cook_time_us` = host overhead.** (RM §4 steps 3–4)
- **Silence test.** Feed digital silence and watch for cook-time spikes, as the regression check for
  B.1's FTZ/DAZ coverage. (RM §4 step 5)
