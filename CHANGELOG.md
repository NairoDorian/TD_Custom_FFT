# Changelog

All notable changes to `Plugin_FFT` are documented here.

## How to read this file

Newest release first. Every version opens with a one-line summary of what it was about, then the
sections below. They mean specific things here, and the distinction has been worth keeping:

| Section | What it means |
|---|---|
| **Added** | Something the node can now do that it could not before |
| **Changed** | Existing behaviour that now behaves differently |
| **Removed (breaking)** | A parameter or feature that is gone. Existing `.toe` files lose it, so these entries say what to do instead |
| **Fixed** | A defect that was actually wrong, described with the mechanism and not just the symptom |
| **Verified** | The evidence: the test check count, the benchmark number, or the check that was run against a real binary. A change with no entry here was not measured |
| **Measured / Confirmed in TouchDesigner** | Numbers and observations from a real run, with the machine named |
| **Notes / Still open** | Design reasoning, and - deliberately - what is *not* established. If a claim in this file could not be verified, it says so here rather than being stated as fact |
| **Evaluated, not kept** | A change that was built and measured, and lost. Recorded so it is not tried again blind |
| **Tests** | Same role as **Verified**, for entries whose evidence is mainly new test coverage |
| **Documentation** | Markdown-only changes that shipped with the release (documents added, retired or corrected) |

Some entries also carry a section named for their topic (for example *"Upgrading a node saved with
v2.10"* or *"FFT backend and plan flags"*). Every entry corresponds to committed work: a
documentation-only pass that shipped without a version bump is filed under the version it followed,
with the commit named in its heading, rather than as "Unreleased".

Two conventions that matter if you are adding an entry:

- **Name the machine** next to any timing. An absolute number without one is not comparable to
  anything, and this project has measurements from two machines in it.
- **Keep the "Still open" sections honest.** They exist because the middle-click popup was
  misdiagnosed three times, and the confident version of that history was wrong. An entry that
  claims more than was measured is worse than no entry.

For what the node does *now* (rather than when it changed), see [README.md](README.md); for the
performance analysis and the current plan, see [AUDIT_AND_PLAN.md](AUDIT_AND_PLAN.md).
(The older FFT_REALTIME_OPTIMIZATION_ANALYSIS.md and FFT_REALTIME_PERFORMANCE_ROADMAP.md, which entries
below still name as history, were retired 2026-09-23; see git history.)

> **Relationship to PluginBuilder_V2:** this plugin is built by the sibling
> [`../PluginBuilder_V2`](../PluginBuilder_V2) repository — CMake module (`TDPlugin.cmake`),
> rename-in-place deploy, API-10 SDK headers, `PluginBuilder.tox` hot reload, and
> `dev/ci.py --project PluginProjects/FFT` as the cross-repo smoke test. Builder-side version
> history lives in *that* repo's CHANGELOG; entries here are DSP/node behaviour only, except
> where a builder contract change is what made a release possible (e.g. 2.9.0's backend toggle
> needed `td_plugin_use_fftw3(... VERSION ... DYNAMIC)`).

---

## [v2.12.1] - 2026-09-23 — FFT Planner = Patient has no time limit

### Changed
- **The background `FFTW_PATIENT` measurement is no longer capped.** v2.10–v2.12 bounded it at 1.5 s with
  `fftwf_set_timelimit`. The measurement runs on its own thread, never on the cook thread, so a slower machine
  may now take as long as it needs to find the best plan: ~2.7 s at N = 32768 on the i9-13900H, once per size
  per machine, then cached in wisdom. The `fftwf_set_timelimit` hookup is removed from `FftBackend.h`; the
  plugin resolves twelve `fftwf_` symbols again.
- The FFT Planner menu label says "no time limit".
- **What can still wait on an unfinished measurement**, because FFTW's planner is process-wide:
  - a re-plan (a size change, another node). With Async on that is the analysis worker, so TouchDesigner
    keeps cooking. With Async off it is the cook thread: no measurement starts while Async is off, but one
    already running when it was switched off finishes first;
  - deleting the node or quitting TouchDesigner while a measurement runs.

### Documentation
- The notes are now living documents with undated names: `AUDIT_AND_PLAN.md` is rewritten as the audit of
  the current code (v2.12.1) with an open-items-only plan; `ESSENTIATD_LESSONS.md` and `INSTALLER_PLAN.md`
  are the EssentiaTD review and the installer plan. History stays in this file and in git.
- `README.md` and `3rdParty/fftw3/README.md` describe the current state only (historical tables and
  "earlier revision" asides removed).

## [v2.12.0] - 2026-09-23 — AVX2/FMA pass: load+permute warp, branch-free peak, table-free dB log, vector features

Every kernel change was A/B-measured before it was kept: the committed v2.11 `fft_bench` against this
tree, run interleaved in random order, pinned to one P-core at high priority, 3 rounds on an idle CPU
(i9-13900H). The table shows medians in µs. Proposals that did not measure faster were not kept (see
"Evaluated, not kept").

| Metric (µs, median) | v2.11 | v2.12 | Speedup |
|---|---|---|---|
| pipeline, full chain (dB + ballistics + features) | 36.35 | 22.20 | **1.64×** |
| pipeline, plugin defaults | 19.10 | 17.60 | 1.09× |
| pipeline, Visual 60 preset | 9.90 | 8.90 | 1.11× |
| cook, synchronous | 28.15 | 25.20 | 1.12× |
| stage: warp, Log linear, 16384 bins | 4.50 | 2.52 | **1.79×** |
| stage: warp, Log cubic, 16384 bins | 8.08 | 4.82 | **1.68×** |
| stage: dB normalised, 16384 bins | 4.22 | 3.58 | 1.18× |
| stage: peak + index, 16384 bins | 1.26 | 0.94 | 1.34× |
| stage table total (Log linear, dB, ballistics) | 24.79 | 21.31 | 1.16× |

### Changed (faster)
- **Warp, linear and cubic: load + permute instead of gather.** i0 is non-decreasing along the axis. When
  the 8 output bins of a vector read taps inside one 8-float window, which is the fine, upsampled part of
  the axis (~83 % of the vectors at the 16384-bin Log default), each tap is one unaligned load plus one
  `vpermps` instead of an 8-element gather. The output is bit-identical to the gather path, and the tests
  compare every grid against a scalar reference.
- **Warp with aggregation: the trailing run of aggregated bins is no longer interpolated.** Aggregation
  overwrites those bins anyway, and they are exactly the gather-heavy coarse part of the axis.
- **dB: `FastLog2Seg` replaces the 8 KB table gather.** It uses the exponent plus an 8-segment quadratic of
  the mantissa, with the coefficients fetched by `vpermps` from registers. The constants fold into one
  FMA per 8 bins, and the reference offset is now exact (`std::log10`, once per call). The maximum error
  drops from **0.0027 dB to 0.00016 dB**, and there is no gather, which is also the slow instruction on
  E-cores.
- **Peak + index: four independent (max, index) vector chains**, with no data-dependent branch. The cost
  no longer depends on the data: the old filter-and-rescan form cost 4.5 µs on a rising spectrum, and this
  one costs ~0.9 µs on any spectrum. The first maximum still wins a tie.
- **Spectral features: vectorised.** The loop is split at the band edges, so there is no per-bin branch.
  The sums use AVX2 float lanes, flushed to double every 256 bins. The rolloff search skips 64-bin blocks,
  and the RMS is vectorised. Measured **~16.5 → ~3.5 µs** at 8193 bins. The comment claiming "2–4 µs" for
  the old loop was wrong.
- **Ballistics: `applyInPlace` writes the output and the state in one pass.** This removes the extra
  16384-float `memcpy` per cook in `runChannel`.
- **Small ones:** `blockIsSilent` masks the sign bit once after the loop (132 → 91 ns).
  `FastLog10::scaledVec` drops a redundant `& 0xFF`, since its inputs are positive.

### Second pass (same day): reductions and the pass structure after the warp
These were measured the same way, with in-process microbenchmarks (min of 40 rounds × 3 reps, pinned
P-core) and interleaved A/B runs of the previous build.
- **`peakMagnitude`: four independent max accumulators.** With one accumulator every iteration waited on
  the previous max's 4-cycle latency, so the loop was latency-bound. **2× faster:** 0.41 → 0.21 µs at
  8193 bins, 0.81 → 0.41 µs at 16384. The dB stage with a Frame Peak reference measured 1.12× faster in
  the stage A/B.
- The same fix went into the other single-accumulator reductions: the rolloff block sums, the RMS sum,
  and the new weighting+max pass.
- **Weighting and the dB reference peak share one pass** (`multiplyInPlaceMax`): 1.0 → 0.57 µs at 8193
  bins. With a dBFS reference, the peak pass is skipped entirely, since that reference never used it.

### Changed (Textport)
Follow-up commit `de8b13a`, same day, same version number.
- **The long backend line is printed once per backend, not once per plan.** It is repeated only on a
  backend switch or when the plan's SIMD kernels change. Every plan still gets its short `plan N=... in
  x ms` line. Resizing on oneMKL used to print the full "FFTW 3.3.4 wrappers to Intel oneMKL [...]" line
  for every size.

### FFT backend and plan flags (measured, nothing to change)
- **oneMKL runs the FFT 18–27 % faster than FFTW:** 3.87 / 8.41 / 17.73 µs vs 4.71 / 11.55 / 23.65 µs
  (FFTW with MEASURE plans) at N = 8K / 16K / 32K, median of 3 pinned runs. FFTW with a PATIENT plan is
  9.70 µs at 16K, so oneMKL still leads by ~13 %. FFTW stays the default because it is 3 MB, vendored
  and wisdom-cached. Choose oneMKL (FFT Backend on) for speed. It then loads 5 DLLs (~177 MB) and spends
  ~39 ms initialising once per process, on the worker thread when Async is on.
  - *Correction:* an earlier version of this entry (commit `01d0c00`) had it backwards ("FFTW beats
    oneMKL at every size"). That bench run passed `--backend 1` where the bench takes a name
    (`--backend mkl`), so both runs used FFTW. Corrected in `de8b13a`.
- **oneMKL deployment trimmed (2026-09-23):** `__Plugins__/FFT/` now holds the 14 oneMKL DLLs the
  backend can dispatch to (plus `oneMKL-licenses/`). `mkl_intel_thread`, `mkl_tbb_thread` and
  `libimalloc.dll` were removed: the sequential threading layer is forced at load, so they are never
  loaded.
- Preserve-input out-of-place, the current setup, is the fastest execute:
  - `FFTW_DESTROY_INPUT` plus re-zeroing the pad each frame is 1–5 % slower;
  - in-place plus re-zeroing is 10–20 % slower.
- `FFTW_PATIENT` executes 10–15 % faster than `FFTW_MEASURE` at 16K/32K (9.70 vs 10.97 µs at 16K). Choose
  **FFT Planner = Patient** for it: the one-time 1.5 s background planning per size is then cached in
  wisdom. It is not the default, because every new pipeline, the test suite's included, would pay that
  planning up front, and engine teardown waits for it to finish.

### Evaluated, not kept (measured)
- **One fused kernel for dB + ballistics + the peak search:** 15 % slower with the peak search in it,
  because ~23 live vectors spill on AVX2's 16 registers. Without the peak search it was 0–4 % faster,
  since at these sizes the spectrum is L1/L2-resident and the dB loop is ALU-bound. Not worth ~150 lines.
- **The features log through `FastLog2Seg`:** 4 % slower than the gather table in that ALU-heavy loop,
  where the gather uses otherwise idle load ports.
- **Unrolling the features loop ×2:** 12 % slower (register spills).
- **Hardware `_mm256_sqrt_ps` for the magnitude:** 45 % slower than the existing rsqrt + Newton-Raphson
  step (1185 vs 820 ns at 8193 bins). A 6-instruction NR variant was not faster either.
- **`cmp(s, d)` instead of `cmp(diff, 0)` in ballistics:** no change. The loop iterations are independent,
  so shortening one iteration's dependency chain buys nothing.
- **Polynomial log2 (degree 3–5) for dB:** 30 % slower than the gather table. The 8-segment form above is
  what beat it.

### Tests
- `test_v212_simd_kernels` covers:
  - both warp kernels vs a scalar reference on five grids (mixed permute/gather, all permute, all gather);
  - the peak index vs a scalar "first max" for 13 lengths × 4 patterns;
  - the silence edge cases (-0.0 and denormals);
  - the dB error bound;
  - the `FastLog2Seg` error bound, scalar and vector;
  - the aggregation skip (NaN sentinel: every bin written, and every Peak bin is a real FFT bin);
  - `applyInPlace` == `apply` + copy, bit for bit;
  - the features vs the pre-2.12 scalar loop;
  - the plan log: the long backend line once per backend, one short `plan N=` line per size (added
    with the Textport change in `de8b13a`).
- Result: 744 checks, 0 failures at `01d0c00`; 748 checks, 0 failures at `de8b13a`.
- `bench/perf_baseline.json` was last written while the machine was under load. Regenerate it on an idle
  machine with `fft_bench --gate bench/perf_baseline.json --update 1`.

### Documentation
- FFT_REALTIME_OPTIMIZATION_ANALYSIS.md and FFT_REALTIME_PERFORMANCE_ROADMAP.md are retired (see git
  history). [AUDIT_AND_PLAN.md](AUDIT_AND_PLAN.md) supersedes both.
- New notes: [ESSENTIATD_LESSONS.md](ESSENTIATD_LESSONS.md)
  and [INSTALLER_PLAN.md](INSTALLER_PLAN.md).
- `FFT_REFERENCE/` (the original Python prototype) is removed from the repository. It is kept on disk
  and is now gitignored.
- `PluginProjects/FFT/3rdParty/fftw3/README.md`: the oneMKL file list is now the trimmed set above,
  with the three unneeded DLLs named and the reason they are never loaded.

## [v2.11.0] - 2026-09-23 — Output Bins drives the output again; raw rfft bins; zero-padding toggle

The node exists to zero-pad the transform and put its spectrum on any number of output bins, including far
more than a plain rfft's N/2+1. v2.10 broke that by making Output Bins Mode = Auto the default: the output
length then followed Window Sampling and Output Bins stopped mattering. This release puts the count back in
the user's hands and adds the two raw-rfft controls that were asked for.

### Changed
- **Output Bins Mode = Auto (the default) is now the rfft's own bin count: N/2+1 of the transform actually
  run.** That is the Zero-Pad Len frame (16384 → 8193 samples, whatever the window: a 4096-sample window
  gave 3081 samples under the v2.10 rule), or the window with Zero-Padding off. The bins are laid out on
  Scale / Display Max. The v2.10 rule ("what the window resolves", capped by Output Bins) is gone, together
  with `autoBinCount` and `kMinAutoBins`. In Auto, Output Bins has no effect and is greyed out.
- **Fixed** makes Output Bins the number of samples in each output channel, whatever the window or the pad.
  For example, 32768 bins from a 16384-point transform (8193 rfft bins) is interpolated 4x (pinned by
  `test_output_bin_modes`).
- **Kaiser Beta Mode defaults to Manual (β 15)**, so the default spectrum has the same shape as in v2.9.
  Auto β stays available, and the presets still use it.
- **Quality presets no longer touch the output count.** Visual 60/120 and Analysis set the pad,
  interpolation, β mode and aggregation only. Visual 120 no longer caps Output Bins at 2048.

### Added
- **Raw RFFT Bins (no interpolation)** (`Rawbins`, Spectrum page, default off). Outputs the rfft magnitude
  untouched: N/2+1 samples, bin k at k·sr/N Hz, DC..Nyquist. The warp is the exact identity and runs as a
  memcpy, and the output matches an independent DFT of the same frame (checked in the tests). Scale, Display
  Max, Warp Blend, Log Floor, Output Bins, Output Bins Mode, Warp Interpolation and Warp Aggregation are
  greyed out while it is on. Weighting, loudness/dB and ballistics still apply, because they act per bin.
  Measured: 11.8 µs p50 vs 18.7 µs for the default warped path (`pipeline_rawbins_p50_us`).
- **Zero-Padding** (`Zeropad`, Spectrum page, default on). Off: the transform runs on the window itself.
  N is the window length, rounded up to even (at most one zero sample) so the last bin is exactly Nyquist.
  Zero-Pad Len is greyed out. With Raw RFFT Bins or Auto, the output is then window/2+1 bins. With Fixed,
  those bins are interpolated onto Output Bins. A window length with large prime factors (e.g. 3175 → 3176
  = 8·397) plans a slower FFT than a power of two, so prefer 2048/4096/... samples with padding off.
- Info DAT `resolution` row: shows the mode (Fixed / Auto = N/2+1 / Raw rfft), the FFT size and whether
  it is zero-padded.

### Upgrading a node saved with v2.10
PluginBuilder's reload restores the parameter values a node already had, so the node keeps its Output
Bins Mode; Auto now means N/2+1 of the zero-padded FFT, and Fixed means exactly Output Bins. The new
toggles arrive with their defaults (Raw off, Zero-Padding on).

## [v2.10.0] - 2026-09-23 — lock-free real-time pipeline, presets, aggregation, spectral features

Summary (the detailed plan and measurements are in `AUDIT_AND_PLAN.md`):
- **Async handoff:** wait-free triple buffers between the cook and the worker (`AsyncAnalysis`). Worker
  Wake (Poll 2 ms / Signal) and Worker Priority (Highest / MMCSS "Pro Audio").
- **Planner:** abandoned MEASURE/PATIENT plans go to a graveyard instead of blocking a cook. PATIENT is
  time-limited to 1.5 s.
- **New parameters:** Quality Preset, Warp Aggregation (Off / Peak / RMS, only where one output bin covers
  several FFT bins), Kaiser Beta Mode (Auto designs β from the dB range), Input Ingest (Auto sample
  cursor), Spectral Features (8 Info CHOP channels).
- **UI:** parameters that the current settings make inert are greyed out (Common API 3).
- **Tests:** a steady-state allocation gate in the tests. A perf-regression gate (`ctest -L perf`,
  `bench/perf_baseline.json`).

## [v2.9.1, docs-only follow-up] - 2026-09-21 — maintainability pass: comments, structure and documentation only

Commit `71c13b8`, made after the v2.9.1 release and without a version bump, so the node still reported
v2.9.1; the next build to ship it was v2.10.0 (`8e18b41`). This entry was headed "Unreleased" until
2026-09-23.

No behaviour change of any kind. No parameter added, removed or renamed, no default changed, no
algorithm touched, no version bump. **`fft_tests`: 607 checks, 0 failures** before and after, which
is the whole of the evidence that nothing moved.

### Changed
- **A plain-language layer was added above the existing comments across the source tree.** The comments
  this project already had are dense and accurate - they explain *why* a thing is the way it is, with
  measured facts - but they assume the reader already knows what the code does. Every file now opens
  with what it is for and how to read it, and every non-obvious function and member has a short
  WHAT / WHY / HOW TO CHANGE block above the existing prose. **No existing comment was deleted**: the
  technical facts are all still there, with a plainer layer above them.
- **The files concerned:** `source/DSPModules.h`, `source/AnalysisPipeline.h`,
  `source/AnalysisPipeline.cpp`, `source/FFT.cpp`, `source/FFT.h`, `source/FftBackend.h`,
  `source/RateModel.h`, `source/Parameters.h`, `source/Parameters.cpp`, `tests/dsp_tests.cpp`,
  `bench/bench.cpp`, `bench/fftw_threads_probe.cpp`, `bench/fftw_version_probe.cpp`.
- **Every markdown file was extended as well**, and `README.md` gained two navigation sections: a table
  of which of the five documents answers which question, and a "Where to change what" map for a first
  modification. The project-layout block in `README.md` had also gone stale - it was missing
  `AnalysisPipeline.h/.cpp` and `RateModel.h` entirely, i.e. the file that holds the stage order and
  the file that holds the sample-rate arithmetic.

### Fixed (documentation only)
- **Stale facts in the surrounding prose, corrected against the source.** The `README.md` project layout
  was missing three source files (`AnalysisPipeline.h`, `AnalysisPipeline.cpp`, `RateModel.h`), one bench
  file (`fftw_version_probe.cpp`) and the two build helpers, and it described `CMakeLists.txt` as "15
  lines" (it is not, and the number was guaranteed to age badly). Claims of the form "CALLED BY ..." and "used by ..."
  in the new comments were each checked with a search before being written, and several were corrected as
  a result - notably the reason `interp` is in `WarpKey`, which is that
  `AnalysisPipeline::updateWarp()` calls `setInterpolation()` *inside* its key-guarded block, so
  removing it from the key would make the interpolation menu silently do nothing.
- **A latent maintenance hazard was found and named, which is worth recording because it will recur:**
  the comments cite each other as `file:line` ("CALLED BY: `FFT::pollParameters()` (`source/FFT.cpp:239`)"),
  and **adding comments shifts every line below them, so this pass invalidated a whole set of those
  citations at once**. The citations are being rewritten to name the symbol instead of the line
  (`FFT::pollParameters()`), because a symbol survives an edit and a line number does not. The convention
  is now stated in `README.md` under *"Where to change what"*. Nothing was wrong with the code in any of
  these cases - the reference, not the referent, had moved.
- **Three files pointed at something that is not there.** `3rdParty/fftw3/VERSION` and
  `3rdParty/fftw3/include/fftw3.h` both named `source/FftwVersion.h` as the home of the FFTW version
  pin. No such file exists anywhere in the tree. The real pin is the `expectedVersion` field of the
  `kFftw3Backend` descriptor in `source/FftBackend.h` (`"3.3.11"`), which is what the runtime mismatch
  check actually compares `fftwf_version()` against, and both files now name that instead.
  `CMakeLists.txt` had the same class of error one step further out: it said the `3.3.5 -> 3.3.11`
  comparison "in `3rdParty/fftw3/README.md`" was measured with `fft_version_probe`. That comparison
  has never been in that file - it records the FFTW3-vs-oneMKL comparison - so the comment now says
  no such A/B is kept there and to re-run the probe, rather than pointing at numbers a reader cannot
  find. All three edits are comment text only: `CMakeLists.txt` is byte-identical with its `#` lines
  stripped, all 18 `KEY=` value lines in `VERSION` are unchanged, and the `#include` in `fftw3.h` is
  untouched.
- **Two comments asserted a default and a location that were both wrong.** In `tests/dsp_tests.cpp`,
  `test_rate_model()`'s note described the default `Parameters::Values` as "WinMode=Ms(50/72),
  pad=32768". The real defaults are `WinMode::Samples` (3175 samples, which is 72 ms, so the two default
  fields describe the same window) and `kPadDefault` = 16384, i.e. index 4 of `kPadValues` - and "50/72"
  is the attack/release ms pair, not a window mode at all. In `AnalysisPipeline::rebuild()`, the comment
  justifying the backend cast cited "the static_assert in FFT.cpp"; `FFT.cpp` contains no `static_assert`
  anywhere, and the asserts that pin the two numberings are the ones after the menu tables in
  `Parameters.cpp`. Both are latent traps rather than live bugs - the test passes either way and the cast
  is correct either way, so nothing would ever have flagged them. Both are comment text only.
- **The two realtime documents described a codebase that no longer exists.** They are the oldest prose in
  the project - written as forward-looking plans and then kept as history - so much of what they name as
  current had since been renamed, removed or overtaken. Corrected against the source, with the correction
  placed beside the original claim rather than replacing it:
  - **Four commit hashes were attached to the wrong descriptions and two commits were missing** from the
    15-commit sweep that made it into the text. All six were checked individually with `git log -1`.
  - **`MKLEngine` was described as a live implementation.** It does not exist, and has not since v2.2.0,
    where it was removed as a byte-for-byte copy of `FFTWEngine`.
  - **About a dozen symbol names were dead** (`hasActiveFilter()`, `applyWindow`, `applyWeightingCurve`,
    `ChannelState::initBuffers()`, `getPlanLogHistory()`, `processChannel`, `rebuildDSP()`,
    `std::max_element`, ...). Each was replaced with the symbol that search proved is current
    (`BiquadEQ::updateAndCheckActive()`, `FFTDSP::multiplyInto()`, `PlanLog::snapshot()`,
    `python_logger::writeToTextport()`, `AnalysisPipeline::runChannel()`, `FFTDSP::findPeakWithIndex()`).
  - **Several "already in place" claims were false**, most notably an "adaptive planner (MEASURE for
    small, ESTIMATE for large)" that has never existed - what exists is the user-selected `PlannerPolicy`
    plus a background upgrade. Flagged, not quietly dropped.
  - **Numbers that had been overtaken**: the scalar-dB decision (the converter is now fully vectorized,
    16 bins/iteration), the warp (8 bins/iteration, not 4), `FastLog10`'s table (2048 entries, not 256),
    the sample-rate clamp (1 - 384000 Hz, not 1 - 192000), the default parameter reads (20 / 33, not
    19 / 31), and the v2.3.0 / v2.4.0 dates (16 days apart, not the same day).
  - **What could not be checked was labelled, not deleted**: the `FFTW_EXHAUSTIVE` "32 s" stall, the
    "16 branches" counts, every `Expected:` estimate, the GPU figures, and the hot-spot percentages are
    each now marked as an estimate or as unverified. Every §1 item in the roadmap also gained its real
    outcome - done, shipped-then-removed, or open - instead of reading as though it were still pending.

### Notes
- **One pre-existing inconsistency was found and deliberately left alone.** `PluginProjects/FFT/plugin.json`
  still declares `"version": "2.9.0"`, while the node itself reports **v2.9.1** from
  `kMajorVersion`/`kMinorVersion`/`kPatchVersion` in `FFT.cpp`, and the newest release in this file is
  v2.9.1. The two numbers are maintained by hand in different places and nothing derives one from the
  other. It was not corrected here because this pass changes no behaviour of any kind and a manifest
  version is data rather than a comment - but a reader who finds the two disagreeing should know which is
  authoritative for "which build is actually loaded": the constants are, because that is what the
  middle-click popup's `Plugin:` line and the Info DAT report. See the comment above `kMajorVersion`.
  *(Resolved since: `plugin.json` was bumped with the node from v2.11.0 on, and reads `2.12.0` now.)*
- **Nothing in this pass is a performance change, and no number in this file or in the README was
  re-measured.** The benchmark figures quoted in the entries below and in the README are the ones taken
  when those changes were made; treat them as a record, not as a current reading, and re-run
  `fft_bench` if a decision depends on one.

---

## [v2.9.1] - 2026-09-21 — the popup's length is now a constant, and the log line is gone

### Fixed
- **`kMaxPopupChars` was not a bound on the popup string.** It was named as one, documented as one ("everything
  the popup can say, including the tail, is held under 1600 characters"), and it guarded exactly one of the four
  places that append: the plan-log tail loop. The fixed body could sit at ~780 characters and the **first** tail
  line - 240 characters, because a backend description embeds the absolute path of the FFT library - could be
  appended before anything was checked, so the string could exceed the bound the code claimed to enforce. This is
  the failure mode the bound existed to prevent, sitting inside the bound's own implementation. The bound now
  applies to the whole string: every append past the identity block goes through one `add` lambda that refuses a
  line which would not fit, whole lines only, so a line of numbers can never be handed over half-written.
- **The popup's length moved by hundreds of characters from cook to cook, which is what made a length-dependent
  failure read as a random one.** The fixed body is ~780 characters; the tail was three plan-log lines of up to
  240 each, so the total swung between about 800 and 1300 depending on which plan events happened to be last - and
  the character count in the `info_callback_calls` row, which exists to be compared, could not be compared with
  anything. Each rendered line is now clipped to 72 characters (`kMaxTailLineChars`, via the new, unit-tested
  `FFTDSP::clipLine`), so the total is a constant the reader can predict. The full line is untouched in the Info
  DAT's `plan_log_*` rows and in the textport: neither is a fixed-size surface, and a clipped log line read as the
  log would be worse than a long popup.
- **`kMaxPopupChars` moved 1600 → 1200**, which is the headroom the ~1660-rendered / ~1760-empty measurement
  supports. At ~700 characters typical against a 1200 bound, the bound now never binds in a normal configuration -
  it is there for an unusual one (a very long node path, a very long install path) rather than being a ceiling the
  string routinely sits under.
- The string was also trimmed where the length was pure prose, with every number kept: `Engine & Plan:` → `Engine:`
  (`FFT: N=…, window …, … magnitude bins` and `Axis:` replace the `Spectrum axis:` sentence), `Rate:` and
  `Throughput:` are one line - they are the declared and measured form of the same quantity and were two lines of
  label for six numbers - `Output: … samples x … channel(s)` → `Output: … x … ch`, and `GPU: none (CPU/AVX2 only;
  TD reports per-node GPU time)` merged into the `SIMD:` line as `| GPU: none (CPU only)`.

### Changed
- **The popup reports a patch version** (`v2.9.1`), because it is the only place in TouchDesigner that says which
  build is answering. That matters for this surface specifically: the popup is the thing that goes blank, so when
  it comes back the first question is which DLL produced it - `Binary:` gives the path, the `Plugin:` line now
  gives the build.
- **The textport announcement line is gone**, at the user's request. It printed from *inside* `getInfoPopupString`
  (`getInfoPopupString() called (#4800, node cook #4801) - TD is reading this node's custom popup text (1218 chars
  handed over)`), which meant its absence and the popup's absence looked like one symptom. They are not: the line's
  two facts - the call count and the characters handed over - are in the `info_callback_calls` Info DAT row, where
  they are values rather than rendered text, and the question it answered ("is TouchDesigner entering this callback
  at all?") is read from row 19 by comparing the counters against `cookCount`.

### Verified
- `fft_tests`: **607 checks, 0 failures** (was 593). The new `test_clip_line` asserts the `clipLine` contract in
  both directions - a line that fits is handed through byte for byte, one character over is trimmed to exactly the
  bound and marked, and the real 240-character backend description keeps its identifying head while losing the
  library path (which is what made the line's length unbounded, and which the `Binary:` line already carries).
- `fft_bench --info 2000`: **54.6 µs/cook → 0.6 µs/cook, −99 %** (0.32 % of a 16.7 ms frame). The bench mirrors
  the clip so the one part of the "after" path the "before" path never paid for - truncating each rendered line -
  is measured rather than hidden inside the saving.
- `build/bin/Release/FFT.dll` and `__Plugins__/FFT/FFT.dll` are byte-identical (SHA-256
  `B9E0C2E5…D26EF787`), and the new popup literals (`GPU: none (CPU only)`, `--- Plan log (last `) were confirmed
  present **in the built binary** rather than assumed from the build succeeding.

### Still open, and recorded as open
The popup came back on this build before these changes were compiled, and went blank earlier on a build with **no
code change in between** - which is what rules the DLL, and the string, out as the variable that flips it. What
this release does is remove the one mechanism that has ever been *measured* for this failure (length, and length
that moved), so that the next blank popup is a clean signal instead of an ambiguous one: with the length now a
constant, `info_callback_calls` reading an ordinary character count beside a blank popup means TouchDesigner was
handed a well-formed string and did not draw it, while counters frozen against `cookCount` mean the callback never
ran. Both are read without a rebuild. The cook-driven mechanism, the two installs, and the five causes in the order
they actually occur are in *"The middle-click info popup"* in `README.md`.

---

## [v2.9.0] - 2026-09-21 — two FFT libraries in one node, and an Async toggle that really means one thread

### Added
- **`FFT Backend` (Performance page): the node plans and transforms with either the vendored FFTW3 build or
  Intel oneMKL's FFTW3 interface, switched at run time.** Neither library is *linked*: FFTW3 and oneMKL export
  the identical `fftwf_*` symbol set, so a link would freeze the choice at build time and make a runtime toggle
  impossible. `source/FftBackend.h` resolves the selected library with `LoadLibraryEx` + `GetProcAddress` and the
  engine calls through that function-pointer table; `dumpbin /dependents FFT.dll` shows no FFT library among its
  imports, which is the check that this is really dynamic. The registry is a pair of `constexpr` descriptors
  indexed by the parameter's menu value, and four `static_assert`s in `Parameters.cpp` (count, menu values,
  wisdom support, unpinned MKL version) make a third library a compile error everywhere it has to be added rather
  than a silent mismatch. A selected-but-missing library logs its reason at error level and falls back to the
  vendored FFTW3 for that node - it never leaves the node without a plan. The Info DAT row `fft_backend` and the
  plan log name the library, its path and the version it reports, so there is never a doubt about which one ran.
  Retires the `FFTW_DLL` / `__declspec(dllimport)` trap as a side effect: with `GetProcAddress`, the data symbols
  `fftwf_version` and `fftwf_cc` resolve to the real arrays instead of to an import-library jump thunk.
- **Intel oneMKL support, measured on the real library.** `mkl_rt.3.dll` (oneMKL 2026.1.0) exports 95 `fftwf_*`
  symbols directly, so the wrapper library from Intel's "Building the FFTW3 interface" notes is not needed - that
  was verified on the binary, not taken from the documentation. On the development machine (i9-13900H, AVX2, no
  AVX-512), same binary, N = 16384, only the library differing, four paired runs: **oneMKL is 13-34 % faster on
  the fft+mag stage** (8.85 µs vs 11.94 µs in the first pair), winning every pair. Two deliberate decisions came
  out of that work: `MKL_Set_Threading_Layer(MKL_THREADING_SEQUENTIAL)` is called the moment the library loads,
  because oneMKL otherwise pulls its Intel OpenMP layer (`libiomp5md.dll`) into a process where TouchDesigner has
  already loaded its own copy - the documented *"Error #15: Initializing libiomp5md.dll, but found libiomp5md.dll
  already initialized"* abort - and because the ISSL forbids modifying the DLLs, choosing a layer at load is the
  only fix a plugin has; and the planner policy and wisdom cache are reported as **inapplicable** rather than
  silently ignored, since oneMKL accepts `FFTW_MEASURE` / `PATIENT` / `WISDOM_ONLY` and does nothing with them
  (measured: no wisdom file is created, the calls return 0). The library is not redistributed with the project.
- **`Async` off is now a real single-thread contract.** The background `FFTW_MEASURE` / `FFTW_PATIENT` upgrade was
  the one piece of work that still ran off the cook thread with the worker disabled. `IFFTEngine::setBackgroundAllowed`
  is handed the toggle every cook, before the engine is polled; with Async off the upgrade is not started, the plan
  status says `FFTW_ESTIMATE (measured upgrade deferred: Async is off)` instead of promising an upgrade that is not
  coming, and the engine remembers it is owed one - so flipping Async back on starts the measurement then rather
  than stranding the node on ESTIMATE forever (`prepare()`'s early-out would otherwise never re-plan).
  `dsp_tests` asserts both halves: no background thread over 20 cooks with Async off, then the upgrade arriving
  after it is re-enabled.

### Changed
- **The vendored FFTW is now 3.3.11, built for AVX2 + FMA, and every file carries the version and SIMD level in
  its name** (`libfftw3f-3.3.11-avx2.dll` / `.lib` / `.exp` / `.pdb`, `fftw3-3.3.11-avx2.h`, `VERSION`).
  Two FFTW builds that differ only in SIMD produce identical results and identical `fftwf_version` prefixes, so
  the file name is the only thing on disk that says which one is in play - and the plugin's plan line reports the
  version the library actually returns, with a `** MISMATCH` warning when it is not the pinned one (it caught the
  3.3.10-class DLL still staged in `__Plugins__/FFT` during this work). There is no official AVX2 Windows binary
  from fftw.org - its prebuilt DLLs are 3.3.5/SSE2 - so the DLL is built from source, and the record (source URL,
  tarball MD5, DLL SHA-256, build flags, the one upstream patch) lives in `3rdParty/fftw3/VERSION`. That patch is
  needed because upstream's contributed `CMakeLists.txt` still says `set (FFTW_VERSION 3.3.10)` in the 3.3.11
  tarball; that value becomes `PACKAGE_VERSION`, which is what `fftwf_version` returns *and* the version header
  FFTW stamps into wisdom files, so an unpatched build self-reports 3.3.10 forever.
- **`fft_bench --backend fftw3|mkl`** (names, not indices) and a `backend live:` line naming the library, path and
  version - the attribution line for every number the bench prints.
- **Both binaries set stdout unbuffered.** Piped MSVC stdout is block-buffered and a crash does not flush it, so a
  crash used to swallow every line printed before it and look like a run that produced nothing. `fft_tests` and
  `fft_bench` now lose nothing, and the last line printed is the crash site. This was added after exactly that
  happened: a `STATUS_HEAP_CORRUPTION` abort with zero output that a clean rebuild then did not reproduce.
- Tests: **593 checks** (was 493) - backend selection producing the same transform to float precision on both
  libraries and surviving six switches in one engine, the OpenMP check, and the Async single-thread contract.

### Fixed
- **The middle-click info popup came up empty (regression from v2.3.0).** Reported as "it used to show a lot more
  information". It was not a popup bug at all: `getGeneralInfo` had been returning `cookEveryFrame = false` /
  `cookEveryFrameIfAsked = true` since the mono-first async rework, which the SDK defines as *"if nobody is using
  the output from the CHOP, it won't cook"*. TouchDesigner calls every information callback **inside a cook** - the
  order is documented at the top of `CHOP_CPlusPlusBase.h` (`execute()`, then `getInfoCHOPChan`, `getInfoDATEntries`,
  `getInfoPopupString`, `getWarningString`, `getErrorString`) and nothing calls them on demand from the middle-click
  itself. So an idle node had no Info CHOP, no Info DAT, no warning/error state and an empty popup, because the
  callbacks were correct but were never reached. The node now returns `cookEveryFrame = true`
  (`cookEveryFrameIfAsked = false`), which is the honest trade for a node that carries all of its telemetry through
  those callbacks: the cost is the async pipeline's cook-thread work, **11 us mean / 17 us p99** per cook at 16384
  bins (v2.4.0, `fft_bench --cook`), about 0.07 % of a 60 fps frame.
  A second consequence was worse than the empty popup: `getErrorString` is in that same chain, so a node that had
  stopped cooking could not report a hard failure - a plan that would not build, an input with no usable sample
  rate - and would just go quiet instead of showing an error badge. Both are fixed by the same flag.
  `OP_CustomOPInfo::cookOnStart` is also set now, with the caveat recorded in the source: the SDK honours it only
  for a **Custom Operator**, not for a `.dll` loaded into the built-in CPlusPlus CHOP - which is how PluginBuilder
  hosts this plugin (`plugin_loader` is a `cplusplusCHOP`). There, `cookEveryFrame` is the flag that does the work.
- **The popup now names the node and the binary that answered it.** It reports the node path, the registered op
  type, the plugin version, and `OP_NodeInfo::pluginPath` - the full path of the loaded `FFT.dll`. That last one is
  not decoration: the plugin can be loaded twice in one TouchDesigner process, once as the registered Custom
  Operator under `Documents/Derivative/Plugins/FFT` and once by PluginBuilder from `<project>/__Plugins__/FFT`, and
  each instance resolves its FFTW3/oneMKL libraries from **its own** directory. A log that names one path is
  therefore not evidence about the other instance, and the popup is where that ambiguity gets settled. The cook
  count (`OP_NodeInfo::cookCount`) is printed alongside, because it is the node's own proof that the cook-driven
  telemetry on that popup is live.
- **Reported version was `v2.8` while the changelog documented v2.9.0** - `kMinorVersion` was never bumped for the
  v2.9.0 work, so the popup under-reported its own version. Now 2.9.
- **How the popup fix was confirmed, and what confirming it cost in log noise.** TouchDesigner's middle-click does
  not call the plugin: the popup shows whatever the last cook left behind. So an empty popup has two opposite
  causes - TD never entered the info callbacks, or it entered them and the string did not render - and nothing in
  the plugin could tell them apart. Three counters (`myInfoPopupCalls`, `myInfoDatSizeCalls`,
  `myInfoChopChansCalls`) were added, plus a line to the textport on the first entry. That settled it: the counters
  advance **in lockstep with `OP_NodeInfo::cookCount`, one popup entry per cook** - popup #2100 at cook #2101,
  #2400 at #2401, #2700 at #2701 - so TouchDesigner was calling the chain every frame. The counters stay, because
  they are the only signal that separates "the node stopped cooking" from "the popup string broke"; they are now
  reported in the `info_callback_calls` Info DAT row (row 19, ahead of the plan log), together with the popup's last
  character count and the largest cook gap, so the diagnostic needs no textport and - the point of moving it - no
  presence in the popup string at all.
- **The popup string no longer carries diagnostics of its own, and the instrumentation line is gone entirely.**
  Two changes made in `7b419b5` are undone here, together, because they landed together and both reverted cleanly.
  It had cut the entry line from calls 1, 2 and every 300th down to **once per load**, and it had added two lines to
  the popup text itself - `Info callbacks entered: popup N, Info CHOP N, Info DAT N` and `Cook stall: largest gap
  between cooks ... ms`. The popup went empty after that commit and came back when both were reverted, so the
  string content is where the break was; **which of the two changes caused it is not separated**, and does not need
  to be, because the durable rule is now this: a diagnostic must never be able to change what it measures. Both
  values live in the `info_callback_calls` Info DAT row, which TouchDesigner reads as a value rather than rendering
  as the popup, so whatever the popup does with its text cannot move that number.
  The textport line was restored to one-per-300 cooks in this same release, and then **removed outright** once its
  two facts (the call count and the character count handed over) were readable from that same Info DAT row: with
  the row carrying both, a print on the real-time info path buys nothing and costs a buffered string per
  announcement. The question it existed to answer - "is TouchDesigner entering this callback at all?" - is now
  answered by reading row 19: counters that track `cookCount` 1:1 mean the chain is entered, counters frozen while
  the node visibly cooks mean it is not.
- **A corrected claim, and what it means for the bullets above.** The bullet above records that "the empty popup
  really was the cooking flag". That conclusion was drawn from the counters and it was too strong: the counters
  prove TouchDesigner *enters* the info chain every cook, which is not the same as proving the popup rendered,
  and the popup went empty again afterwards with the flag unchanged. The flag fix is still correct and still
  needed (`cookEveryFrame = false` genuinely was a bug), but it is not established as *the* popup cause. The
  bullets below are the same class of mistake being avoided: they record what was measured, and mark separately
  what has not been confirmed in TouchDesigner.
- **Two error states were latched and could never clear (real defect, independent of the popup).** `myErrorText`
  was set by the `execute()` catch blocks and cleared **only** when the user happened to pulse `Reset`, and the
  error string was driven off the lifetime counter `myPipelineErrors`, which is never cleared at all. So a single
  transient exception - which is exactly what a plugin hot-swap during development produces - left a node that was
  cooking correctly flagged as broken permanently. This matters for the popup specifically because TouchDesigner
  reports a node's error state in place of the operator's own information, which is one of the few things that can
  empty a middle-click popup while every callback behind it runs normally. Both now clear on the condition they
  describe: `myErrorText` is cleared by the first cook that completes, and the error string is gated on a new
  `myPipelineFailing` flag that is set on a throw and **cleared when an analysis publishes a result**. The lifetime
  count is kept and reported as the history it is. A fault that recurs re-latches on the next cook, so nothing is
  hidden. Tested: 593 checks, 0 failures.
- **`getInfoPopupString` publishes its text once, at the end, on every path.** It had been changed to publish a
  four-line identity block first and then publish the full text again, so `setString` ran twice per call. The
  single-call form - build the whole string, set it once - is the form that was observed rendering the complete
  popup in this harness, so it is restored. The safety property the two-call order was reaching for is kept without
  a second call: the identity block is in `text` before the `try`, and the catches **append** a note rather than
  replacing it, so this final `setString` always hands TouchDesigner a populated popup. **Confirmed in
  TouchDesigner** - the complete popup was watched rendering on this form, at a stable 1660 characters, with the
  callback entered on every cook; see the section below.
- **The Info DAT declared a row count it might not fill.** `getInfoDATSize` computed `rows` from `myLog.size()` and
  `getInfoDATEntries` walked `myLog` again. `PlanLog::log()` truncates the history by half at
  `kMaxPlanLogEntries` (256), so a plan event logged by the worker between the two calls made the log *shorter*
  after the size was declared, leaving TouchDesigner walking rows the plugin never wrote. The log is now frozen
  into `myInfoDatLog` at size time and the walk reads only that copy - the same view for both calls, which is what
  the API requires.
- **Cook-stall telemetry, because a stopped cook is invisible from outside.** Every signal this node produces is
  emitted from inside a cook, so when cooking stops they all go quiet together and the node simply looks blank
  with nothing anywhere saying why. `executeImpl` now records the largest gap between two consecutive cooks
  (`myMaxCookGapMs`, two clock reads per cook, no allocation) and the popup and the `info_callback_calls` Info DAT
  row report it: a value near 16.7 ms is one 60 fps frame, and a value in seconds says the node stopped cooking
  for that long and its information output was blank while it lasted.

- **The info chain cost 60.7 µs of every cook, and now costs 0.4 µs - measured, not estimated.** This is the
  answer to the other half of the popup report: not "the popup is empty" but *"it sometimes takes time to show up,
  or takes several middle-clicks"*, with the DLL loaded and the FFT visibly running. The callbacks are called
  **inside a cook**, so their cost lands on the cook thread, and TouchDesigner drives them one item at a time: 21
  `getInfoCHOPChan` calls, 276 `getInfoDATEntries` calls, and the popup. Each of those asked for the same
  `AnalysisPipeline::Status`, and the answer was a mutex lock plus a **by-value copy** of a struct holding two
  `std::string`s - so ~300 lock acquisitions and ~850 heap allocations per frame, on the real-time thread, against
  the *same* mutexes the audio worker takes when it logs a plan event. Separately, `getInfoDATSize` copied the whole
  256-entry plan log to declare its row count and the popup copied it again to render a three-line tail: ~512 string
  copies per frame. That is what a middle-click query walked, and it is why the delay was worse around a plan
  rebuild than in the steady state. It also contradicted the "no allocation on the cook thread after warm-up"
  invariant the rest of the plugin is built around. Three changes, all of them restoring a guarantee the code
  already claimed:
  - **`statusSnapshot()` is memoized behind a version stamp** (`myStatusPubVersion` / `myStatusReadVersion`). The
    published copy only moves when the worker rebuilds a plan or the tables - the write side was *already* gated
    that way; only the read side re-copied. The steady state is one acquire load and a returned reference. It now
    returns a reference, so callers bind by reference: a by-value return would have put both string copies straight
    back on all ~300 calls.
  - **`PlanLog::version()` and `PlanLog::snapshotTail(n, out)`.** `version()` is bumped under the log's own mutex on
    every mutation, so a reader can tell "the log did not change" without copying it to find out. `snapshotTail`
    copies only the entries asked for, into a reused scratch vector, so the popup's tail costs no allocation once
    warm. `getInfoDATSize` now re-takes its frozen view **only when the version moved**, which is the same guarantee
    it always had - the copy is still frozen for the whole row walk - but is an integer compare in the steady state.
  - **A `--info N` mode in `fft_bench` so the number can be re-taken rather than re-argued.** It drives the same
    access pattern against the real `PlanLog` (the status struct is modelled locally, since the real one is reached
    through `FFT.h` and the TouchDesigner SDK headers), and prints before/after per cook:

    ```
    info-callback access cost (298 status reads + the 276-row log view, per cook, 2000 cooks):
      re-lock + re-copy every call       60.7 us/cook
      memoized + version-stamped          0.4 us/cook   -99%
      saved                              60.3 us/cook   (0.36% of a 16.7 ms frame at 60 fps)
    ```

    For scale, the analysis itself is ~88 µs/cook for one channel at the default settings, so the bookkeeping for
    reading the node's own status was costing about as much as the DSP it reports on. **What this does not claim:**
    that TouchDesigner's renderer was timing out. Nothing inside a plugin can observe that. What is established is
    that a large, measured, allocation-heavy cost was removed from the path a middle-click query walks.
  - Tests: **593 checks** (was 547) - `PlanLog` version/tail coverage, including the property that matters most for
    the popup: for every `n`, `snapshotTail(n)` returns exactly the last `n` entries `snapshot()` would have, at the
    history cap as well, so the displayed lines cannot silently change.
- **The popup string was shortened, its numbers now read properly, and its length is bounded.** Three separate
  things, all in the same string:
  - **Timing figures were printed with six decimals.** `std::to_string(double)` is `%f`, so a 13 µs cook time
    read `Cook: 13.000000 us CPU (params 10.700000 us) | DSP: 124.300000 us` - six digits of noise on a
    microsecond figure. Now `%.1f`: `Cook: 13.0 us CPU (params 10.7 us) | DSP: 124.3 us`.
  - **The cook count no longer ends the Cook line.** Proving the node is cooking is the identity block's job, and
    the count is in the `info_callback_calls` Info DAT row, where reading it costs the popup nothing. Wordy labels
    were trimmed throughout the same pass (`FFT Size:` → `FFT:`, `magnitude bins computed` → `magnitude bins`,
    `Sample rate (to TouchDesigner)` → `Rate:`, `Measured throughput` → `Throughput:`, `frames/s` → `fps`, and the
    `Output:` line's window length, which the FFT line above it already reports). Every number is still there.
  - **The string is now bounded at 1600 characters, with at most 3 plan-log lines.** The body is a fixed set of
    numbers - only the node path and the plugin path vary - but a plan-log entry is arbitrary text, so the tail was
    the one unbounded input. The bound is enforced where that input enters: the body is budgeted first and the tail
    fills only while the total fits. `kMaxPopupChars` is set **under the shortest length known to have failed**
    (~1760, measured) rather than from a theory of the real limit, because none is documented. Measured effect:
    ~1660 characters before this change, roughly **1100-1300** now, with the ceiling no longer reachable by a long
    log line. The actual length is reported in the `info_callback_calls` Info DAT row, so if the bound is ever the
    thing that is wrong, it says so rather than being silent.
    **Corrected in v2.9.1: this bound did not do what the paragraphs above say it did.** It was implemented on the
    tail loop only, so the fixed body plus the first tail line could be handed over before the total was compared
    against it - so "the ceiling is no longer reachable by a long log line" was not true when it was written. The
    bound is enforced on the whole string from v2.9.1, and the number is 1200. Left as written rather than rewritten,
    because that release's measurement (~1660 rendered, ~1760 empty) is still the evidence the bound is sized from.

### Confirmed in TouchDesigner, and what that confirmation cost
The middle-click popup was watched rendering **in full** from this plugin, in TouchDesigner, on the build this
entry describes. The evidence is the textport line, and it settles two questions at once:

```
[FFT Plugin] getInfoPopupString() called (#4200, node cook #4201) - TD is reading this node's custom popup text (1662 chars handed over)
[FFT Plugin] getInfoPopupString() called (#4500, node cook #4501) - TD is reading this node's custom popup text (1660 chars handed over)
[FFT Plugin] getInfoPopupString() called (#4800, node cook #4801) - TD is reading this node's custom popup text (1660 chars handed over)
```

- The popup call count tracks `OP_NodeInfo::cookCount` exactly one behind, so TouchDesigner enters the callback on
  **every cook**. The chain was never the problem.
- The length is **1660-1662 characters and stable**, so the text was never outgrowing anything.
- What it took to get here is the subject of the two bullets above, and the reason the popup string now carries no
  diagnostics at all: two lines added to it (`Info callbacks entered: ...`, `Cook stall: ...`) took it to roughly
  1760 characters and the popup came up **empty**, and removing them brought it back. Reproduced in both
  directions; the mechanism is **not** established (the SDK documents no cap on `OP_String::setString`), which is
  exactly why the guard is a rule rather than an explanation - see *"The middle-click info popup"* in `README.md`.

**Still open, and recorded as open.** The same build that rendered the popup at `#4800` later reported blank with
**no code change in between**. That single fact rules out the DLL, and the string, as the variable: the popup is a
snapshot of the last cook, so a node that has stopped cooking has no information surface at all - and the spectrum
it last produced holds on screen and looks healthy, which is why "the plugin still works but the popup is empty" is
the expected symptom of that failure, not a contradiction. The counters and the character count exist so the next
report can say which branch it is without a rebuild: counters frozen with the log line gone means the chain is not
being entered (fix the cook), and counters climbing with a healthy length on a blank popup means a real string was
handed over and not drawn (not a plugin problem). See *"Making sure the node is cooking"* in `README.md` for the
five causes in the order they actually occur, including the two that make this look intermittent - a node running a
DLL that is not the one just built, and the separate `Documents/Derivative/Plugins/FFT` install that only the
**Install Plugin** pulse updates.

### Notes
- `fftwf_cleanup` and `fftwf_forget_wisdom` are deliberately **not** in the resolved API table. They free
  process-global state - every plan and every cached trigonometric table - so there is no such thing as cleaning
  up one node's FFTW state in a host that can hold several FFT CHOPs.
- FFTW's planner is serialised under one process-wide mutex, on both the cook thread and the background
  measurement thread, including `destroy_plan` and the wisdom import/export. That is FFTW's own recommended
  pattern: the manual states `fftw_execute` (and the new-array variants) are the only thread-safe routines and
  that everything else "should only be called from one thread at a time" because planner calls share wisdom and
  trigonometric tables. `fftwf_make_planner_thread_safe` is not used - the manual calls it "the worst of all
  worlds" and the engine's own lock already does its job with the project's priority ordering.

---

## [v2.8.1] - 2026-09-14

### Changed
- **`AnalysisPipeline` extracted into a TD-free translation unit.** `AnalysisJob`, `AnalysisResult`,
  and the full `AnalysisPipeline` class (window → FFT → magnitude → warp → weighting → dB → ballistics)
  now live in `source/AnalysisPipeline.h` / `source/AnalysisPipeline.cpp`. `FFT.h` now `#include`s the
  header; `FFT.cpp` no longer contains the pipeline implementation. The extraction makes the pipeline
  unit-testable without a worker thread or the TouchDesigner operator — `dsp_tests.cpp` drives
  `AnalysisPipeline::process()` directly: 1 kHz sine → peak verified at ~1 kHz, 3-channel parallel fan-out
  bin-for-bin identical to serial, silence short-circuit yields all-zero output. 493/493 checks green.
- **Default zero-pad length reduced 32768 → 16384.** FFT stage roughly halves (≈ 34 µs → ≈ 18 µs at
  44.1 kHz); the warp resamples 8193 → 16384 linear bins to the 16384 output bins. Per §3.3 of the review
  doc this is a 2–4× FFT-time cut with no visible loss for a 16384-bin log display.

---

## [v2.8.0] - 2026-09-12 — fewer knobs, same features: withdraw `FFT Threads`, the parallel-channel knobs and `Raw Linear Bins`

The multi-threading added earlier today was built on FFTW's own threads, and it was the wrong lever: FFTW
parallelizes the `howmany` loop of a `plan_many`, never the inside of a single 1-D transform, so it did nothing
for the default `Mono Mix` — and changing its menu rebuilt the plan under the process-wide FFTW planner lock,
which froze the node. It is gone.

The v2.2.0 `Parallel Channels` / `Parallel Min Channels` parameters that briefly replaced it are gone too. This
node is **one mono channel per instance**: several channels means several nodes, each with its own analysis worker
thread, so a per-channel fan-out knob can never fire in the intended use. `Performance` now holds exactly one
control, `Async`.

`Raw Linear Bins` went the same way for a different reason: it was a second spelling of settings that already
exist. Nothing it did is lost.

### Removed (breaking: existing `.toe` files lose these parameters)
- **`Performance > FFT Threads`** (menu Auto / 1 / 2 / 4 / 8), the `FFTWEngine::prepareBatch` /
  `executeBatchRFFT` batched-`plan_many` path, its `m_batch_*` buffers, `AnalysisPipeline::setThreads` /
  `refreshBatchPlan` / `resolveThreads`, and `FFTWEngine::initFftwThreads`. Selecting a thread count destroyed and
  rebuilt an FFTW plan on the pipeline thread while `fftwf_plan_with_nthreads` mutated **global** FFTW state — the
  access violation that took TouchDesigner down when the menu changed.
- **`Performance > Parallel Channels`** and **`Parallel Min Channels`**: dead reads in the mono-per-instance
  design, and the same category of knob as the two removed in v2.7.0. The fan-out they controlled is not a
  parameter any more — see below.
- **`Spectrum > Raw Linear Bins (no resampling)`**: it only skipped the frequency warp, and the warp already
  detects the identity case and `memcpy`s the linear magnitude through. It was therefore reachable from the
  sliders all along — `Scale = Linear`, `Warp Blend = 0`, `Display Max >= Nyquist`, `Output Bins = fft_size/2+1`
  — so the toggle gave two spellings of one setting the chance to disagree. `Scale`, `Display Max` and
  `Output Bins` still describe the grid; nothing about the feature was dropped.

### Changed
- `AnalysisPipeline::process` runs its channel loop under `std::execution::par` whenever the job has **more than
  one** channel, and serially otherwise. With one channel the branch is a single integer compare; the fan-out
  exists only for `Channels = All Channels`, the one mode that still produces several transforms per cook. Each
  channel owns its `DspState` (padded frame, magnitude, scratch, ballistics history); the only shared state is
  read-only. No plan is rebuilt and no FFTW state is touched, so nothing here can stall a cook.
- Info CHOP `raw_linear` (17) is now **`linear_grid`**, and it no longer reads a parameter: it reports what the
  built warp tables actually are (`PerceptualWarping::isIdentity`), which is the same flag `applyWarp()` branches
  on, so the Info row cannot claim a bypass the pointer loop did not take. The Info DAT and popup strings that
  mentioned a "raw" mode now say "linear grid / identity warp".
- `Display Max Hz` slider max raised **48000 → 192000** and `Output Bins` slider max **32768 → 65536**, so the
  linear grid is reachable from the UI at every accepted input rate (up to 384 kHz) and every pad size (up to
  64K → 32769 bins). Previously the sliders could not express it, which is what made the removed toggle look
  necessary.
- `outputBinCountFrom()` is now a function of the parameter snapshot alone, and `outputAxisRate()` /
  `hzPerSample()` / `outputSampleRate()` / `outputBandwidth()` lost the arguments that only existed to serve the
  old raw path. `outputSampleRate()` takes no input rate at all — passing the audio rate into it is what used to
  make it wrong.
- Info CHOP `channel_fanout` (20, was `parallel_active`) and Info DAT `channel_fanout` (was `parallel_channels`)
  report whether the fan-out ran — in normal mono use it is off, which is the correct reading, not a fault. Renamed
  so nothing in the operator's interface is still spelled after a parameter that no longer exists.

### Measured
- `fft_bench` reports the fan-out directly (same per-channel pipeline, serial vs `par`, outputs compared
  bin-for-bin first so a speedup cannot come from skipping work), N = 32768, 16384 bins, `Planner Fast`:

  | channels | serial | `std::execution::par` | |
  |---|---|---|---|
  | 2 | 252.9 µs/cook | 156.1 µs/cook | **−38 %** |
  | 4 | 528.0 µs/cook | 186.3 µs/cook | −65 % |
  | 8 | 1050.1 µs/cook | 252.9 µs/cook | −76 % |

  Outputs identical bin for bin in every case. This only ever applies to `Channels = All Channels`.
- `bench/fftw_threads_probe` keeps the evidence for why the channel loop and not FFTW's threads: a single 1-D R2C
  transform is **13–51 % slower** with `nthreads > 1` under both `FFTW_ESTIMATE` and `FFTW_MEASURE`.

### Notes
- `Mono Mix` (the default, and the design target) issues one transform per cook and stays serial — not by
  policy but because there is nothing to split. Running several mono channels is several node instances, each
  with its own `FFT Custom CHOP analysis` worker, and that already spreads across cores with no knob at all.
- The two "every N cooks" parameters stay removed and `Parameters::eval` still runs on every cook, so a parameter
  change lands on the next frame. (`eval` does skip the blocks that a section toggle has switched off — EQ,
  ballistics, and the dB options when no dB mode is active — but that is a function of the toggle, not of the
  cook count.)

---

## [v2.7.0] - 2026-09-12 — remove the two "every N cooks" parameters; nothing runs below normal priority

### Removed (breaking: existing `.toe` files lose these parameters)
- **`Performance > Update Every N Cooks`** (was default 1). It skipped the whole job publish on N−1 cooks out of N
  so the spectrum only refreshed every N frames. That saving was already banked in v2.3: the FFT runs on the worker
  and the cook thread pays only the ingest (≈ 2 µs) and the result copy (≈ 2–7 µs), neither of which the divider
  touched. Net effect was a strictly worse node — half the spectrum update rate for a cook-thread saving that the
  async architecture had already made irrelevant. Every cook now publishes a job.
- **`Performance > Parameter Poll Every N Cooks`** (was default 1). It skipped `Parameters::eval` on N−1 cooks,
  saving ~18 `getPar*` calls (~20 µs, host-side). The price was UI latency: a parameter change took up to N frames
  to appear. Every parameter is now read on every cook, so a change lands on the next frame — and the ~20 µs is
  ~0.1 % of a 60 fps frame.
- `Parameters::Values::updateEvery` / `paramPoll` and their name/label constants are gone. `outputBandwidth()` no
  longer divides by the divider, and the `output_bandwidth_sps` Info channel and popup line lost their
  "Update Every N Cooks" suffix. `pollParameters()` is now an unconditional `eval()`.

### Changed
- **Nothing in the plugin runs below normal priority any more.** The FFTW background planner thread moved from
  `THREAD_PRIORITY_BELOW_NORMAL` to `THREAD_PRIORITY_ABOVE_NORMAL`, and both threads are now named for the
  debugger (`FFT Custom CHOP analysis`, `FFT background planner`). The planner still sits one notch under the
  analysis worker (`THREAD_PRIORITY_HIGHEST`), so a cook always wins the core back from it; the point of the raise
  is that a ~3 s `FFTW_PATIENT` measurement now completes in its budget under load instead of stretching out.

### Measured, and rejected
- **FFTW's built-in threading does not help this node, and would hurt it.** `fftwf_init_threads` /
  `fftwf_plan_with_nthreads` *are* exported by the vendored `libfftw3f-3.dll` (checked the `.def`, the import
  library and the export table), so this was worth testing rather than assuming. Measured with the plugin's
  zero-padding (N = 32768, 3175 nonzeros) via `bench/fftw_threads_probe.cpp` (new, wired as
  `fft_threads_probe.exe` so the claim is reproducible) — nthreads 1 / 2 / 4 / 6 → **68.8 / 85.3 / 81.7 /
  102.5 µs**, i.e. it gets *slower*: FFTW parallelizes the `howmany` loop (and multi-dimensional transforms),
  not the inside of a lone 1-D transform, so the threads only add hand-off cost. The same probe shows where it
  *would* pay — a `howmany = 4` plan drops from 86.3 µs to 29.9 µs per transform (−65 %) — i.e. it is a real
  lever for `Channels = All Channels`, but it needs a batched `fftwf_plan_many_dft_r2c` plan, which the engine
  does not currently build. Not implemented; the default `Mono Mix` is one transform per cook and cannot use it
  at all. Re-run twice on a machine with TouchDesigner running: absolute numbers drift ~15 %, every delta's
  sign reproduced.

### Tests
- 432 checks, 0 failures, unchanged — neither the divider nor the poll interval was ever exercised by the headless
  suite (both lived in `FFT::executeImpl` / `pollParameters`, which need an `OP_Inputs`).

---

## [v2.6.0] - 2026-09-12 — `sample_rate = output bins × me.time.rate`


Requested change, overruling the v2.5.0 axis model. The reported sample rate is now the rate of the output frames
**concatenated**: one vector of `output bins` samples is produced every `1/me.time.rate` seconds, so the node emits
`bins × me.time.rate` samples/s — **983 040** at the defaults (16384 bins, 60 fps).

### Changed
- **`info->sampleRate = output bins × me.time.rate`** (FFT.cpp `FFT::outputSampleRate`). `me.time.rate` is read from
  `OP_TimeInfo::rate` in `getOutputInfo` — the timeline rate **where the node lives**, so a component with Component
  Time reports that component's rate, not the root's — and is re-read every cook, so an FPS change lands on the next
  frame.
- **The frequency axis moved to its own function**, `FFT::outputAxisRate` (was v2.5.0's `outputSampleRate`): still
  `2 × (top of the axis)` — `sr_in` for raw/full-band grids, `2 × Display Max` when the band is clipped — and still
  independent of `Output Bins`. `hz_per_sample` and the `output_spectrum_axis` row are now derived from it, so the
  bin-index-to-Hz mapping survives the change intact: **`hz_per_sample` and `output_spectrum_axis` are what convert a
  bin index to Hz now, not the sample rate.**

### Notes
- Consequence, stated plainly: the sample rate no longer says anything about what the bins mean. A 16384-bin vector
  describing 0…22.05 kHz and a 16384-bin vector describing 0…10 kHz both report 983 040 Hz at 60 fps. Use
  `hz_per_sample` for frequency.
- `output_bandwidth_sps` (v2.5.0) is unchanged and still the *measured* figure — the same number as the sample rate,
  computed from the cook delta actually observed rather than from `me.time.rate`.
- `Raw Linear Bins` stays **off** by default (user decision).
- Info DAT `output_sample_rate` now reads `983040 samples/s (16384 bins x 60.00 frames/s, warped grid, resampled)`;
  the middle-click popup prints the sample rate, the measured throughput and the cook delta it was measured over.

### Tests
- 432 checks, 0 failures. The `test_raw_linear_and_rate` assertions were **relabelled, not weakened** — they test the
  pipeline's grid (`2 × targetHz()[last]`), which is the axis and is unchanged; the local `reported_rate` is now
  `axis_rate` so the file does not imply the CHOP reports that as `info->sampleRate`.
- `FFT::outputSampleRate` itself is **not** covered headlessly: `getOutputInfo` needs an `OP_Inputs`, so this path is
  verified in TouchDesigner — middle-click the node and read the `Sample rate (to TouchDesigner)` line, or the
  Info CHOP `output_sample_rate` channel.

---

## [v2.5.0] - 2026-09-12 — raw linear bins, and the spectrum's sample rate told properly

The spectrum's `sampleRate` was the input rate, always. That told TouchDesigner the axis reached `sr_in/2` Hz no
matter what the node actually emitted — wrong as soon as `Display Max` clips the band below Nyquist, and it left the
`Output Bins` resampling undocumented. Both are fixed, and there is now a way to skip the resampling entirely.

### Added
- **`Spectrum > Raw Linear Bins`** (toggle, default **off**): skip the warp and output the linear FFT grid itself —
  all `fft_size/2 + 1` bins, DC to Nyquist, copied bit-for-bit (`memcpy`, no interpolation, no rounding, no
  reassociation). `Scale`, `Display Max`, `Warp Blend`, `Output Bins` and `Log Floor` are inert while it is on;
  `raw_linear` in the Info CHOP reports whether the current spectrum is raw. At Zero-Pad 32768 this doubles the
  output bin count versus a 16384-bin warped grid (16385 vs 16384 — the whole FFT grid, not a subset of it).
- **Info CHOP `hz_per_sample`** (18): the Hz between consecutive bins, `rate/(2*(bins-1))` — the same channel name
  TouchDesigner's Audio Spectrum CHOP uses, because the sample rate alone cannot carry the index-to-Hz mapping once
  a grid is resampled. Exact for raw linear / Linear / blend = 0 grids; the mean spacing for a perceptual one.
- **Info CHOP `output_bandwidth_sps`** (19): data throughput, `bins x frames/s` (983 040 samples/s at 16384 bins
  and 60 fps) — the number to size buffers/GPU uploads with, and the rate of a
  signal made by concatenating frames. Deliberately *not* `info->sampleRate`: that would break bin-to-Hz by the
  frame rate and make the reported rate move with the project frame rate for a byte-identical spectrum.
  Info CHOP is now 20 channels, Info DAT 18 fixed rows + plan log; the middle-click popup prints both numbers.

### Fixed
- **Output sample rate is now derived from the grid that is actually emitted**: `sr_out = 2 × (top of the axis)`,
  i.e. the last bin sits on Nyquist. Raw linear and any full-band grid report `sr_in` exactly (44100 in → 44100
  out, bin *i* at `i*sr_in/fft_size`); a band clipped to `Display Max = 10000` reports 20000 Hz instead of 44100.
  Previously the rate was `sr_in` unconditionally, so a 10 kHz view claimed to reach 22.05 kHz. The rate no longer
  depends on `Output Bins` — resampling changes how a band is described, never how wide it is. The pipeline
  publishes the exact value from the tables it built (`AnalysisPipeline::updateWarp`), with the same formula as the
  fallback for the cooks before the first result lands, so the reported rate never jumps.
- **Bark scale folded over above ~6.5 kHz.** `hzToBark` maps `z' = 1.22z - 4.422` above 20.1 Bark but `barkToHz`
  undid it by dividing by 0.78 — the inverse of a different line, agreeing only at `z = 20.1`. Past 6.5 kHz the
  target axis was non-monotonic and its **top bin landed back at 0 Hz** at exactly the 44.1 kHz Nyquist
  (fmax = 22050 → 0.0). The inverse is now `(z + 4.422)/1.22`; forward/inverse round-trips to 1e-6 Hz from 0 to
  22050 Hz, and every scale's axis is monotonic and ends exactly on `fmax`.
- Info DAT's `output_spectrum_axis` and the middle-click popup printed bin spacing as `rate/bins`; it is
  `rate / (2*(bins-1))` (Nyquist over `bins-1` intervals). `output_sample_rate` and `raw_linear` rows added, and
  `sample_rate` was renamed `input_sample_rate` for clarity.

### Tests
- 432 checks, 0 failures (was 375). New `test_raw_linear_and_rate`: raw grid is a bit-exact memcpy and its grid is
  `i*sr/fft_size`; `sr_out == sr_in` for the raw and full-band grids including every scale; `sr_out == 2*Display
  Max` for a clipped axis at 257 / 1000 / 4000 bins alike; every scale monotonic ending on `fmax`; Bark
  forward/inverse round trip over 0…22050 Hz; end-to-end sine at bin 100 of a 1024-point FFT reading back at
  `100*44100/1024 = 4306.64 Hz`.

---

## [v2.4.0] - 2026-09-10 — cook thread: no locks, no kernel calls, nothing that is not a copy

Everything in this release is about the ~16 µs the operator spends on TouchDesigner's cook thread each frame with
Async on; the DSP itself (worker thread) is unchanged. All numbers from `fft_bench.exe --cook 300` (new), which
simulates the exact cook path at 60 fps with the caches evicted between cooks, and from scratch micro-benchmarks.

### Real-time fixes (the cook thread could block or do hidden work)
- **Lock-free job and result handoff** (`FFTDSP::TripleBuffer`, wait-free single-producer/single-consumer triple
  buffer, one atomic exchange per side, "latest wins", never torn). v2.3 used a mutex-guarded mailbox and a
  mutex-guarded result double buffer; the worker held the result mutex while building telemetry **strings**
  (`std::string` allocation, 128 KB `targetHz` copy on table changes), so a descheduled worker could stall the cook
  thread for milliseconds. The cook thread now takes no mutex at all in async mode.
- **No kernel wake-up per cook.** Measured on this machine: unblocking a sleeping worker costs the *signalling* thread
  4–5 µs median and 16–18 µs p99 with every Win32 primitive (WaitOnAddress, SetEvent, semaphore, condvar). The worker
  now polls the job slot every 2 ms on a **high-resolution waitable timer** (independent of the 15.6 ms system clock
  tick; a plain timed wait made it miss frames) and goes dormant after 500 ms without jobs; the cook only signals a
  dormant worker (Dekker handshake, no lost job). Pickup latency 1.2 ms median / 2.8 ms max, 0 drops, 0 holds.
- **Peak search moved to the worker** (it owns the Hz table); the cook thread copies two floats instead of scanning
  the spectrum and copying the table.
- **Parameter reads**: `Parameters::eval()` now runs from `getOutputInfo()` (which precedes every `execute()`), so the
  separate per-cook `readBins` / `readChanMode` / `readParamPoll` fetches are gone and the poll interval is honoured by
  *every* read. The dead `Parallel Channels` / `Parallel Min Channels` parameters (their thread pool was replaced by
  the worker in v2.3, the reads remained) are removed. Default configuration: 18 `getPar*` calls per poll, **0** on
  non-poll cooks (was 19 + 3 every cook).
- Textport log flush checks an atomic flag instead of taking the log mutex every cook.
- Worker thread: `THREAD_PRIORITY_ABOVE_NORMAL`, named `FFT Custom CHOP analysis` (Visual Studio / Process Explorer).
- FFTW wisdom import is now `std::call_once` under the planner mutex (two instances could race on plain statics and
  run the non-thread-safe import concurrently with a planner).

### Measured (cook thread, Async on, stereo mono-mix, 60 fps, caches evicted between cooks)
| configuration | v2.3 (kernel wake) | v2.4 | of which |
|---|---|---|---|
| 1 ch, 16384 bins, N = 32768 (default) | 14.4 µs mean / 30 µs p99 | **11 µs mean / 17 µs p99** | ingest 2.3, snapshot 1.9, output copy 6.8 |
| 1 ch, 4096 bins, N = 16384 cubic | 10.1 / 24 | **7 µs / 11.5 µs** | output copy 2.0 |
| 8 ch (All Channels), 16384 bins | – | 75 µs | output copy 57 (8 × 64 KB cross-core) |
| Async off (inline), default | 91–106 µs | same | FFT on a cold cache is 2× the hot-loop number |

What remains is memory traffic proportional to `Output Bins × channels` (the 64 KB result written by the worker's core
and read by the cook's core, then written into TouchDesigner's buffer); `Output Bins` is the lever.

### FFT Planner = Patient (new menu entry)
- Same non-blocking scheme as Auto (instant estimate plan, upgrade measured on a background thread, swapped in
  on the next analysis, saved to wisdom) but with `FFTW_PATIENT`: 27.2 µs vs 31.1 µs per 32768-point FFT (−12 %)
  for ~2.7 s of planning, once per size per machine. The planner thread runs on a low-priority background thread
  and never touches a TouchDesigner thread (raised from below-normal to above-normal in v2.7.0 — see that entry). Verified: patient wisdom also satisfies Auto's lookup (so Auto gets the faster
  plan afterwards), measured wisdom does not satisfy a patient lookup (so Patient really measures). Caveat: FFTW's
  planner is process-wide, so a size change or a second instance planning during those seconds waits for it.

### Not changed, measured and rejected
- `FFTW_DESTROY_INPUT` (+ the 118 KB re-zero it forces): 34.1 µs vs 31.1 µs — slower.

### Tests / bench
- `TripleBuffer` / `WorkerSignal` tests (single-thread semantics, 2-thread torn-read stress ≈ 1 M publishes,
  high-resolution timeout); 370 checks.
- `fft_bench.exe --cook N`: cook-thread simulation (async and inline) with per-phase breakdown and pickup latency.

---

## [v2.3.0] - 2026-08-26 — mono-first real-time architecture

Implements the roadmap in `FFT_REALTIME_PERFORMANCE_ROADMAP.md` (tiers 0–2 except the FFT library swap; the document was retired 2026-09-23, see git history).

### Architecture
- **Async analysis (default on)**: the cook thread only mixes/EQs/ingests the new audio block and copies the last
  finished spectrum to the output; a worker thread owns the FFT engine, the tables and all DSP state
  (`AnalysisPipeline`). Mailbox with "latest job wins", mutex-guarded double-buffered results, hold-on-late
  (never blocks the cook). `Async` off runs the same pipeline inline as before.
- **Channels = Mono Mix (default)**: every input channel is averaged into ONE analysis channel (a stereo input costs one
  FFT). `First Channel` and `All Channels` (one FFT per channel, the previous behaviour) are selectable. Output channel
  for a mixed multichannel input is named `mix_fft`.
- Worker never calls into Python: the plan log is deferred and flushed to the Textport by the cook thread.

### Real-time
- **FTZ/DAZ** (flush-to-zero / denormals-are-zero) set on the cook and worker threads — no more 100× slow frames on
  silence from denormal IIR/ballistics state.
- **Silence short-circuit**: a channel whose whole window is digital silence outputs zeros without an FFT
  (linear-magnitude mode).
- **Parameter Poll Every N Cooks** (default 1; **removed in v2.7.0**): TouchDesigner parameter reads are the host-side cost that scales with
  options; polling every 2–4 cooks removes most of them at ~33–66 ms UI latency.
- **Update Every N Cooks** (default 1; **removed in v2.7.0**): recompute the spectrum every N cooks and hold in between (the 3175-sample
  window overlaps 77 % between consecutive 60 fps cooks anyway).
- **Magnitude only up to Display Max**: the FFT magnitude is computed only for the linear bins the warp reads.
- **Warp Interpolation = Cubic** (Catmull-Rom, 4 gathers): smoother lobes than linear on a 2× larger FFT — lets a
  16384-point FFT look like the 32768-point one at half the FFT cost. Default stays Linear.
- Info CHOP: `async_active`, `dsp_time_us` (worker), `cook_time_us` (cook thread), `jobs_dropped`, `hold_frames`,
  `analysis_channels`, `param_reads`; Info DAT: mode, magnitude bins computed, async job stats; warning when the worker
  falls behind.

### Numbers (1 analysis channel, N = 32768, 16384 bins, Log, default options)
- DSP per analysis (worker): 44–50 µs (≈ 42 with the measured plan).
- Cook thread with Async on: ingest + window copy + output copy ≈ **3–6 µs** (estimate from stage timings; measure in
  TD with `cook_time_us`).

## [v2.2.2] - 2026-08-26 — "why is it slower than in July?"

A per-stage benchmark of **every commit since the first one** (`bench/`, same flags, interleaved runs) answered it:
the early builds (`0c529f7`…`b4dac6f`) ran the FFT with `FFTW_MEASURE` plans (≈36 µs instead of 54) **and their EQ was
dead** (`hasActiveFilter` evaluated before the design call — fixed in `d60b7e3`, which silently switched the default
6 dB high shelf ON: +16 µs per channel per cook). Later commits went back to `ESTIMATE` (+18 µs). Together that is the
~2× you remembered. This release gets the fast configuration back without re-breaking anything:

### Performance
- **EQ runs at ingest** on the new samples only, with continuous IIR state: 16 µs → 3.6 µs per channel, and it is now
  *correct* (the previous per-window re-filter restarted the filter from a stale state every frame; the new test shows
  a 0.19 error vs the true filter output for the legacy path).
- **Section enable toggles that bypass code AND parameter reads** (TouchDesigner `getPar*` calls are not free):
  `EQ Enable` (+ `High Shelf` / `Low Shelf`), `Ballistics Enable`; dB options are read only when `Loudness ≠ Off`,
  `Kaiser Beta` only for the Kaiser window, ms fields only in ms modes. Default configuration: 19 parameter reads
  instead of 31. **Defaults: EQ off, ballistics off** (what the early builds effectively did).
- **`FFT Planner = Auto` now means: instant plan now, measured plan upgraded in the background.** If wisdom already
  holds a measured plan (`FFTW_MEASURE | FFTW_WISDOM_ONLY`) it is used immediately; otherwise an `ESTIMATE` plan is
  used for the first cooks while a background thread measures the better one, which is swapped in atomically on the
  next cook and saved to wisdom. No stall, ever; the FFT stage runs at the early builds' ~36 µs after the first run.
- Info CHOP `param_reads`, Info DAT `cook_time` now shows `params N us / M reads`.

Default TD configuration (Loudness/Weighting/EQ/Ballistics off), 1 channel: **67 µs → 44–51 µs**.

## [v2.2.1] - 2026-08-26

### Fixed (performance regression found by benchmarking old vs new head-to-head)
- The interpolated two-gather `20·log10` LUT introduced in 2.2.0 doubled the dB stage (4.9 → 10.5 µs/channel).
  Replaced by a single-gather 2048-entry table (0.004 dB worst-case error); dB stage back to ~5 µs.
- `Parallel Channels` is now **off by default** (threshold 8): per-frame thread-pool wake-ups cost more than the
  work they distribute for a few channels and add frame-time jitter.
- Info DAT rows no longer copy the whole plan log per row (`PlanLog::entry`).
- New Info CHOP channel `param_fetch_us` / Info DAT `cook_time` detail: time spent fetching parameters from
  TouchDesigner, to separate DSP cost from host overhead when profiling.

Old (`4abf906`) vs new, identical inputs, 1 channel: total 73–74 µs → 62–66 µs before this fix; the dB fix
brings the new pipeline to ~57 µs.

## [v2.2.0] - 2026-08-26

### Removed
- **`MKLEngine` and the `FFT Engine` menu.** The "Intel MKL / IPP" engine was a byte-for-byte copy of the FFTW
  engine (it called `fftwf_*`); no MKL was ever linked. FFTW3 is now the single backend behind `IFFTEngine`.

### Fixed
- **AVX2 alignment**: the window store landed 16 bytes off a 32-byte boundary (`pad_start = (32768-3175)/2`) and
  the peak search issued an aligned load from `data+1`. MSVC tolerated both (it emits `vmovups`); Clang/GCC would
  fault. The pad offset is now rounded down to a multiple of 8 floats (magnitude is shift-invariant), every
  SIMD helper documents its alignment contract, and unaligned intrinsics are used where alignment isn't guaranteed.
- **Identity bypass never triggered** for a 1:1 linear grid: floating-point `frac = i - ε` and the last bin's
  `i0 = nlin-2 / w = 1` both defeated the check. Warp now snaps near-integer positions and accepts the clamped
  last bin (Linear scale with 16385 bins: warp 4.2 µs → 1.2 µs).
- **Textport logger string injection**: `sanitizeForPython` stripped `"` but the generated script used `'`.
  Replaced by `PySys_WriteStdout` (resolved once from the loaded python DLL) — no script, no escaping, and the
  plan log is now **per instance** instead of a process-global shared by every FFT node.
- **UI drag recompute storm**: window, warp tables and weighting curve each have their own cache key; dragging
  `Display Max` no longer regenerates the Kaiser window, changing the window type no longer rebuilds 16384
  transcendental warp entries.
- `Bins` is hard-clamped (8…262144) — typing 10,000,000 no longer allocates 40 MB per buffer.
- Adding channels no longer resets the audio history of the existing channels.
- `getOutputInfo` no-input path and `getChannelName` null-safety; `OP_CHOPInput::sampleRate` is `double` in API 10.
- Release builds no longer use `/INCREMENTAL` + `/Zi` (Release stays lean; RelWithDebInfo keeps the PDB).

### Added
- **C++ API 10** (`setAPIVersion`, `opHelpURL`, `majorVersion/minorVersion`), compiled against the headers of
  TouchDesigner 2025.33070 via `PluginBuilder_V2/include`.
- **AVX2 CPU guard**: `FillCHOPPluginInfo` checks AVX2+FMA+OS YMM state; unsupported CPUs get an error string and
  silence instead of an illegal-instruction crash.
- **FFT Planner** menu (`Auto` = previous behaviour, `Fast`, `Measured`) with **FFTW wisdom** persisted in
  `%LOCALAPPDATA%\TD_Custom_FFT\fftwf_wisdom.txt`. Measured plans: FFT stage 55 µs → 32 µs at N = 32768.
- **Magnitude Normalization** menu: `Coherent Gain` (previous behaviour) or `Full Scale` (sine amplitude A → A;
  DC/Nyquist bins halved).
- **dB Reference** menu: `Frame Peak` (previous per-frame AGC behaviour), `0 dBFS` (absolute), `Slow AGC`
  (1.5 s peak follower).
- **Window Length Mode** (`Samples` = previous behaviour / `Milliseconds`) with `Window Length ms`.
- **Ballistics Mode** (`Coefficient` = previous behaviour / `Milliseconds`) with `Attack ms` / `Release ms`,
  frame-rate independent through `OP_TimeInfo::deltaMS`.
- **Parallel Channels** (on by default from 4 channels) via `std::execution::par`; channels share only read-only tables.
- **Real AVX2 warp**: `uint32` index tables with implicit `i1 = i0+1` (8 B/bin instead of 20) and `vgatherdps`
  interpolation; interpolated 257-entry `20·log10` LUT (0.034 dB staircase → 2e-5 dB); Newton-refined `rsqrt`
  magnitude (rel. error 2.2e-7).
- `Parameters::Values` + `Parameters::eval()` (one parameter fetch per cook) and `enum class` menus with
  `static_assert`-checked tables — menu reorders can no longer silently remap behaviour.
- Info CHOP: `parallel_active`, `cook_time_us`, `linear_bins`. Info DAT: `window_resolution`, `cook_time`, `wisdom_file`.
- **Headless tests** (`tests/dsp_tests.cpp`, 292 checks: SIMD vs scalar parity, FIFO, normalization, warp, dB,
  ballistics, 1 kHz sine through the full pipeline) and **benchmark** (`bench/bench.cpp`, per-stage µs).
- `plugin.json` manifest and `.vscode/` (attach-to-TouchDesigner, IntelliSense via `compile_commands.json`).

### Changed
- `CMakeLists.txt` is now 15 lines and includes `PluginBuilder_V2/cmake/TDPlugin.cmake`
  (`td_add_plugin`, `td_plugin_use_fftw3`, `td_plugin_optimize`, `td_plugin_add_test/bench`).
- `cookEveryFrame` → `cookEveryFrameIfAsked` (no CPU burn when nothing views the node; FIFO resyncs on resume).
- `Kaiser Beta` is a float parameter.
- Repository: removed tracked `fftw-3.3.5-dll64.zip`, `temp_extract/` (10 MB of bench exes and Fortran headers),
  unused double/long-double FFTW variants, stray `Parameters.obj`; `__Plugins__/**/*.dll` is no longer tracked.
- README rewritten to match the code (no "FFTW_PATIENT", no "Flattop", no "MKL", real DLL size and benchmark numbers).

---

## [v2.1.0] - 2026-08-14

- 2x unrolled AVX2 FMA magnitude kernel (16 bins per iteration).
- `FFTW_MEASURE` primary planner (≤ 16384), `FFTW_ESTIMATE` above.
- `getWarningString` / `getErrorString` health diagnostics.

## [v2.0.0] - 2026-07-29

- Direct C++ → Textport logging through the embedded CPython runtime.
- Consolidated 5-file source layout.
- AVX2 magnitude, vectorized post-processing, zero-copy ballistics bypass.
- Middle-click info popup and Info DAT export of the plan log.
