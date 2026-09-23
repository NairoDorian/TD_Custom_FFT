# EssentiaTD → Plugin_FFT: what is worth porting

**Current as of:** Plugin_FFT v2.12.1.

**EssentiaTD reviewed:** v2.0.3 (HEAD `c050b40`), Darien Brito, AGPL-3.0-or-later.

**Scope:** all ~80 project-authored files were read:
- `src/` (5 CHOPs, their `Parameters_*` files, the `Shared/` framework);
- the headless tests and the `key_oracle` tool;
- `ci/` (the Essentia build overlay and patches), the GitHub workflow, both installers;
- `docs/`, README, CHANGELOG, RELEASE_NOTES.

The ~420 vendored Essentia headers and the 107 MB static lib were inventoried, not read line by line.

**Lens:** Plugin_FFT is an FFT-only CHOP. Tonal key detection, beat tracking and BPM do not apply. What
counts is anything that touches the spectrum, the cook model, TouchDesigner integration, robustness, testing
and distribution. The installer is covered separately in `INSTALLER_PLAN.md`.

---

## 0. The short version

EssentiaTD is five CHOPs (Spectrum, Spectral, Tonal, Rhythm, Loudness) around the Essentia library. Its
engineering value is **not** in the FFT path. Ours is faster at every stage:
- it uses KissFFT, scalar and without SIMD;
- the FFT runs synchronously on the cook thread;
- it allocates in the hot path.

**Its value is in three places:**
1. **Features we don't have**: SuperFlux, MFCC, spectral contrast, HFC, spectral complexity, peak-based
   chroma, K-weighted LUFS.
2. **Measured findings about real-time audio analysis in TD**:
   - the window length versus transient trade-off;
   - audio skipped when a cook's timeslice is longer than the window;
   - clocks that drift when frames drop.
3. **Hard-won robustness patterns**:
   - per-feature failure containment;
   - warning slots;
   - the sanitized cook rate;
   - a headless test harness that drives the *shipped* `execute()`.

**Top 8, in the order I'd do them:**

| # | What | Why | Effort |
|---|---|---|---|
| 1 | **Don't re-analyze a stale window** (§2.1) | A job is published every cook even when no new audio arrived: flux reads 0 on those frames and ballistics advance on old data | S |
| 2 | **Clock ballistics by audio time, not TD's `deltaMS`** (§2.2) | EssentiaTD measured tempo 20 % high under frame drops for exactly this reason | S |
| 3 | **Coverage warning when the timeslice exceeds the window** (§2.3) | Audio is silently skipped; EssentiaTD measured onset F1 collapse at 30 fps | S |
| 4 | **Resolution readout from the true window** (§1.1) | `Zeropad` / Fixed-upsampling narrow the *spacing*, not the resolution; users need the honest number | S |
| 5 | **SuperFlux-style onset novelty** (§1.3) | The best flux variant for audio-reactive work; our log/mel warp already produces the bands | M |
| 6 | **Headless `execute()` test harness** (TDStubs pattern, §5) | Our tests drive the pipeline, not the CHOP; the cook-path bugs live in the CHOP | M |
| 7 | **Per-feature failure containment + warning slots** (§4) | One optional feature must never take down the node; warnings must not overwrite each other | M |
| 8 | **Band features: MFCC / mel bands / spectral contrast / HFC** (§1.4) | Cheap on top of the existing warp; standard inputs for ML/visual mapping | M–L |

---

## 1. Signal processing

### 1.1 FFT size, window, resolution: their measured findings (`docs/tonal-fft-resolution.md`)

- **Resolution comes from the window, not the FFT size.** `Utils.h:132-151` spells out the trap: zero
  padding narrows bin *spacing* without improving true resolution, so any check must use the un-padded
  window.
  - Real-time EssentiaTD disables padding altogether (`RTFrameProcessor::configure(fftSize, win, 0)`).
  - **For us:** `Zeropad` + Pad 16K + Fixed 32768 bins produce very fine *spacing* from a 3175-sample
    window (72 ms), whose true resolution is ~1/T ≈ 14 Hz, or the window's main-lobe width.
  - `mainLobeHalfWidthBins()` in `RateModel.h` already computes the main-lobe figure.
  - **Port:**
    - Add Info CHOP/DAT rows `true_resolution_hz = mainLobeHalfWidth · sr / winSamples` and
      `lowest_semitone_hz = true_resolution_hz / (2^(1/12) − 1)` (their `lowestResolvableSemitoneHz`).
    - Add a warning when **Scale = Chroma** or Log starts below that frequency.
  - Their rule: semitone separation at middle C needs Δ ≤ 15.56 Hz, which is 4096 samples at 44.1/48 kHz
    and 8192 at 88.2/96 kHz (`resolveAutoFftSize`, `Utils.h:99-106`).
- **Long windows kill transients (measured).**
  - Onset F1 was **0.474 at 1024 versus 0.133 at 4096** on a 263-onset reference.
  - A real-time sweep at 60 fps gave 0.461 @1024, 0.377 @512, 0.409 @2048, **0.028 @4096**.
  - **For us:** our default window of 3175 samples sits in the "smeared" zone for flux/onset-style
    features. It is fine for a display.
  - **Port:** compute the novelty feature (§1.3) from a *short* sub-window (e.g. the newest 1024 samples)
    with its own small FFT, independent of the display window. EssentiaTD's v2.0 made the same split:
    per-operator FFT sizes, because a shared spectrum forced one size on conflicting needs.
- **Measurement lesson (theirs, same as our popup lesson):** an op nothing pulls doesn't cook. Their
  earlier results were artefacts of sporadic cooks. In-TD A/B probes need a consumer or a forced cook, and
  should record the playhead as a channel.

### 1.2 Windows and normalization

- **Their window menu:** Hann, Hamming, Triangular, and Blackman-Harris at 62/70/74/92 dB, default BH-62.
  - We have Kaiser (β manual or auto from the dB range), Hann, Hamming, Blackman, Blackman-Harris and
    Rectangular.
  - **Port (low priority):** the BH-92 variant, a fixed ~92 dB sidelobe window that behaves like β≈13
    Kaiser. Kaiser covers it, so this is for users who think in named windows.
- **Their `Windowing(normalized=true)`** scales the window to area 2, so a sine of amplitude A peaks at
  ≈A. **Our `Magnitude Normalization = Full Scale`** is the same convention: a full-scale sine reads its own
  amplitude, and DC/Nyquist are halved.
  - The old comparison doc's "×2 missing" recommendation is therefore covered by that menu. Our default,
    Coherent Gain, is the legacy N/2 reading kept for existing projects.
  - Still worth pinning with a test, "sine of amplitude A reads A in Full Scale", against an independent DFT.

### 1.3 Onset / novelty: SuperFlux (`EssentiaRhythmCHOP.cpp:23-33, 274-308, 485-515`)

- **What it is:** band energies on a coarse filterbank. They use 24 mel bands (26 explicit edges, 27.5 Hz to
  16 kHz); the default 139 bands is too many for a 1024 FFT. Then
  `novelty = Σ_b max(0, log E_b(t) − max(E_{b−1..b+1}(t−μ)))`, i.e. a **±1-band max filter** on the
  reference frame before the positive difference.
  - The max filter suppresses vibrato and slow glides, which plain flux reports as onsets.
  - `SuperFluxNovelty(binWidth=3, frameWidth=2)` over a 3-frame history.
  - **Their bug to avoid:** the fixed 16 kHz top edge throws below 32 kHz sample rates. Clamp the edges to
    Nyquist.
- **Their adaptive onset trigger** is time-based (`EssentiaRhythmCHOP.cpp:543-604`):
  - a lookback of ~83 ms;
  - a decaying threshold with τ = 0.158 s;
  - fire when `odf > mean(lookback) × (1 + 3·(1−sensitivity))` **and** above the decaying threshold, then
    raise the threshold to 1.1·odf;
  - a **relative** silence gate, `odf < max(1e-6, 0.01·runningPeak)`;
  - no firing on the first frame after a reset.
- **Port, as a Features-page option:**
  - a `novelty` channel (SuperFlux on a short sub-window, over the **log-magnitude** of our warped bands, or
    of a fixed 24-band mel set);
  - an optional `onset` pulse channel with the adaptive threshold.
  - Everything in seconds (§2.2).
  - Our current `flux` stays: it is half-wave-rectified linear-magnitude flux, normalised by the frame's
    total magnitude.
- **Their flux first-frame artefact:** a freshly built Essentia `Flux` returns ‖S‖ instead of 0. **We don't
  have this bug:** `prev_linear` is cleared on rebuild, so the first frame reports 0. Worth a regression
  test anyway: "no flux spike after a reconfigure" (their `spectralflux_test.cpp`).

### 1.4 Spectral features we don't have (`EssentiaSpectralCHOP.cpp`)

We have centroid, rolloff (85 %), flatness, flux, RMS dB, and bass/mid/high dB. They additionally have the
following, all computed from the linear magnitude:

| Feature | Essentia config | Cost on our side | Value |
|---|---|---|---|
| **Mel bands** (power, N bands, lo/hi Hz; optional 10·log10) | `MelBands type=power` | Our warp already makes a mel axis; a *band-energy* output (sum of power per band, not interpolation) is ~1 pass | High: compact, standard |
| **MFCC** (13 coeffs over 40 mel bands) | `MFCC` | Mel energies → log → DCT-II (40×13, precomputed matrix) ≈ 0.5 µs | High: timbre vector for ML/mapping |
| **Spectral contrast** (6 bands, peak–valley) | `SpectralContrast` | Per-band sort or partial select, ~1–2 µs | Medium |
| **HFC** (Masri: Σ k·\|X_k\|²) | `HFC` | Free in our `featureSums` loop (we already accumulate `pw·k`, which *is* Masri's HFC before normalization) | Medium: percussive energy |
| **Spectral complexity** (peak count above a threshold) | `SpectralComplexity` | One pass | Low |
| **PCA over the feature vector** | Their `PCAProcessor` (JAMA eigen, throttled, sign-flip continuity) | Not our job; a downstream CHOP can do it | Low |

- **Channel naming worth copying:** mel bands named `mel{i}_{loHz}_{hiHz}` (their `Melfreqnames`
  option), so downstream selects are self-documenting. Same idea for chroma: `note_c`, `note_cs`, …
- **HFC is nearly free:** `detail::featureSums` already accumulates Σ pw·k. Exposing it is one more output
  channel.

### 1.5 Chroma done properly (`EssentiaTonalCHOP.cpp:1007-1088`)

- **Our Chroma scale** warps every FFT bin onto a pitch-class axis from a 20 Hz floor.
- **Theirs:**
  1. `SpectralPeaks`: 60 peaks by magnitude, threshold 1e-5, 20 Hz to 3500 Hz, re-sorted by frequency in
     scratch buffers (no allocation);
  2. `SpectralWhitening`;
  3. `HPCP`: cosine weighting, harmonics, normalization unitMax;
  4. for key, peak-normalize then gate below 0.2.
- **Why peaks + whitening are better for chroma:** broadband energy and loud low notes stop dominating,
  and the band limit keeps percussion out.
- **Harmonic leakage note (theirs):** summing harmonics leaks each note's 3rd harmonic a fifth up.
- **Port (Medium):** a "Chroma (peaks)" variant that outputs **12/24/36 HPCP bins** as a separate small
  output, not through the display warp:
  - parabolic-interpolated peaks from our linear magnitude;
  - cosine-weighted pitch-class accumulation over 20–3500 Hz;
  - optional whitening.
- **Principle from their fix (issue #12):** display smoothing had leaked into the analysis accumulator.
  **Our features already read the pre-ballistics linear magnitude.** Keep it that way, and document it on
  the Features page.

### 1.6 Loudness / LUFS (`EssentiaLoudnessCHOP.cpp`)

- **K-weighting biquads from the analog prototype**, valid at any sample rate (:38-71):
  - shelf: f0 1681.97 Hz, Q 0.70718, +4.0 dB;
  - high-pass: f0 38.135 Hz, Q 0.50033;
  - TDF-II with double state.
  - **Verify before porting:** their HPF normalizes b by a0, while BS.1770/libebur128 use b = [1,−2,1]
    (a ~0.04 dB difference at 48 kHz).
- **Momentary (400 ms) and short-term (3 s) LUFS**, power-averaged; the integrated value is gated at −70 LUFS
  absolute and −10 LU relative.
- **Their deviations from EBU R128, don't copy:**
  - blocks don't overlap (the spec is 400 ms blocks at a 100 ms step);
  - the window length uses `ceil`;
  - channel 0 only;
  - `dynamic_range` isn't LRA;
  - the float floor is −100.7 instead of the −144 sentinel.
- **For us (Medium):** an optional `lufs_momentary` / `lufs_short` feature channel. Our ingest already has
  the EQ biquad machinery (RBJ shelves at ingest, stateful), so a K-weighting pre-filter is the same code
  with different coefficients, run on a **parallel** copy of the block so the spectrum is unaffected.
- Mono sum: channels are power-summed (BS.1770 weights: 1.0 for L/R/C, 1.41 for surround); −0.691 offset.

---

## 2. Real-time cook model: gaps we verified in our own code

### 2.1 Stale cooks re-analyze the same window (Plugin_FFT gap, verified)

- **In our code:**
  - `FFT::ingest()` returns early when `IngestCursor::fresh()` reports 0 new samples (the same input
    `totalCooks`, or the range didn't advance): `FFT.cpp:521`.
  - But `executeImpl` then **still calls `fillJob` + `publish()` every cook** (`FFT.cpp:786`).
  - The worker re-analyzes an identical window. Spectral **flux** then reads **0** on those frames, and
    alternates value/0 when the audio source cooks slower than the node.
  - **Ballistics** keep converging toward the same frame, using the cook's `dtMs`.
- **Their fix** (`EssentiaRhythmCHOP.cpp:200-213`): `accumulate()` returns false on a stale stamp, and the
  op **holds** its outputs. It does not re-analyze and does not advance its clocks.
- **Port:**
  - When a cook ingested nothing, skip the publish. `copyResultsToOutput` already holds the last result,
    so the output stays as it was.
  - Keep a time accumulator so the next real frame's ballistics use the **total** elapsed audio time
    (§2.2).
  - Reset still republishes.
  - Add a regression test.

### 2.2 Ballistics / rates must follow audio time (Plugin_FFT gap, verified)

- **In our code:** `BallisticsFilter::coefFromMs(p.attackMs, job.dtMs)`, where `dtMs` is TD's
  `timeInfo->deltaMS` (`FFT.cpp:752-758`, `AnalysisPipeline.cpp:575`).
- **Their measured failure:** Rhythm clocked its detection function by the reported cook rate. When
  frames drop, TD lengthens the timeslice while still reporting the nominal rate, so the tempo read **144
  instead of 120 BPM** at a sustained 50 fps. The fix was `dt = samplesThisCook / sampleRate` (CHANGELOG
  2.0.3, `rhythmclock_test.cpp`).
- **Port:**
  - `dtMs = freshSamples / sampleRate · 1000` from the ingest cursor, falling back to `deltaMS` only when
    no audio arrived (a stale cook skips the frame anyway, §2.1).
  - Same for anything else "per second" we add: novelty rate, onset timing, LUFS windows.
  - **Test:** the same ballistic trajectory at 735/882/1470-sample timeslices (their pattern).
- **Related hardening:** `sanitizedCookRate()` accepts `timeInfo->rate` only when it is finite and between
  1 and 1000, otherwise 60 (`BatchCommon.h:272-277`).
  - They saw transient garbage there, and one bad value in a declared `sampleRate` made downstream Trail
    CHOPs try enormous allocations.
  - Our `sampleRateToTouchDesigner()` falls back to 60 when the rate is not `> 0`, which also catches NaN.
    It still accepts `inf` and absurd values such as 1e9. Add the same clamp.

### 2.3 Audio skipped when the timeslice exceeds the window (Plugin_FFT gap)

- **Their `coverageWarning()`** (`RTFrameProcessor.h:154-163`): when `samples per cook > window`, the excess
  is never analyzed. They measured 35–68 % of the audio skipped at 30 fps with 512/1024 windows.
- **In our code:** `ingest` keeps `min(fresh, capacity)` and one frame is analyzed per cook, so the same
  hole exists. With the default 3175 window it takes < 15 fps at 48 kHz to open; with a user-chosen 1024
  window, anything below ~47 fps opens it.
- **Port:**
  - a warning `"Window N < input timeslice M samples: X samples/cook never analyzed"`;
  - an Info row `analyzed_fraction`.
  - **Better than theirs:** our worker could analyze **all** hops in a long timeslice, and the ballistics
    would then see every frame. It's worth it only for the novelty feature (§1.3); the display only shows
    the newest frame.

### 2.4 What we already do better (no action)

| Topic | EssentiaTD | Plugin_FFT |
|---|---|---|
| FFT | KissFFT 1.3.0 scalar, no SIMD (`ci/essentia-CMakeLists.txt:88-91`), on the cook thread | FFTW 3.3.11 AVX2/FMA (Patient plans) or oneMKL (18–27 % faster), on a worker thread |
| Hot path | Essentia `Algorithm` objects, `std::vector` copies per cook, `std::string` warnings | Allocation-free steady state (pinned by the allocation gate), AVX2 kernels |
| Hand-off | Synchronous | Wait-free triple buffers, Poll/Signal worker wake, MMCSS |
| Input dedupe | `totalCooks` stamp (Rhythm holds; **Tonal and Loudness don't**, their bug) | `IngestCursor` (totalCooks + range cursor), unit-tested |
| Output shape | 1 sample per feature, `startIndex = 0` | Same `startIndex = 0`; `timeslice = false` like theirs |
| Build flags | No `/arch:AVX2`, no `/fp:fast`, no LTCG | AVX2 + fast-math + LTCG, CPU guard |

---

## 3. TouchDesigner integration patterns

- **Warning slots** (`UnifiedCHOPBase.h:252-277`): one string per warning class, rendered in priority order
  and joined with `" | "`. The classes are Transient, Cached, Stale, Resolution, Multichannel, Algo.
  - TD shows a single warning string per op; without slots, whichever code wrote last silently erased the
    others.
  - **Port:** our `getWarningString` has a single source today. Adopt slots before adding the coverage and
    resolution warnings of §2.3/§1.1.
- **A sticky config error with a throttled retry** (`UnifiedCHOPBase.h:284-357`):
  - the error persists until the config succeeds;
  - a failed construction is retried every **60 cooks**, not every cook (a retry storm), and never only
    once (no recovery).
  - **For us:** an FFTW plan failure or a missing backend already falls back and logs. Apply the pattern
    to any future optional feature that can fail to construct.
- **Never declare 0 samples or rate 0** (`cachedNumSamples() ≥ 1`): TD turns that into a sticky "Sample rate
  is zero" error. Also keep `timeslice = false`, which we do; they document the same sticky error for
  timesliced custom CHOPs.
- **Zero the output when nothing is enabled:** `numChannels` can't be 0, so the declared channel must be
  written or it carries stale buffer contents. We always write the spectrum, so no action for us.
- **Input-contract error:** detect a wrong input by its signature and say how to fix it. Theirs rejects a
  Spectrum CHOP wired into an analyzer. For us, a *spectrum* wired into the FFT CHOP could be detected the
  same way: `numSamples > 2`, a channel named like our output, rate ≠ audio.
- **Multichannel warning** ("Analyzing channel 0 only"): ours has an explicit Channels menu (Mono Mix / First
  / All), so no action.
- **Enable-state caveat** (`EssentiaTonalCHOP.cpp:152-155`): don't grey out a parameter that still affects
  the output. Worth a pass over our `setEnableStates` (e.g. Pad greyed with Zero-Padding off is right, but
  check each case).

---

## 4. Robustness patterns

- **Per-feature guarded construction** (`EssentiaSpectralCHOP.cpp:747-854`):
  - each algorithm is created in its own try block;
  - failures are collected with the library's verbatim reason, because it names the fix;
  - a partial failure is a warning;
  - a total failure (no enabled feature alive, `anyEnabledFeatureLive`) is a sticky error with zeroed
    output.
  - **For us:** when §1.3–1.6 add optional features, give each its own guarded state, so one bad parameter
    (e.g. mel bands narrower than the bin spacing) never kills the spectrum.
- **Clamp frequency pairs before constructing** (`clampFreqBounds`): hi is in [100, Nyquist], lo is in
  [0, hi/2]. Our Display Max already clamps to Nyquist; apply the same to any new band's lo/hi.
- **Smoothing capped at 0.99:** at 1.0 the coefficient is zero and the output freezes forever. Our
  ballistics already clamp to 0.99/0.999.
- **Process-wide init under `std::call_once`, and the library logger disabled**
  (`EssentiaInit.h`): Essentia's logger is unguarded and shared by every thread. Our PlanLog is
  mutex-guarded and deferred to the cook thread, so no action.

---

## 5. Testing and tooling patterns

- **`tests/TDStubs.h` + `driveRealtime()`:**
  - fake `OP_Inputs` implement only the parameter getters, `getInputCHOP`, `getTimeInfo` and `enablePar`;
    unknown parameters return 0/"", so the plugin's own fallbacks are exercised;
  - it feeds timeslices with an incrementing `totalCooks`, calls the **shipped** `getOutputInfo()` +
    `execute()`, and captures warnings and errors;
  - `errorGaps` counts cooks after the first error that show no error: the "error visible for one frame,
    then looks healthy" bug.
  - **Port (High):** our `fft_tests` drive `AnalysisPipeline`, `AsyncAnalysis` and `RateModel` directly,
    but never `FFT::execute()`. The cook-path gaps in §2 (stale publish, dt source, coverage) are exactly
    the kind a stub harness would have caught.
  - Their rule is **one CHOP per executable**, because the DLL entry points collide. Our one-CHOP project
    has no such issue.
- **Golden-vector test** (`rtframeprocessor_test.cpp`): a 440 Hz sine at 48 kHz in 800-sample slices. It
  checks not ready at 800/1024, a same-stamp no-op, the peak bin, centroid ±40 Hz, energy concentration, and
  1025 bins with 1024+1024 padding. We have equivalents for most of these; the coverage-warning checks are
  new.
- **The "same output at different frame rates" test** (`rhythmclock_test.cpp`): the template for §2.2.
- **`tools/key_oracle`:**
  - an offline oracle that runs the **shipped** batch function against the library's reference chain;
  - JSONL output with fixed field order, `%.6f`, `setlocale("C")` for byte-stable diffs;
  - a self-check that exits 2 on drift.
  - **Their lesson, the hard way:** the oracle's "mirror" chain has drifted from the shipped code, so its
    self-check would now fail. Always call the shipped function, never a copy.
  - `wav_reader.h` (dependency-free RIFF: 16/24/32-bit PCM, float32, EXTENSIBLE, mean downmix) is reusable
    as-is. It would let `fft_bench` run on real audio files instead of synthetic sines.

---

## 6. Build, CI and packaging facts

- **Static Essentia** (`/WHOLEARCHIVE:essentia.lib`, 107 MB): each of the 5 DLLs is ~5 MB and
  self-contained, with its own factory registry.
  - It must be whole-archive, or the algorithm registrars are dead-stripped and `create()` throws at run
    time.
  - Not applicable to us: FFTW is GPL and oneMKL is chosen at run time, so both stay dynamic.
- **Dynamic CRT (`/MD`)** in both projects. Neither pins the toolset against the runtime present on the
  target machine. Our installer plan checks for `MSVCP140_ATOMIC_WAIT` / `VCRUNTIME140_1` (§4.3 there).
- **Essentia patches** (`ci/essentia-patches/`):
  - `ESSENTIA_NO_EIGEN` guards remove the Eigen/Tensor dependency. It is ABI-relevant: `Pool`'s layout
    changes, so every consumer must define it.
  - A `roguevector.h` reinterpret hack is safe only in Release; a Debug build would corrupt memory.
  - Not relevant to us beyond "compile definitions that change a class layout must be pinned in one CMake
    place". Our `TDPlugin.cmake` module does that.
- **CI** (`.github/workflows/build.yml`):
  - Windows + macOS, and Essentia built from a pinned SHA with the `ci/` overlay, cached on
    `hashFiles('ci/**')`;
  - ctest golden tests;
  - an Inno Setup installer built from **pwsh** (bash's MSYS layer mangles `iscc /D` switches, a gotcha
    worth remembering);
  - GitHub Release on `v*` tags with unversioned asset names.
  - No code signing anywhere; macOS ad-hoc only.
  - Plugin_FFT has **no CI yet**. §5 of the installer plan sketches a workflow.
- **Installer:** see `INSTALLER_PLAN.md`, which adapts their `.iss` line by line.

---

## 7. Reference designs for other open ideas

- **Magnitude convention.** Essentia's `Windowing(normalized=true)` makes a sine of amplitude A read ≈A.
  Plugin_FFT's `Magnitude Normalization = Full Scale` uses the same convention (DC/Nyquist handled).
  Worth pinning with a test: a sine of amplitude A reads A in Full Scale, against an independent DFT.
- **Batch mode** ("analyse a whole file into N spectra"): EssentiaTD's design is a clean reference.
  - `UnifiedCHOPBase` switches between realtime and batch.
  - `AsyncBatchRunner` runs the batch on a worker, with cancel and progress.
  - A 16-probe audio fingerprint triggers Autocompute.
  - A `std::tie`-based parameter snapshot warns when the cached result is stale.
- **Phase output:** EssentiaTD exposes phase (`CartesianToPolar`). It is only needed for complex-domain
  novelty or resynthesis; low priority for a magnitude display node.

---

## 8. Suggested order

The canonical, prioritised plan with acceptance criteria is `AUDIT_AND_PLAN.md`; this is the
EssentiaTD-derived subset in the order it makes sense to do it.

| Order | Item | Section | Effort |
|---|---|---|---|
| v2.13 | Skip publishing on stale cooks, and audio-time dt for ballistics; tests at 3 timeslice sizes | §2.1, §2.2 | S |
| v2.13 | Sanitize `timeInfo->rate` (finite, 1..1000) | §2.2 | XS |
| v2.13 | Warning slots, coverage warning, `analyzed_fraction` and `true_resolution_hz` Info rows | §3, §2.3, §1.1 | S |
| v2.14 | Headless `execute()` harness (TDStubs pattern) + golden-vector and frame-rate-invariance tests | §5 | M |
| v2.14 | HFC channel (nearly free), mel band energies, MFCC | §1.4 | M |
| v2.15 | SuperFlux novelty on a short sub-window + optional adaptive onset pulse | §1.3 | M |
| later | Peak-based HPCP chroma output, K-weighted LUFS channel, `wav_reader` in the bench, batch mode | §1.5, §1.6, §5 | M–L |
