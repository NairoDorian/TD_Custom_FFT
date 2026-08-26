# Changelog

All notable changes to `Plugin_FFT` are documented here.

---

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
