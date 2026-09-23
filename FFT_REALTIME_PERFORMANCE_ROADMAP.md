# FFT CHOP — Real-Time Performance Roadmap (how to make the cook loop as fast as it can be)

> **What this document is.** A forward-looking plan: a measured breakdown of where the cook loop's
> time goes, and a ranked list of changes that would make it cheaper — with the outcome of every item,
> since most of the plan has now been built. It is a *plan with a scorecard*, not a description of the
> node.
>
> **Who it is for.** A developer deciding what to work on next. If you are using the node rather than
> changing it, read `README.md`.
>
> **How it relates to the other markdown files.**
>
> | File | What it holds |
> |---|---|
> | `README.md` | The user-facing description: what the node does, every parameter, how to build it, and the **current** measured performance numbers |
> | `CHANGELOG.md` | The version history, newest first, one entry per release — this is where each item below is recorded as done or removed |
> | [`FFT_REALTIME_OPTIMIZATION_ANALYSIS.md`](FFT_REALTIME_OPTIMIZATION_ANALYSIS.md) | The *backward-looking* sibling: a commit-by-commit analysis of the optimization work up to `e9985c0`, and the priorities that came out of it |
> | **This file** | The plan: where the time goes, what to change, and what each change turned out to be worth |
> | [`../PluginBuilder_V2`](../PluginBuilder_V2) | The builder/toolchain this plugin is compiled and hot-reloaded through (CMake module, SDK headers, `plugin.json`, `PluginBuilder.tox`) — any idea below that touches CMake flags, link mode or deployment must respect that contract; smoke test `python ../PluginBuilder_V2/dev/ci.py --project PluginProjects/FFT` |
>
> **Measured, estimated, or idea — read this before trusting a number.** Every row in §0 and every
> figure marked *measured* comes from `bench/bench.cpp` (`fft_bench`, interleaved runs, minimum per
> stage) and is re-checkable by running it. Every figure marked *estimate* is the author's
> projection — **not** a measurement — and is kept as written so the reasoning stays auditable.
> Everything in §1 that is not marked either is an **unmeasured idea**. The `Expected:` lines
> throughout §1 are estimates by construction; several of them turned out to be wrong in interesting
> ways, and where that happened the real measured figure is given next to them.
>
> **Status of the whole plan, at a glance (as of v2.9.1, 2026-09-21).** Of the ten numbered items, **six
> are implemented** (1.1, 1.2, 1.5, 1.7, 1.8, 1.9), **two shipped and were then deliberately removed**
> (1.3, 1.6), and **two remain open** (1.4, 1.10) — neither of which was ever the big win. The scorecard:

| § | Item | Status |
|---|---|---|
| 1.1 | Asynchronous pipeline | **DONE** — v2.3.0, `Async Analysis` toggle, default on |
| 1.2 | FTZ/DAZ on the cook and worker threads | **DONE** — v2.3.0, `DenormalGuard` |
| 1.3 | Parameter polling interval | **DONE then REMOVED** — shipped in v2.3.0, removed in v2.7.0 |
| 1.4 | Output Bins sized to what is displayed | **OPEN** — guidance only; no `Bins = Auto` exists |
| 1.5 | Silence / no-change short-circuit | **DONE** — v2.3.0 |
| 1.6 | Update-rate divider | **REMOVED** — shipped in v2.3.0, removed in v2.7.0 |
| 1.7 | Smaller FFT + cubic interpolation | **DONE** — v2.3.0, `Warp Interpolation = Linear \| Cubic` |
| 1.8 | FFT library swap | **DONE** — v2.9.0, vendored FFTW 3.3.11 AVX2 + `FFT Backend` toggle |
| 1.9 | Magnitude only up to `Display Max` | **DONE** — v2.3.0 |
| 1.10 | Cache footprint | **OPEN** — in-place r2c not implemented |
| Tier 3 | Parallel channels / GPU / sliding DFT | **SUPERSEDED** — the parallel-channel *parameters* were removed in v2.8.0; the fan-out is now automatic |

> **Status (v2.4.0, 2026-09-10):** the cook thread is now lock-free and syscall-free (wait-free triple buffers both
> ways, worker polls on a high-resolution timer, peak search on the worker, parameters polled from `getOutputInfo`,
> dead `Parallel*` reads removed). Measured with the new `fft_bench --cook`: **11 µs mean / 17 µs p99** per cook at
> 16384 bins, 7 µs at 4096 bins, of which 60 % is the `Bins × channels` result copy. See CHANGELOG v2.4.0.
>
> **Status (v2.3.0, 2026-08-26):** implemented — 1.1 async pipeline (default on), 1.2 FTZ/DAZ, 1.3 parameter poll
> interval, 1.5 silence short-circuit, 1.6 update-rate divider, 1.7 cubic warp interpolation (option), 1.9 magnitude
> only up to Display Max, plus **Channels = Mono Mix** (mono-first: one FFT for any input). Not done: 1.4 Bins Auto
> (guidance only), 1.8 FFT library swap, 1.10 cache footprint, tier 3.
>
> **Correction to the second block.** It used to read **"v2.3.0, same day"**. That was wrong: v2.3.0 is
> **2026-08-26** and v2.4.0 is **2026-09-10**, sixteen days apart. The "same day" confusion is understandable —
> several other releases *are* same-day pairs (v2.5.0 / v2.6.0 / v2.7.0 / v2.8.0 are all 2026-09-12, and
> v2.9.0 / v2.9.1 are both 2026-09-21) — but v2.3.0 and v2.4.0 are not one of them. Dates are from the version
> headings in `CHANGELOG.md`.
>
> **Status (v2.9.1, 2026-09-21) — where the plan stands now.** The project is at **v2.9.1**, nine releases
> past the v2.2.2 baseline this file was written against (v2.3.0, v2.4.0, v2.5.0, v2.6.0, v2.7.0, v2.8.0,
> v2.8.1, v2.9.0, v2.9.1 — from the headings in `CHANGELOG.md`). Since the two status blocks above:
> the two "every N cooks" parameters were **removed** (v2.7.0) because the async worker had already made
> their saving irrelevant; the parallel-channel and `FFT Threads` knobs were **removed** (v2.8.0) because
> the node is one mono channel per instance; `Raw Linear Bins` was **removed** (v2.8.0) as a second spelling
> of settings that already existed; and the FFT library question that 1.8 was planning for was **settled**
> (v2.9.0) by vendoring FFTW 3.3.11 built with AVX2 + FMA and adding an `FFT Backend` toggle that can load
> Intel oneMKL's FFTW3 interface instead. So of §1, **1.1, 1.2, 1.5, 1.7, 1.8 and 1.9 are done**, **1.3 and
> 1.6 are removed**, and **1.4 and 1.10 remain open** — neither was ever the big win. The cook thread is
> not the bottleneck any more; the current work is interface quality and correctness, not cook-loop speed.
> Details per item below.

> Date: 2026-08-26 · Baseline: v2.2.2 (`0851d19` — verified: `git log --oneline -1 0851d19` reports
> `perf(FFT v2.2.2): restore the July real-time budget - EQ at ingest, section bypass toggles, background
> measured plans`) · All numbers measured on this machine with `bench/bench.cpp`
> (interleaved runs, min per stage) unless marked *estimate*. 1 channel, N = 32768, window 3175, 16384 bins, Log scale.
>
> **Correction to the line above.** "This machine" is the machine the file was written on, which is the
> **original development machine** — the one `README.md` labels "i7-class desktop". It is **not** the current
> development machine (an **i9-13900H**, Raptor Lake, AVX2 + FMA, no AVX-512, 6 core / 12 thread). The numbers
> in §0 are the *same vintage* as the historical block in `README.md` § *Performance* (41–48 µs FFT+mag,
> 4.8 µs warp, 44–51 µs total), which is filed there as the pre-i9 machine; the i9 re-measurement gives
> **28.96 µs** total per channel for the default configuration. The two are **not comparable** — different CPU,
> different FFTW build, different plan — so do not read the difference as a speed-up. **Flagged as inferred**:
> this file does not itself record the CPU or the build, so the machine attribution is by matching the numbers
> against `README.md`, not by a note in this file.

---

## 0. Where the time goes today (measured)

Default TD configuration (EQ / Loudness / Weighting / Ballistics off):

| stage | µs / channel / cook | share | branch-free? |
|---|---|---|---|
| FIFO ingest + linearize | 0.4 | 1 % | yes (memcpy) |
| window multiply | 0.3 | 1 % | yes (AVX2) |
| **FFT r2c 32768 + magnitude** | **36 (measured plan) / 44–54 (estimate plan)** | **80–85 %** | FFTW |
| warp (16384 gathers) | 4.8 | 10 % | yes (AVX2 gather) |
| output memcpy (64 KB) | ~1.5 | 3 % | yes |
| peak telemetry | 0.02 | – | yes |
| bypassed stages (eq / weighting / dB / ballistics) | 0.02 each | 0 % | – |
| **total DSP** | **≈ 44–51 (cold) / ≈ 42 (measured plan)** | | |

Measured, original development machine, FFTW 3.3.x with an AVX2-capable build, `Planner Fast` vs a
measured plan, N = 32768 with 3175 non-zero samples, 16384 output bins, `Scale = Log`, 1 channel. Shares are
percentages of the total row and are rounded — the FFT row alone is 80–85 % of it.

Not in the bench, but paid on the TouchDesigner cook thread every frame:

| host-side cost | how big | how to see it |
|---|---|---|
| `getPar*` calls (**20** by default, **33** with everything on) | *estimate* 1–5 µs each → 20–100 µs | Info CHOP `param_fetch_us`, `param_reads` |
| TD allocating/handing out a 16384-sample × N-channel CHOP, downstream cooks, viewers | scales with **Bins × channels** | TD performance monitor minus `cook_time_us` |
| Python bind expressions on the PluginBuilder COMP (only while its parameter dialog is visible) | *estimate* 20 µs × 33 pars | close the dialog → gone |

> **Correction — the parameter counts.** This table used to say **"19 by default, 31 with everything on"**.
> Those were the v2.2.2 figures and they are **stale**. The verified counts today are **20** and **33**
> (`README.md` § *Performance*, "reads — when disabled: `eval()` reads **20** parameters per cook in the
> default configuration, against **33** with" everything on), confirmed by counting the `getI` / `getD` /
> `menu` calls in `Parameters::eval()` (`PluginProjects/FFT/source/Parameters.cpp` —
> each of its three lambdas increments a counter `n`, and `n` is what is reported). The counts moved because
> parameters were added (the `FFT Backend` menu in v2.9.0, the async/planner entries) and removed (the two
> "every N cooks" in v2.7.0, `Raw Linear Bins` and `FFT Threads` in v2.8.0). `CHANGELOG.md` v2.7.0 says
> "~18 `getPar*` calls" — that was that release's own approximation on a different parameter set, not a
> contradiction. The row's *estimate* of 1–5 µs per read is unchanged and still an estimate: read the live
> number from `param_fetch_us / param_reads` rather than trusting it. `param_reads` is Info CHOP channel 13
> (`FFT::getInfoCHOPChan()`, `PluginProjects/FFT/source/FFT.cpp`).

### Straight answer to "are the ifs slowing the loop?"

No — measured. The per-cook code has 16 branches in `executeImpl` and 16 in `processChannel`; they run **once per cook**
(not per sample), cost < 0.1 µs total, and every bypassed stage measures 0.02 µs. All per-sample / per-bin loops are
branch-free AVX2 (blends instead of ifs). The loop is slow for exactly three reasons, in this order:

1. **the FFT is 32768 points** for a 3175-sample window (90 % zeros) — 80 % of the DSP,
2. **the output is 16384 bins** — warp, memcpy and everything TouchDesigner does downstream scale with it,
3. **the work runs on TouchDesigner's cook thread**, serialized with everything else in the frame.

Everything below attacks those three.

> **Two corrections to this paragraph.** First, **`processChannel` no longer exists**: the per-channel
> function is `AnalysisPipeline::runChannel(DspState&, const AlignedVector&, bool silent, const Values&,
> float attackCoef, float releaseCoef, float agcDecay, AlignedVector&) noexcept`
> (in `PluginProjects/FFT/source/AnalysisPipeline.cpp`), declared in
> `PluginProjects/FFT/source/AnalysisPipeline.h`. And `executeImpl` has grown: the cook is now
> five numbered steps (1–5, with `myExecStage == 0` meaning "outside the cook") with the async publish and the deferred log flush among them
> (`FFT::executeImpl()` in `source/FFT.cpp`, with `FFT::execute()` as its `try`/`catch`
> wrapper). The steps are now numbered in comments 1 to 5, with `myExecStage` set to 1–4 for the first four
> and left at 0 for the fifth: **1** parameters (normally already polled by `FFT::getOutputInfo()` for this cook),
> **2** channel-count resolution + `FFT::ingest()`, **3** the analysis job (start/stop the worker, publish, take
> the newest result), **4** `FFT::copyResultsToOutput()`, **5** the deferred Textport log flush
> (`if (myLog.hasPending()) myLog.flushToTextport();` — step 5 gets no `myExecStage` marker, so an error
> there reports "outside the cook"), then `myExecStage = 0` on the way out. Second, **the "16
> branches / 16 branches" counts are unverified** — they were not re-counted
> against the current source, and the source they were counted in no longer exists in that form. The
> *conclusion* (per-cook branches are not the cost) is still sound and is now even more clearly so,
> because reason 3 was the one that mattered and reason 3 is **fixed**: the work no longer runs on the
> cook thread when `Async Analysis` is on, which is the default (`≈ 11 µs mean / 17 µs p99` per cook at
> 16384 bins, measured with `fft_bench --cook`). Reasons 1 and 2 are still true.

---

## 1. The upgrade list, ranked by real-time gain

> Each item keeps its original text. A **Status** line under the heading says what happened, with the
> symbol to look at, then the measured outcome where there is one. Where the original `Expected:` line
> was proved wrong, both numbers are given.

### Tier 0 — structural (the big one)

#### 1.1 Asynchronous pipeline: cook = ingest + copy; the FFT runs on a worker thread
**Status: DONE (v2.3.0, default on).** The `Performance > Async Analysis` toggle is the `Async` toggle this
item asked for, and it defaults **on** — `README.md` § *Parameters*: "FFT & post-processing on a worker thread;
the cook only ingests and copies (≈ 11 µs at 16384 bins, 7 µs at 4096, measured). Off = inline, and **one thread
for the whole node**: no worker, and no background plan measurement either". The mailbox is
`FFTDSP::TripleBuffer` (in `source/DSPModules.h`) and the job/result pair is `AnalysisJob` /
`AnalysisResult` (`source/AnalysisPipeline.h`). Measured result: **≈ 11 µs mean / 17 µs p99** per cook at
16384 bins with `fft_bench --cook`, caches evicted between cooks — against the 44–51 µs of DSP this item set out
to remove. Note the design did not end up as a per-channel worker pool: it is **one analysis worker thread per
node instance** (`FFT Custom CHOP analysis`, `THREAD_PRIORITY_HIGHEST`), and several channels means several
nodes. See also 1.8.

*Original text follows.*

*Expected cook-thread cost: 44–51 µs → **≈ 5–10 µs per channel** (ingest 0.4 + output memcpy 1.5 + bookkeeping).*

How real analyzers (DAW spectrum meters, Ableton/RME) do it. The cook thread only:
1. appends the new audio block to the FIFO (0.4 µs),
2. publishes a snapshot of the window (12.7 KB memcpy) to the worker's mailbox,
3. copies the **last completed** spectrum into `output->channels` (1.5 µs).

A worker thread (one per plugin instance, or one pool for all channels) does window → FFT → magnitude → warp →
options and writes into a back buffer; a sequence counter (`std::atomic<uint32_t>`) flips front/back. If the worker
hasn't finished when the next cook arrives, the cook re-uses the previous spectrum (hold) — never blocks.

- Latency: +1 frame (16.7 ms @ 60 fps) on a 72 ms window → invisible. Make it a toggle (`Async`), default on.
- Determinism: output is always a complete spectrum; no tearing (double buffer + atomic index).
- Thread: high priority, pinned nowhere (let the scheduler place it), sleeps on a condition variable; `FTZ/DAZ` set.
- FFTW execute is thread-safe on distinct arrays; planning stays on the cook thread (already mutex-guarded).
- With N channels: one worker per channel (they are independent) or a small pool; for 2 channels one worker is enough
  (2 × 42 µs = 84 µs of work per 16.7 ms frame = 0.5 % of a core).

This is the only change that moves the plugin from "≈ 100 µs per cook for stereo" to "≈ 15 µs per cook for stereo",
whatever the FFT size. Everything else below then only improves *throughput*, not the frame budget.

> **Deviations from the sketch, worth knowing if you are reading the code.** (a) The worker does **not** sleep on
> a condition variable — it polls on a high-resolution timer, which is what removes the syscall from the cook's
> wake-up path (v2.4.0: "no locks, no kernel calls, nothing that is not a copy"). (b) The cook thread does **not**
> do the 12.7 KB snapshot memcpy into a mailbox it then publishes; it publishes a job and the *worker* takes the
> snapshot, so the cook pays 2 µs rather than the estimate here. (c) Planning **does** leave the cook thread: the
> `Auto` planner policy measures plans on a background thread (`FFT background planner`,
> `THREAD_PRIORITY_ABOVE_NORMAL`) under FFTW's process-wide planner lock. (d) The 2-channel arithmetic in the
> last bullet assumes per-channel workers; the shipped design is one transform per node instance
> (`Channels = Mono Mix` is the default and the design target), so the fan-out question is answered by
> "several nodes" instead.

### Tier 1 — cheap, safe, do first

#### 1.2 Flush-to-zero / denormals-are-zero on the cook (and worker) thread
**Status: DONE (v2.3.0).** Implemented as `FFTDSP::DenormalGuard` (in `source/DSPModules.h`):
`DenormalGuard() noexcept : saved(_mm_getcsr()) { _mm_setcsr(saved | 0x8040u); }` and
`~DenormalGuard() { _mm_setcsr(saved); }` — the RAII save/restore this item asked for, setting both FTZ
(bit 15) and DAZ (bit 6) in MXCSR. It is constructed once per cook at the top of the work:
`FFTDSP::DenormalGuard ftz;` in `FFT::executeImpl()` (in `source/FFT.cpp`); the worker thread sets the
same bits for its own lifetime. The effect is the one predicted — no per-sample denormal stalls on quiet input.

*Original text follows.*

*Expected: prevents 10–100× per-sample slowdowns on quiet audio.* Biquad IIRs and the ballistics filter decay into
denormal floats on silence; every operation on a denormal costs ~100 cycles. Set `_MM_SET_FLUSH_ZERO_MODE` /
`_MM_SET_DENORMALS_ZERO_MODE` at the top of `execute()` (save/restore MXCSR around the cook). This is likely one
reason "it feels slower sometimes": sporadic slowness when the input goes quiet.

#### 1.3 Parameter polling interval
**Status: DONE, THEN REMOVED (v2.3.0 → v2.7.0).** It shipped exactly as proposed, as
`Performance > Parameter Poll Every N Cooks` (default 1), and `Parameters::Values::paramPoll` held it.
It was **removed in v2.7.0**, and the reason is worth reading before re-proposing it — `CHANGELOG.md`
v2.7.0: *"It skipped `Parameters::eval` on N−1 cooks, saving ~18 `getPar*` calls (~20 µs, host-side). The
price was UI latency: a parameter change took up to N frames to appear. Every parameter is now read on
every cook, so a change lands on the next frame — and the ~20 µs is ~0.1 % of a 60 fps frame."* The
`getPar*` machinery is now an unconditional `eval()` per cook (`FFT::pollParameters()`, `source/FFT.cpp`), and `param_reads`
reports the count live. In today's numbers: **20 reads by default, 33 with everything on** (see the
correction in §0). The item's own advice — *measure the per-read cost first* — is the advice that sank it:
the saving was real but ~20 µs on a 16.7 ms frame is 0.1 %, which does not buy a frame of UI lag.

*Original text follows.*

*Expected: −(19…31 reads × per-read cost) on most frames.* TD has no "parameter changed" callback for C++ OPs, so
`Parameters::eval()` re-reads everything every cook. Add `Param Poll` (frames, default 1; e.g. 4 = read every 4th
cook, ~66 ms UI latency — fine for an analyzer). Menus/toggles that gate DSP rebuilds are cheap to keep polling.
Measure the actual per-read cost first via `param_fetch_us / param_reads` in the Info CHOP — if it is ~1 µs, this
tier is worth ~20 µs; if ~5 µs, it is worth ~100 µs and beats everything except 1.1.

> The `19…31` in the expected line is the stale count; it is **20…33** today. Kept as written so the estimate can be
> read in the terms it was made in.

#### 1.4 Output Bins sized to what is displayed
**Status: OPEN — the guidance stands, the `Bins = Auto` mode was never built.** `Output Bins` still defaults to
**16384** and is a plain `Int` parameter (hard-clamped 8…262144; slider max 65536, raised from 32768 in v2.8.0),
not a menu with an automatic entry — see `README.md` § *Parameters* and the pads in
`source/Parameters.h` (`kPadValues`, `kPadDefault`, `kMinBins`, `kMaxBins`). What `README.md` does now is
*explain* the trade-off rather than recommend a number ("Fewer bins means the axis is coarser (each output bin
averages a wider slice of the linear grid)"), which is the honest form of this item. The predicted saving is
still real if you set it by hand: warp scales with the gather count, and everything TouchDesigner does
downstream scales with `Bins × channels`.

*Original text follows.*

*Expected: warp 4.8 → 0.6 µs at 2048 bins, memcpy 1.5 → 0.2 µs, and TD downstream cost ÷8.* A 16384-bin spectrum
is 8× more samples than a 1920-px-wide display can show. Recommend 2048–4096 in the README; consider a `Bins = Auto`
mode (= 2 × FFT bins needed for the chosen `Display Max`, capped).

#### 1.5 Silence / no-change short-circuit
**Status: DONE (v2.3.0) — with one condition the original text does not mention.** `AnalysisPipeline::runChannel()`
opens with the short-circuit (in `source/AnalysisPipeline.cpp`), whose guard is
`if (silent && p.loudness == Parameters::Loudness::Off && !p.ballEnable)`: when it fires, the channel's output is
filled from the previous spectrum and the window → FFT → magnitude → warp chain is skipped entirely. The guard is
narrow on purpose — the comment above it explains that *"the output of a silent frame is NOT zero - a dB mode has
to publish the floor, and a ballistic"* filter has to decay towards its floor rather than jump. So the short-circuit
guarantees the right *picture* in every configuration and the cheap path only in the default one (no dB mode, no
ballistics). The `silent` flag itself is the caller's "this whole window is digital silence" and is computed once per
job, so the per-channel check is a branch on a flag rather than a scan of the audio.

*Original text follows.*

*Expected: 100 % of the DSP skipped while the input is silent or frozen.* If the new block is all zeros and the FIFO
is already all zeros (a per-channel "zero run" counter), output zeros (or the held spectrum) without windowing/FFT.
Cheap: the check is a single AVX2 OR-reduction over the 735 new samples (< 0.1 µs).

#### 1.6 Update-rate divider (hop control)
**Status: REMOVED (shipped v2.3.0, removed v2.7.0).** It shipped as `Performance > Update Every N Cooks`
(default 1). **A `Min Hop ms` parameter never existed** — it is only an alternative suggested here, and it was not
the one built. v2.7.0 removed it with a blunt explanation: *"That saving was already banked in v2.3: the FFT runs
on the worker and the cook thread pays only the ingest (≈ 2 µs) and the result copy (≈ 2–7 µs), neither of which
the divider touched. Net effect was a strictly worse node — half the spectrum update rate for a cook-thread saving
that the async architecture had already made irrelevant. Every cook now publishes a job."* In other words, **1.1
subsumed 1.6**, which is exactly what this file predicted at the bottom of §1.1 ("Everything else below then only
improves *throughput*, not the frame budget"). Do not re-propose it: with the worker in place there is nothing left
on the cook thread for a divider to save. Separately, `raw_linear` (Info CHOP 17) lost its
"Update Every N Cooks" suffix and `outputBandwidth()` no longer divides by the divider.

*Original text follows.*

*Expected: cost ÷N on average, e.g. 30 Hz analysis at 60 fps = half.* Today the spectrum is recomputed every cook
with a 735-sample hop on a 3175-sample window (77 % overlap). An `Update Every N Frames` (or `Min Hop ms`) parameter
holds the last spectrum between updates; with 1.1 the worker simply idles.

### Tier 2 — FFT throughput

#### 1.7 Smaller FFT + better interpolation instead of zero-padding
**Status: DONE (v2.3.0) — and the visual claim was confirmed.** `Warp Interpolation` exists as a two-entry menu,
**`Linear`** (the default) and **`Cubic`**, held in `Values::warpInterp` and applied by
`PerceptualWarping::setInterpolation(int)` (in `source/DSPModules.h`); the cubic path is
`PerceptualWarping::applyWarpCubic()`, a Catmull-Rom 4-tap interpolation, 8 output bins per
iteration. `Zero-Pad Len` was indeed left as it was. The measured outcome: at **N = 16384 with
`Warp Interpolation = Cubic`**, the README reports **15–16 µs FFT+mag and 9 µs warp** for **24–27 µs total per
channel**, and describes it as "visually equivalent to 32K linear" — i.e. the estimate of 16 µs for N = 16384 was
about right, and the "smoother lobes than linear at a third of the cost" claim held. The default N was **not**
changed: the default FFT size is still 16384 bins of output over a padded transform, and the choice is left to the
user, which is a deliberate outcome rather than an oversight.

*Original text follows.*

*Expected: FFT 36 → 6.4 µs (N = 8192) or 16 µs (N = 16384); warp +2–4 µs for cubic.*
The window resolution is 13.9 Hz; the 32768-pt FFT's 1.35 Hz bins are 10× denser than the information content.
The zero-padding only does *cosmetic* interpolation of the magnitude lobes so the log grid (0.0087 Hz apart at 20 Hz!)
looks smooth. A Catmull-Rom (4-tap) interpolation in `applyWarp` on an N = 16384 spectrum gives smoother lobes than
linear interpolation on N = 32768, at a third of the total cost. Keep `Zero-Pad Len` as is; add
`Warp Interpolation = Linear | Cubic` and re-benchmark N = 8192/16384 visually.

#### 1.8 FFT library options
**Status: DONE (v2.9.0) — but not by choosing a row from this table.** The end state is that the plugin ships
**two** FFT libraries and lets you pick at run time with the `FFT Backend` menu: the vendored **FFTW 3.3.11 built
with AVX2 + FMA**, and **Intel oneMKL**, loaded through its FFTW3-compatible interface. The rows below were
therefore resolved as a *feature*, not as a migration: no `IFFTEngine` implementation was swapped out, and
`pffft` was not adopted. The "not to be confused with" detail worth knowing is that none of this is link-time —
`td_plugin_use_fftw3(FFT VERSION 3.3.11-avx2 DYNAMIC)` links **no import library**, and `fftwf_*` is resolved at
run time through the function-pointer table in `source/FftBackend.h`, because oneMKL exports the same symbol
names. See `IFFTEngine` and `FFTWEngine` in `source/DSPModules.h` and
`PluginProjects/FFT/3rdParty/fftw3/VERSION`.

*Original text follows.*

| option | expected FFT stage | notes |
|---|---|---|
| FFTW 3.3.5 dll64 (the option at the time of writing), measured plan | 36 µs | fftw.org's prebuilt Windows DLL: SSE2 only, **no AVX2/FMA codelets** |
| FFTW 3.3.10 built with `-DENABLE_AVX2=ON` (vcpkg `fftw3[avx2]`) | *≈ 28–32 µs* | same API, GPL |
| pffft (BSD, single header, AVX-capable fork) | *≈ 30–38 µs* | no DLL to ship, no GPL |
| Intel IPP `ippsFFTFwd_RToCCS_32f` / oneMKL DFTI | *≈ 20–28 µs* | fastest on Intel; ~30 MB redistributable |
| `fftwf_plan_many_dft_r2c` (all channels in one call) | −1 µs / channel | batched execution, better cache use |
| `FFTW_PATIENT` via wisdom (background, once, ~3 s) | −2–5 % | free after 1.1's background planner |

> **Row-by-row verdict, since the table is the thing people will skim.**
>
> - **Row 1 (FFTW 3.3.5 SSE2-only)** — obsolete as an option: it is no longer what ships. The project now builds
>   FFTW from source precisely because fftw.org ships no AVX2 Windows binary. The 36 µs figure is the §0
>   measurement, unchanged.
> - **Row 2 (FFTW 3.3.10 AVX2)** — **done, with a version bump**: the vendored build is **3.3.11**, not 3.3.10,
>   and it is built with AVX + AVX2/FMA codelets from source (the `VERSION` file records the upstream URL, the
>   tarball's MD5, the built DLL's SHA-256, and the one upstream patch needed because upstream's CMake build still
>   stamps 3.3.10). The ≈ 28–32 µs estimate was in the right range.
> - **Row 3 (pffft)** — not taken. Nothing ruled it out; the FFTW path had already been built and benched.
> - **Row 4 (Intel IPP / oneMKL)** — **done, via oneMKL's FFTW3 interface, measured: 8.85 µs vs FFTW3's 11.94 µs**
>   at N = 16384 on the development machine, same binary, only the library differing; oneMKL was ahead in all four
>   paired runs (13–34 %). The "~30 MB redistributable" note is the reason this is a toggle rather than a
>   replacement — nothing is shipped, the plugin loads `mkl_rt.3.dll` only if you switch the backend and it is
>   installed. This is also why the estimate in this row was *conservative*: 8.85 µs is well below the 20–28 µs
>   predicted here, because the prediction was for a DFTI rewrite and what actually shipped is the FFTW3 ABI used
>   unchanged.
> - **Row 5 (`plan_many` batching)** — **not implemented, and deliberately so.** It was implemented for a while as
>   `FFT Threads` (v2.7.0) and then removed in v2.8.0 along with its `prepareBatch` / `executeBatchRFFT` path,
>   because it can only ever help `Channels = All Channels` and the default `Mono Mix` issues one transform per
>   cook and cannot use it. The v2.7.0 probe is the evidence: a `howmany = 4` plan drops 86.3 → 29.9 µs per
>   transform (−65 %), so the row's "−1 µs / channel" was far too pessimistic *if* you are in that mode — but the
>   node is not. See `CHANGELOG.md` v2.8.0 and `bench/fftw_threads_probe.cpp`.
> - **Row 6 (`FFTW_PATIENT` via wisdom)** — **done**: wisdom is imported once per process and exported after every
>   measured plan (`%LOCALAPPDATA%\TD_Custom_FFT\fftwf_wisdom.txt`), and the `FFT Planner` menu offers `Auto`
>   (default; instant plan, background upgrade), `Fast`, `Measured` and `Patient`. `Patient` is exactly the
>   background ~3 s measurement this row predicted, and `README.md` measures it at **−12 % FFT time at
>   N = 32768**.

#### 1.9 Magnitude only where it is needed
**Status: DONE (v2.3.0).** The pipeline computes magnitude only up to the last bin the warp can read:
`AnalysisPipeline::updateWarp()` sets
`myMagnitudeBins = std::min(n_linear_bins, myWarping.maxLinearIndex() + 1);`
(`source/AnalysisPipeline.cpp`, with `PerceptualWarping::maxLinearIndex()` in
`source/DSPModules.h`), and that count is what `IFFTEngine::executeRFFT()` is given. The estimate of
"3 µs → 2 µs when `Display Max < Nyquist`" is the right shape; the saving scales with how far `Display Max` sits
below Nyquist, and is zero when it is at or above Nyquist (the default `Display Max = 24000` against a 22.05 kHz
Nyquist is already at the top, so the default configuration sees **no** saving from this item).

*Original text follows.*

*Expected: 3 µs → 2 µs when Display Max < Nyquist.* `computeMagnitude` runs over all N/2+1 bins; the warp only reads
up to `fmax` (e.g. 16 kHz of 22.05 kHz = 73 %). Stop at the warp's max index.

#### 1.10 Cache footprint
**Status: OPEN — unchanged.** Per-channel buffers are still the padded frame, the complex scratch, the magnitude
and the warped spectrum, and the in-place r2c idea was not implemented. What did change is where the buffers live
and how they are aligned: they are the members of `AnalysisPipeline::DspState`
(in `source/AnalysisPipeline.h`), sized by `AnalysisPipeline::rebuild()`, and they use
`FFTDSP::AlignedAllocator` with **32-byte alignment** by default (in `source/DSPModules.h`) — so the SIMD
loads can be `_mm256_load_ps` rather than `_mm256_loadu_ps`. The `float16`/`uint16` warp table idea was not taken.
So: the row's diagnosis (≈ 400 KB per channel, 8 channels beyond L2) still stands, and its cheapest suggestion
(−128 KB by reusing the input buffer for the complex output) is still the obvious next thing if the footprint ever
matters. `Channels = Mono Mix` is the default and one channel per node is the design target, which is why this
never became urgent.

*Original text follows.*

Per channel: padded frame 128 KB + complex scratch 128 KB + magnitude 64 KB + warped 64 KB ≈ **400 KB**; 8 channels
≈ 3.2 MB — beyond L2, so per-channel cost grows with channel count. Options: in-place r2c (input buffer reused for the
complex output, −128 KB, costs a 118 KB memset of the zero region ≈ 3 µs), `float16`/`uint16` warp tables (−64 KB
shared), and **process one channel completely before the next** (already the case) so each channel's set stays hot.

### Tier 3 — larger channel counts / far options

- **Parallel channels** (exists, off by default): only worth it from ~8 channels; with 1.1 in place it becomes
  "workers per channel" and the question disappears.
  - **Status: SUPERSEDED (v2.8.0).** There is no parallel-channel *parameter* any more — `Performance > Parallel
    Channels` and `Parallel Min Channels` were removed along with `FFT Threads`, for the reason in `CHANGELOG.md`
    v2.8.0: "This node is **one mono channel per instance**: several channels means several nodes, each with its own
    analysis worker thread, so a per-channel fan-out knob can never fire in the intended use." The fan-out itself
    was **not** removed: `AnalysisPipeline::process` runs its channel loop under **`std::execution::par`** whenever a
    job has more than one channel, and serially otherwise, with no knob at all
    (`source/AnalysisPipeline.cpp`; `source/AnalysisPipeline.h`). It reports whether it ran as
    Info CHOP **`channel_fanout`** (channel 20, renamed from `parallel_active` in v2.8.0; in normal mono use it reads
    off, which is correct rather than a fault). Measured (v2.8.0, N = 32768, 16384 bins, `Planner Fast`, outputs
    compared bin-for-bin first so a speed-up cannot come from skipping work): 2 channels 252.9 → 156.1 µs (**−38 %**),
    4 channels 528.0 → 186.3 µs (−65 %), 8 channels 1050.1 → 252.9 µs (−76 %). So the file's "only worth it from ~8
    channels" was wrong in magnitude — it pays from 2 — but right in direction, and it is now free.
- **cuFFT / VkFFT** for 32+ channels: batched 32768-pt FFTs cost ~1 µs each on GPU but the upload/download
  (128 KB + 64 KB per channel per frame) and sync dominate below ~16 channels. Not worth it for stereo.
  - **Status: unchanged, still an unmeasured idea.** Nothing here has been tried; the numbers in the sentence are
    the author's estimates. The current node reports `GPU: none (CPU only)` in the middle-click popup, so there is
    no GPU path at all.
- **Sliding/recursive DFT** on the log grid: O(bins) per sample — 16384 × 735 per frame — far slower. No.
  - **Status: unchanged, still no.** Nothing since has changed the arithmetic, which is what makes it decisive.

---

## 2. What the plan buys, cumulatively (stereo, 60 fps, cook thread)

> **This table is now a scorecard, not a forecast.** It was written against v2.2.2 and predicts the path to
> "≈ 10–15 µs per cook". The measured end state is **≈ 11 µs mean / 17 µs p99** at 16384 bins. Every step below
> except the last was taken; the last was taken in a different form than the row describes, and that form is what
> made the difference. Original text kept.

| step | per cook (2 ch) | comment |
|---|---|---|
| v2.2.2 today | ≈ 85–100 µs + parameter reads | measured DSP 42–50 µs/ch |
| + FTZ/DAZ (1.2) | same, **no more silence spikes** | **done** — `DenormalGuard`, v2.3.0. The "same" claim is the point: the win is the absence of spikes, and it is not visible in a mean |
| + param polling ÷4 (1.3) | −15…−75 µs on 3 of 4 frames | **removed** — v2.7.0 took the ~20 µs and kept every cook's parameters instead; 0.1 % of a frame does not buy a frame of UI lag |
| + Bins 4096 (1.4) | ≈ 75 µs | **open** — still a manual choice. Verified in the shipped numbers: `fft_bench --cook` gives **7 µs** at 4096 bins against 11 µs at 16384 |
| + async worker (1.1) | **≈ 10–15 µs** | **done** — measured **≈ 11 µs mean / 17 µs p99**. The estimate was right, which is rare enough to say so |
| + N = 16384 cubic / AVX2 FFTW (1.7 / 1.8) | worker load ÷2 | **done, and better than ÷2** — both landed, and oneMKL (`FFT Backend`) is a further 13–34 % off the FFT on top |

Target: the FFT CHOP disappears from the frame budget (< 1 % of a 60 fps frame for stereo), with the same picture.

> **Was the target met?** At the 11 µs mean, the cook-thread cost is **0.066 %** of a 16.7 ms frame, and even the
> 17 µs p99 is 0.1 % — comfortably inside the "< 1 %" target, and this is measured (`fft_bench --cook`, caches
> evicted between cooks) rather than projected. The *DSP* itself is not free (≈ 29 µs/channel on the i9 with a
> measured plan) — it has moved to the worker, which is the whole point of 1.1. Note the p99 is what a frame
> budget actually cares about, so quote 17 µs, not 11 µs.

---

## 3. Things that do NOT need changing (measured, so nobody re-optimizes them)

> All five entries still hold. Annotations below; the text is original.

- Stage-level `if`s and the option system: 0.02 µs per bypassed stage.
  - Still true by construction: bypassed stages are still measured at 0.02 µs each in §0. Note the *mechanism* was
    improved since — section toggles now mean the pipeline can skip the stage entirely rather than pay a branch,
    and `BiquadSection::process()` returns its input unchanged when the shelf is inactive, so an "off" section is a
    predicted early-out rather than a branch around real work.
- Window multiply, weighting, dB, ballistics, peak search: all < 5 µs combined and already AVX2.
  - Still true, and now measured per stage on the i9: warp 4.66 µs, dB 3.53 µs, ballistics 1.29 µs (default row
    28.96 µs/channel total). The peak search is `FFTDSP::findPeakWithIndex()` (in `source/DSPModules.h`) and
    it now runs on the **worker** rather than the cook thread, so its cost does not touch the frame budget at all.
- Aligned vs unaligned loads on MSVC: identical code generation (`vmovups`).
  - Still true. This is worth stating plainly because it is counter-intuitive and it is what makes the 32-byte
    alignment work (1.10) a *portability* and *contract* improvement rather than an MSVC speed-up: the aligned
    loads are there so the next compiler does not fault, not because MSVC was slow. The aligned-input contract is
    written down in the section comment in `source/DSPModules.h`.
- `rsqrt`/LUT tricks: magnitude 3 µs, dB 3.5 µs — already in the noise next to the FFT.
  - Still true, and both were done anyway for reasons unrelated to this row: `computeMagnitudeAVX2_FMA` uses a
    `fast_sqrt_ps` (`_mm256_rsqrt_ps` + one Newton-Raphson step, `source/DSPModules.h`) and the dB path
    uses `FastLog10`, a single-gather 2048-entry LUT. The row's point survives: neither was the
    bottleneck, and neither shows up in the totals.
- `cookEveryFrame` vs `cookEveryFrameIfAsked`: no cost difference; the latter just avoids cooking when unused.
  - Still true. The constants are set in `FFT::getGeneralInfo()` (`source/FFT.cpp`:
    `cookEveryFrame = true; cookEveryFrameIfAsked = false;`), and the operator still cooks every frame.

---

## 4. Measurement protocol (before/after every step)

> Unchanged, and still the right protocol. One addition, because the interesting number changed: with `Async
> Analysis` on, the cook thread is no longer where the DSP is, so **measure both surfaces** — the cook-thread cost
> (`fft_bench --cook`, or Info CHOP `cook_time_us`) *and* the worker's DSP cost (`fft_bench` per-stage), because an
> optimization can move work from one to the other without changing either total.

1. `fft_bench.exe --channels 2 --eq 0 --db 0 --weight 0 --ball 0` — DSP per stage (this file's numbers).
   - **Still valid.** All of those flags exist (`bench/bench.cpp` accepts `--channels --fft --win --bins --scale
     --iters --db --eq --weight --ball --interp --fmax --cook --info --planner --backend`). Add `--cook` for the
     cook-thread number, `--planner` to pick `Fast` / `Measured` / `Patient` so a plan difference cannot be read as
     a code difference, and `--backend` to A/B FFTW3 against oneMKL.
2. Info CHOP on the node: `cook_time_us` (inside the plugin), `param_fetch_us`, `param_reads`.
   - **Still valid.** `cook_time_us` is Info CHOP channel **9**, `dsp_time_us` is 10, `param_fetch_us` is 12,
     `param_reads` is 13, `jobs_dropped` is 14, `hold_frames` is 16, `channel_fanout` is 20
     (`FFT::getInfoCHOPChan()`, `source/FFT.cpp`). `hold_frames` is the async-specific one: it counts cooks served
     from the previous spectrum because the worker had not finished — a healthy async build shows small nonzero
     values, not zero and not growth.
3. TouchDesigner Performance Monitor: the node's cook time. **TD time − cook_time_us = host overhead** (output
   handling, downstream, UI). If that gap dominates, 1.3/1.4 matter more than any DSP work.
   - **Still valid**, and note 1.3 is now *removed*, so if the gap dominates there is no longer a polling dial to
     turn: the lever is `Output Bins` (1.4) and downstream node count.
4. Null-plugin floor: a CHOP that outputs zeros with the same Bins × channels measures what TD itself charges for
   a 16384-sample CHOP; nothing in this plugin can go below that number.
   - **Still valid, and now the more useful half of the measurement**: with the DSP on the worker, the cook-thread
     cost is mostly this floor plus the result copy, so the floor is close to the whole cook-thread budget.
5. Silence test: feed digital silence and watch for cook-time spikes (denormals → 1.2).
   - **Still valid as a regression test.** 1.2 is implemented, so a spike here means a regression in
     `DenormalGuard` coverage (or a new float path running outside it), not something to fix by adding FTZ again.

---

## 5. Suggested order of implementation

> The order was followed, and the notes below say how each step went. Original text kept.

1. **1.2 FTZ/DAZ** (10 lines) and **1.5 silence short-circuit** (20 lines) — zero risk.
   - **Done, v2.3.0.** FTZ/DAZ is one RAII class (`DenormalGuard`, 2 lines of MXCSR arithmetic).
2. **1.3 Param Poll** + **1.4 Bins guidance/Auto** — small, measurable in TD immediately.
   - **1.3 was done and then removed** (v2.7.0) — it was small and measurable, and the measurement is what killed
     it. **1.4 was not done**; the guidance lives in `README.md` as an explanation of the trade-off rather than a
     recommendation.
3. **1.1 Async worker** — the real change; ~250 lines (mailbox, double buffer, worker loop, `Async` toggle,
   hold-on-late), plus tests (sequence/tearing, hold behaviour) and bench (`--async` mode measuring cook-thread cost).
   - **Done, v2.3.0.** The `--async` mode exists as **`--cook`**, and the "hold-on-late" behaviour is observable as
     the `hold_frames` Info CHOP channel. The mailbox became `FFTDSP::TripleBuffer` (three slots, so the writer
     never waits for a reader), not a double buffer with a sequence counter — same guarantee, one fewer failure
     mode to test, and it is what makes the cook thread wait-free rather than merely lock-free.
4. **1.7 cubic warp + smaller default N** — visual A/B, then decide the default.
   - **Done, with a different decision than the bullet implies:** cubic warp shipped as an option, and the default
     N was **not** changed. The visual A/B was done and is recorded in `README.md` ("visually equivalent to 32K
     linear" at N = 16384 + Cubic), which is what makes it reasonable to leave the choice to the user.
5. **1.8 FFT backend** — build FFTW 3.3.10 AVX2 or drop in pffft behind `IFFTEngine`; keep whichever benches faster.
   - **Done, v2.9.0, with the version bumped to 3.3.11 and the answer being "both".** The `IFFTEngine` interface
     survived and is what the run-time backend registry plugs into (`source/FftBackend.h`); pffft was not tried. See
     the row-by-row verdict under 1.8.
6. **1.9 / 1.10** polish.
   - **1.9 done** (v2.3.0); **1.10 not done** and no longer urgent, because the design went mono-per-instance and
     the 8-channel footprint the row worries about is now several nodes rather than one node's cache.

---

## 6. If you are picking this up today

The cook-loop work this file describes is finished; the remaining two open items (1.4, 1.10) were never the big
wins, and the two removed items (1.3, 1.6) were removed because the async worker made them pointless. If you want
real work, the honest list is:

- **1.10 cache footprint** — the in-place r2c idea (−128 KB per channel, at the price of a ~3 µs memset of the
  zero region) is still unmeasured. Worth a bench run if you care about many-channel instances or small L2 parts;
  **unmeasured idea**, and the 3 µs price tag is the author's estimate.
- **1.4 `Bins = Auto`** — a menu entry that sizes `Output Bins` from `Display Max` at the current pad size. The
  reasoning in 1.4 still holds and the parameter is still a plain integer, so this is a real (small) feature, not a
  performance fix. **Unmeasured.**
- **Batched `plan_many_dft_r2c` for `Channels = All Channels`** — the one measured, unresolved performance finding
  in `CHANGELOG.md` (v2.7.0): a `howmany = 4` plan takes 29.9 µs per transform against 86.3 µs serial (**−65 %**),
  and the batched path that would use it was removed in v2.8.0 along with `FFT Threads`. It is a real lever for
  exactly one mode. **Measured, but for a probe rather than the shipped node**, and re-implementing it means
  re-introducing the global-FFTW-state hazard that v2.8.0 removed.
- **Everything else here is done.** For the current state of the node's performance, read `README.md` §
  *Performance* — it is measured on the i9-13900H and is the number to quote.
