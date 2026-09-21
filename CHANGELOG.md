# Changelog

All notable changes to `Plugin_FFT` are documented here.

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
- Tests: **547 checks** (was 493) - backend selection producing the same transform to float precision on both
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
- **The popup string no longer carries diagnostics of its own, and the instrumentation line is back to every 300.**
  Two changes made in `7b419b5` are undone here, together, because they landed together and both reverted cleanly.
  It had cut the entry line from calls 1, 2 and every 300th down to **once per load**, and it had added two lines to
  the popup text itself - `Info callbacks entered: popup N, Info CHOP N, Info DAT N` and `Cook stall: largest gap
  between cooks ... ms`. The popup went empty after that commit and came back when both were reverted, so the
  string content is where the break was; **which of the two changes caused it is not separated**, and does not need
  to be, because the durable rule is now this: a diagnostic must never be able to change what it measures. Both
  values live in the `info_callback_calls` Info DAT row, which TouchDesigner reads as a value rather than rendering
  as the popup, so whatever the popup does with its text cannot move that number. The periodic line is restored to
  the cadence this harness had when the popup was last observed full - one line per 300 cooks, about one every 5 s
  at 60 fps, carrying the call index, the node's cook count, and the character count handed to TouchDesigner. It is
  more log noise than a one-shot line, and it is the only thing that separates "TD never entered the callback" from
  "TD entered it and nothing rendered", which are the two remaining explanations and need opposite fixes.
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
  hidden. Tested: 547 checks, 0 failures.
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

Implements the roadmap in `FFT_REALTIME_PERFORMANCE_ROADMAP.md` (tiers 0–2 except the FFT library swap).

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
