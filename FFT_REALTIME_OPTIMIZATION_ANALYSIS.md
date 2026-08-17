# FFT Plugin — Real-Time Performance Analysis & Optimization Plan

> Analysis of the last 15 commits in `Plugin_FFT`, focused on real-time (60–120 FPS) performance.

---

## Commit-by-Commit Performance Analysis (Oldest → Newest)

### 1. `492b549` — Add explicit console logging for FFT plan generation
**Impact: NEUTRAL**

- Added `printf`/`fflush` logging for each plan-creation step (EXHAUSTIVE, PATIENT, MEASURE, ESTIMATE).
- Inserted `FFTW_PATIENT` as an intermediate fallback between `FFTW_EXHAUSTIVE` and `FFTW_MEASURE`.
- **Note**: `FFTW_EXHAUSTIVE` was already the pre-existing primary planner before this commit (not introduced here). This commit only made the fallback chain more verbose and added logging.
- The `printf`/`fflush` calls run **only during plan creation** (inside `rebuildDSP()`, an infrequent path), not in the per-frame `execute()` hot path. Console logging itself is **not** a real-time regression.
- The actual problem was the pre-existing `FFTW_EXHAUSTIVE` primary planner (32 s stalls for N=16,384), which was later corrected by `cd94d9d` (→ `FFTW_MEASURE`) and `2daf9f1` (→ `FFTW_ESTIMATE`).

### 2. `a9653da` — Expose dynamic FFT plan status to TouchDesigner
**Impact: NEUTRAL**

- Added `getPlanStatus()` virtual method to `IFFTEngine` / `FFTWEngine` / `MKLEngine`.
- Pure diagnostic — no runtime cost during `execute()`.

### 3. `8debd78` — Add high-resolution timer benchmarking and persistent file logging
**Impact: NEUTRAL at runtime, MIXED at plan-creation**

- Added `chrono::high_resolution_clock` timing around `fftwf_plan_dft_r2c_1d`.
- Added `fft_plan_log.txt` file I/O via `std::ofstream` append.
- File I/O during plan creation adds latency, but plan creation only happens on rebuild (infrequent).
- The timing itself is useful for diagnostics.

### 4. `9432d24` — Fix: Acquire Python GIL before PyRun_SimpleString
**Impact: NEUTRAL**

- Fixed a thread-safety bug: `PyRun_SimpleString` was called without holding the GIL.
- Correctly acquires `PyGILState_Ensure()` / `PyGILState_Release()` around Python calls.
- No perf impact on the audio processing path.

### 5. `260c009` — Fix: Absolute file path for fft_plan_log.txt, export history to Info DAT
**Impact: NEUTRAL**

- Hardcoded absolute path (`c:/Users/Z/...`) for log file.
- Added in-memory `getPlanLogHistory()` vector (256-entry cap) for Info DAT export.
- Log history is accumulated in-memory — minor memory overhead, no runtime cost.

### 6. `c0e6537` — Refactor: Remove file logging, route Python log via sys.stdout.write
**Impact: MIXED**

- **Removed file I/O** (`fft_plan_log.txt`) — eliminates disk writes during plan creation. **Positive.**
- Switched from `print()` to `sys.stdout.write()` with `try/except/fallback` — more robust but slightly more Python code to execute per log call.
- Still uses `FFTW_EXHAUSTIVE` as primary planner (inherited from `492b549`).

### 7. `c5cf2ff` — Docs: Add CHANGELOG.md and README.md
**Impact: NEUTRAL**

- Documentation only.

### 8. `c85c5e0` — Fix(EQ): Resolve critical EQ bug + fix AVX2 permute build break
**Impact: MIXED (correctness win, minor perf regression)**

- **CRITICAL FIX**: EQ filters were never activating. `hasActiveFilter()` was evaluated *before* the filter design call, so coefficients were always stale/inactive. The EQ was effectively a no-op (fast but wrong).
- **AVX2 fix**: Replaced `_mm256_permute4x64_ps` (which caused a build break on the compiler) with a `reorderLanes` lambda using `_mm256_castpd_ps` / `_mm256_permute4x64_pd` / `_mm256_castps_pd` — a free bit-pattern cast workaround. This restored the 16-bin SIMD magnitude pipeline.
- **New branching**: `processAudio` now checks `if (m_high_shelf.active)` and `if (m_low_shelf.active)` **per-sample** inside the processing loop. When EQ is enabled, this introduces branch mispredictions in the hot path.
- Split `processAudio` from `updateAndCheckActive` — lazy redesign via parameter tuple hashing. **Positive** for avoiding redundant trig calculations.

### 9. `d60b7e3` — perf: Upgrade computeMagnitudeAVX2_FMA to 2x unrolled 16-bin SIMD
**Impact: POSITIVE (major)**

- Upgraded `computeMagnitudeAVX2_FMA` from 8-bin SIMD to **16-bin SIMD** (2x unrolled, processes 16 complex bins / 32 floats per loop iteration).
- This is the most significant real-time performance improvement in the history: the magnitude spectrum calculation is the post-FFT hot path, and doubling throughput here directly reduces per-frame CPU time.
- Added `getWarningString` / `getErrorString` for health telemetry — no runtime cost.
- Fixed a bug where `info->sampleRate` was set to `bins` (output bin count) instead of the actual sample rate in the no-input case. **Correctness + avoids downstream resampling in TouchDesigner.**

### 10. `cd94d9d` — perf: Remove FFTW_PATIENT, enforce FFTW_MEASURE as primary
**Impact: POSITIVE (major)**

- Changed primary planner: `FFTW_EXHAUSTIVE` → `FFTW_PATIENT` → `FFTW_MEASURE` → `FFTW_ESTIMATE`
- Then this commit: `FFTW_PATIENT` → `FFTW_MEASURE` (removed PATIENT)
- FFTW_MEASURE benchmarks a limited set of codelets — produces well-optimized plans with sub-millisecond creation time (vs 32 s for EXHAUSTIVE).
- **Tradeoff**: MEASURE plans are slightly less optimal than PATIENT/EXHAUSTIVE, but the near-instant creation makes it viable for real-time.

### 11. `ec3a9bf` — chore: Sync Plugin_FFT.toe workspace file
**Impact: NEUTRAL**

- Binary workspace sync only.

### 12. `2daf9f1` — fix: Recover dedup changes + crash-guard + log scale fix
**Impact: MIXED (major positive for real-time reliability, minor negative for raw execution speed)**

- **FFTW_ESTIMATE primary** (MEASURE fallback): **Major positive.** FFTW_ESTIMATE creates plans instantly with zero benchmark overhead — no stalls during rebuilds. This is the correct choice for real-time. Tradeoff: ESTIMATE plans are less CPU-optimized than MEASURE plans, so `execute()` runs slightly slower per call. The tradeoff favors ESTIMATE for real-time (consistent, predictable frame times).
- **Crash-guard**: `try/catch` around `execute()` and `rebuildDSP()`. Exception handling has zero cost when no exceptions are thrown (modern C++ / MSVC zero-cost EH). Positive for robustness.
- **Defensive clamps**: null checks, sample rate clamp `[1, 192000]`, channel count clamp `[0, 64]`. Negligible cost.
- **Stage markers** (`myExecStage`): negligible overhead.
- **Extracted `applyWindow` / `applyWeightingCurve` helpers**: code quality improvement, no perf change.
- **DecibelConverter**: Replaced the SIMD dB conversion loop with a **scalar** loop (peak search still SIMD). This is a **performance regression** — `std::log10` is now called per-bin in a scalar loop. The comment acknowledges this: "log10 has no SIMD instruction; peak search above is vectorized, this pass is compute-bound on log10." **This is now one of the top CPU hot spots.**
- **Fixed scale case labels**: Log was `case 1` → `case 0`, etc. Correctness fix.
- **`ChannelState::initBuffers()`**: consolidated buffer init. No perf change.
- **Python logger**: moved to `python_logger` namespace, 256-entry history cap. No runtime cost.
- **CMake relocatability**: `${CMAKE_CURRENT_SOURCE_DIR}/../../../PluginBuilder_V2` instead of hardcoded path. No runtime impact.
- **CMake MSVC flags**: `/O2 /Oi /Ot /fp:fast /arch:AVX2` — all correct for real-time.

### 13. `e9985c0` — fix: Honor Logfloor parameter in Log and Melog frequency scales
**Impact: NEUTRAL**

- Corrected `fmin` calculation in `computeTargetHzGrid` (was clamped to `max(1, fmax*0.1)`; now uses `floor_hz` directly).
- Pure correctness fix, no runtime performance impact.

---

## Current State Assessment (e9985c0)

### What's Already Optimized
| Area | Strategy | Status |
|---|---|---|
| Plan creation | `FFTW_ESTIMATE` primary, `FFTW_MEASURE` fallback | Optimal for real-time — zero stalls |
| FFT execution | Pre-built plans reused in `execute()` | Zero overhead at runtime |
| Magnitude spectrum | 16-bin AVX2 FMA SIMD, 2x unrolled | Optimal |
| Windowing | 16-float AVX2 SIMD, 2x unrolled | Optimal |
| Weighting curve | 16-float AVX2 SIMD, 2x unrolled | Optimal |
| Ballistics | 16-bin AVX2 FMA, 2x unrolled, bypass when disabled | Optimal |
| EQ | Bypass when inactive, lazy redesign via tuple hash | Good (per-sample branching when active) |
| Zero-padding | Persisted, zero-filled once at rebuild | Optimal |
| Output copy | `std::memcpy` to TD memory | Optimal |
| Crash safety | `try/catch` wrappers, null clamping | Robust |
| Build flags | `/O2 /Oi /Ot /fp:fast /arch:AVX2` | Optimal |

### Remaining CPU Hot Spots
| # | Hot Spot | Current Implementation | Estimated % of per-frame time (rough) |
|---|---|---|---|
| 1 | **Decibel (dB) conversion** | Scalar `std::log10` per-bin loop | **~15-30%** (largest non-FFT cost) |
| 2 | **Peak frequency tracking** | `std::max_element` (scalar) in channel-0 telemetry | ~1-3% (only on channel 0) |
| 3 | **Warping interpolation** | 4-bin AVX2 (`i += 4`) — not 2x unrolled | ~5-10% |
| 4 | **FFTW plan quality** | `FFTW_ESTIMATE` plans are sub-optimal vs `FFTW_MEASURE` | ~5-10% (mitigated by instant creation) |
| 5 | **Python logging** | `GetModuleHandle` + `GetProcAddress` calls per `logPlanEvent` | ~0% (only during rebuild, not execute) |
| 6 | **EQ per-sample branching** | `if (active)` checks inside sample loop | ~0-5% (only when EQ active) |

---

## Real-Time Optimization Plan

### Priority 1 — Replace Scalar log10 in dB Conversion (Highest ROI)

**Problem**: `DecibelConverter::convertToDB()` loops over all bins calling `std::log10` scalar. For 16,384 bins at 60 FPS this is ~1M `log10` calls/second.

**Option A: Lookup table + linear interpolation (recommended)**
- Pre-compute a 65,536-entry `log10f` lookup table at startup (covering dB range).
- Use `_mm256_i32gather_ps` or 4x scalar lookup with 256-entry table + interpolation.
- 5-10x speedup over `std::log10`.

**Option B: Fast log10 approximation (bit manipulation)**
- Use the IEEE 754 exponent trick: extract exponent, approximate mantissa log via polynomial.
- No memory overhead, ~3-5x speedup.
- Less accurate than table lookup but sufficient for dB display.

**Implementation**: Create a `FastLog10` utility with a static lookup table initialized lazily on first call. Replace the scalar loop in `convertToDB`.

**Estimated savings**: 15-30% reduction in per-frame CPU time for the dB conversion stage.

### Priority 2 — AVX2 Peak Frequency Tracking

**Problem**: `std::max_element` over `warped_spectrum` (up to 16,384 bins) is scalar.

**Solution**: Use `_mm256_max_ps` reduction (already used in `DecibelConverter` peak search). Extract max + index via AVX2 + scalar fallback for the remainder.

**Estimated savings**: ~1-3% per-channel reduction (only runs on channel 0).

### Priority 3 — 2x Unroll Warping Interpolation

**Problem**: `PerceptualWarping::applyWarp` processes 4 bins per AVX2 iteration (`i += 4`). All other SIMD helpers use 2x unrolling (16 or 8 bins).

**Solution**: Unroll to 8 bins per iteration (`i += 8`), matching the pattern in `applyWindow`, `applyWeightingCurve`, and `computeMagnitudeAVX2_FMA`.

**Estimated savings**: ~5% reduction in warping stage time.

### Priority 4 — FFTW Wisdom Persistence (Optional)

**Problem**: `FFTW_ESTIMATE` produces generic plans. `FFTW_MEASURE` would produce faster plans but costs ~1ms per size at rebuild.

**Solution**: Use `fftwf_export_wisdom_to_file` / `fftwf_import_wisdom_from_file` to persist measured plans between sessions. On startup, try to import wisdom; fall back to `FFTW_MEASURE` with a timeout, then cache to disk.

**Alternative**: Add a user-toggleable "Performance Mode" parameter:
- **Real-Time** (default): `FFTW_ESTIMATE` — zero setup cost, consistent frames.
- **Benchmark** (one-time): `FFTW_MEASURE` — slightly faster execute, one-time setup stall. User manually applies.

**Estimated savings**: 5-10% in FFT execution speed (if measured plans are used), at the cost of one-time setup latency.

### Priority 5 — EQ Branch Elimination

**Problem**: When EQ is active, `processAudio` checks `if (m_high_shelf.active)` / `if (m_low_shelf.active)` inside the per-sample loop, causing branch mispredictions.

**Solution**: Restructure to 3 separate loops:
1. Always: apply high-shelf (if active) or skip entirely
2. Always: apply low-shelf (if active) or skip entirely
3. Always: blend with mix formula

Or use branchless blending: `filtered = x + amount * (shelf.process(x) - x)` where `shelf.process` returns `x` when inactive.

**Estimated savings**: ~2-5% when EQ is active (negligible when inactive, since already bypassed).

### Priority 6 — AVX2-Accelerated dB Conversion for Normalized Mode

**Problem**: `mode == 2` (normalized dB) does `std::max(0.0f, std::min(1.0f, ...))` per bin. These min/max calls can be SIMD-vectorized.

**Solution**: After computing `db` with the fast log10, use `_mm256_max_ps` / `_mm256_min_ps` for clamping when in normalized mode. This combines with Priority 1.

### Priority 7 — Eliminate Python DLL Resolution on Every Log Call

**Problem**: `python_logger::resolvePythonModule()` calls `GetModuleHandleA` multiple times per log event. Only runs during plan creation (not per-frame), so low priority.

**Solution**: Cache the `HMODULE` and resolved function pointers after first call.

**Estimated savings**: Negligible for real-time path (plan creation only).

---

## Build Configuration Review

The CMakeLists.txt at `PluginProjects/FFT/CMakeLists.txt` applies:
- `/O2` — maximize speed ✓
- `/Oi` — enable intrinsics ✓
- `/Ot` — favor fast code ✓
- `/fp:fast` — fast floating-point (enables FMA contraction, reassociation) ✓
- `/arch:AVX2` — generates AVX2 + FMA instructions ✓

**Recommendation**: Add `/fp:contract` explicitly (though `/fp:fast` enables it by default) and ensure FFTW is linked with the AVX2-capable library.

---

## Summary: What Makes Sense vs. What Doesn't

### Commits that make sense for real-time use
- **FFTW_ESTIMATE as primary planner** (`2daf9f1`): Correct — eliminates all setup stalls.
- **16-bin 2x unrolled SIMD** (`d60b7e3`): Correct — maximizes post-FFT throughput.
- **Crash guards** (`2daf9f1`): Correct — zero cost when no exceptions.
- **Zero-padding persistence** (`2daf9f1`): Correct — eliminates millions of zero-writes.
- **AVX2 windowing/weighting** (`2daf9f1`): Correct — full SIMD utilization.

### Commits that were problematic (and corrected)
- **FFTW_EXHAUSTIVE as primary planner** (pre-existing before `492b549`, corrected by `cd94d9d` and `2daf9f1`): Caused 32s stalls for N=16,384 — corrected to MEASURE → ESTIMATE. The console logging in `492b549` itself was not a regression; it only runs during infrequent plan creation.
- **Scalar dB conversion replacing SIMD** (`2daf9f1`): The original SIMD dB loop was slower than scalar due to the load/store round-trip through `alignas(32)` temp arrays. The scalar replacement is actually **faster than the original SIMD version** (which was inefficient), but still a bottleneck compared to a lookup table approach.
- **Per-sample EQ branching** (`c85c5e0`): Minor regression when EQ active, but EQ is typically off by default.

### The trajectory makes sense
The project evolved from:
> EXHAUSTIVE (32s stalls) → PATIENT → MEASURE → ESTIMATE (instant)

with progressively more SIMD vectorization (8→16 bins), culminating in the crash-safe, zero-stall real-time architecture currently at `e9985c0`. The remaining opportunity is optimizing the scalar `log10` in dB conversion, which is the biggest non-FFT CPU bottleneck.

---

## Implementation Log

### Commit `adc8945` — perf: Real-time pipeline optimizations

All per-frame priorities implemented:

1. **Priority 1 (DONE)**: `FastLog10` class added with 256-entry lookup table using IEEE 754
   bit manipulation. Scalar path replaces `std::log10` in `db_offset` and scalar tail loop.
   AVX2 path (`scaledVec`) processes 8 floats per call via `_mm256_i32gather_ps`.
   Estimated ~15-20x speedup over `std::log10` for the dB conversion stage.

2. **Priority 2 (DONE)**: `findPeakWithIndex` AVX2 function with movemask-gated reduction.
   (Originally from stash commit `f1cb0d0`.)

3. **Priority 3 (DONE)**: `PerceptualWarping::applyWarp` unrolled from 4 to 8 bins per iteration.

4. **Priority 5 (DONE)**: EQ per-sample branching eliminated. `BiquadSection::process()`
   returns `x` when inactive, so calling unconditionally is safe and removes branch
   mispredictions.

5. **Priority 6 (DONE)**: Integrated with Priority 1 — normalized-mode clamping
   (`mode == 2`) handled via AVX2 `_mm256_min_ps`/`_mm256_max_ps` in the same loop.

6. **Priority 7 (DONE)**: `writeToPythonConsole` caches `HMODULE` and `GetProcAddress`
   results in static variables, eliminating per-log-call DLL resolution overhead.

### Remaining

- **Priority 4 (TODO)**: FFTW wisdom persistence or "Performance Mode" toggle.
  The adaptive planner (MEASURE for small, ESTIMATE for large) is already in place.
  Wisdom persistence would save measured plans between sessions.

## Next Steps

1. Consider **Priority 4** (FFTW wisdom or Perf Mode toggle).
2. Explore aligned memory (32-byte) for `std::vector` buffers to enable aligned
   AVX2 loads/stores (`_mm256_load_ps` vs `_mm256_loadu_ps`).
3. Explore `_mm256_rsqrt_ps` approximation for magnitude `sqrt` (accuracy tradeoff).
4. Track progress in this file or a dedicated task list.
