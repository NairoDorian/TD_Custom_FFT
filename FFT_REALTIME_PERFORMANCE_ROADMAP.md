# FFT CHOP — Real-Time Performance Roadmap (how to make the cook loop as fast as it can be)

> **Status (v2.4.0, 2026-09-10):** the cook thread is now lock-free and syscall-free (wait-free triple buffers both
> ways, worker polls on a high-resolution timer, peak search on the worker, parameters polled from `getOutputInfo`,
> dead `Parallel*` reads removed). Measured with the new `fft_bench --cook`: **11 µs mean / 17 µs p99** per cook at
> 16384 bins, 7 µs at 4096 bins, of which 60 % is the `Bins × channels` result copy. See CHANGELOG v2.4.0.
>
> **Status (v2.3.0, same day):** implemented — 1.1 async pipeline (default on), 1.2 FTZ/DAZ, 1.3 parameter poll
> interval, 1.5 silence short-circuit, 1.6 update-rate divider, 1.7 cubic warp interpolation (option), 1.9 magnitude
> only up to Display Max, plus **Channels = Mono Mix** (mono-first: one FFT for any input). Not done: 1.4 Bins Auto
> (guidance only), 1.8 FFT library swap, 1.10 cache footprint, tier 3.

> Date: 2026-08-26 · Baseline: v2.2.2 (`0851d19`) · All numbers measured on this machine with `bench/bench.cpp`
> (interleaved runs, min per stage) unless marked *estimate*. 1 channel, N = 32768, window 3175, 16384 bins, Log scale.

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

Not in the bench, but paid on the TouchDesigner cook thread every frame:

| host-side cost | how big | how to see it |
|---|---|---|
| `getPar*` calls (19 by default, 31 with everything on) | *estimate* 1–5 µs each → 20–100 µs | Info CHOP `param_fetch_us`, `param_reads` |
| TD allocating/handing out a 16384-sample × N-channel CHOP, downstream cooks, viewers | scales with **Bins × channels** | TD performance monitor minus `cook_time_us` |
| Python bind expressions on the PluginBuilder COMP (only while its parameter dialog is visible) | *estimate* 20 µs × 31 pars | close the dialog → gone |

### Straight answer to "are the ifs slowing the loop?"

No — measured. The per-cook code has 16 branches in `executeImpl` and 16 in `processChannel`; they run **once per cook**
(not per sample), cost < 0.1 µs total, and every bypassed stage measures 0.02 µs. All per-sample / per-bin loops are
branch-free AVX2 (blends instead of ifs). The loop is slow for exactly three reasons, in this order:

1. **the FFT is 32768 points** for a 3175-sample window (90 % zeros) — 80 % of the DSP,
2. **the output is 16384 bins** — warp, memcpy and everything TouchDesigner does downstream scale with it,
3. **the work runs on TouchDesigner's cook thread**, serialized with everything else in the frame.

Everything below attacks those three.

---

## 1. The upgrade list, ranked by real-time gain

### Tier 0 — structural (the big one)

#### 1.1 Asynchronous pipeline: cook = ingest + copy; the FFT runs on a worker thread
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

### Tier 1 — cheap, safe, do first

#### 1.2 Flush-to-zero / denormals-are-zero on the cook (and worker) thread
*Expected: prevents 10–100× per-sample slowdowns on quiet audio.* Biquad IIRs and the ballistics filter decay into
denormal floats on silence; every operation on a denormal costs ~100 cycles. Set `_MM_SET_FLUSH_ZERO_MODE` /
`_MM_SET_DENORMALS_ZERO_MODE` at the top of `execute()` (save/restore MXCSR around the cook). This is likely one
reason "it feels slower sometimes": sporadic slowness when the input goes quiet.

#### 1.3 Parameter polling interval
*Expected: −(19…31 reads × per-read cost) on most frames.* TD has no "parameter changed" callback for C++ OPs, so
`Parameters::eval()` re-reads everything every cook. Add `Param Poll` (frames, default 1; e.g. 4 = read every 4th
cook, ~66 ms UI latency — fine for an analyzer). Menus/toggles that gate DSP rebuilds are cheap to keep polling.
Measure the actual per-read cost first via `param_fetch_us / param_reads` in the Info CHOP — if it is ~1 µs, this
tier is worth ~20 µs; if ~5 µs, it is worth ~100 µs and beats everything except 1.1.

#### 1.4 Output Bins sized to what is displayed
*Expected: warp 4.8 → 0.6 µs at 2048 bins, memcpy 1.5 → 0.2 µs, and TD downstream cost ÷8.* A 16384-bin spectrum
is 8× more samples than a 1920-px-wide display can show. Recommend 2048–4096 in the README; consider a `Bins = Auto`
mode (= 2 × FFT bins needed for the chosen `Display Max`, capped).

#### 1.5 Silence / no-change short-circuit
*Expected: 100 % of the DSP skipped while the input is silent or frozen.* If the new block is all zeros and the FIFO
is already all zeros (a per-channel "zero run" counter), output zeros (or the held spectrum) without windowing/FFT.
Cheap: the check is a single AVX2 OR-reduction over the 735 new samples (< 0.1 µs).

#### 1.6 Update-rate divider (hop control)
*Expected: cost ÷N on average, e.g. 30 Hz analysis at 60 fps = half.* Today the spectrum is recomputed every cook
with a 735-sample hop on a 3175-sample window (77 % overlap). An `Update Every N Frames` (or `Min Hop ms`) parameter
holds the last spectrum between updates; with 1.1 the worker simply idles.

### Tier 2 — FFT throughput

#### 1.7 Smaller FFT + better interpolation instead of zero-padding
*Expected: FFT 36 → 6.4 µs (N = 8192) or 16 µs (N = 16384); warp +2–4 µs for cubic.*
The window resolution is 13.9 Hz; the 32768-pt FFT's 1.35 Hz bins are 10× denser than the information content.
The zero-padding only does *cosmetic* interpolation of the magnitude lobes so the log grid (0.0087 Hz apart at 20 Hz!)
looks smooth. A Catmull-Rom (4-tap) interpolation in `applyWarp` on an N = 16384 spectrum gives smoother lobes than
linear interpolation on N = 32768, at a third of the total cost. Keep `Zero-Pad Len` as is; add
`Warp Interpolation = Linear | Cubic` and re-benchmark N = 8192/16384 visually.

#### 1.8 FFT library options
| option | expected FFT stage | notes |
|---|---|---|
| FFTW 3.3.5 dll64 (current), measured plan | 36 µs | MinGW build, **no AVX2/FMA codelets** |
| FFTW 3.3.10 built with `-DENABLE_AVX2=ON` (vcpkg `fftw3[avx2]`) | *≈ 28–32 µs* | same API, GPL |
| pffft (BSD, single header, AVX-capable fork) | *≈ 30–38 µs* | no DLL to ship, no GPL |
| Intel IPP `ippsFFTFwd_RToCCS_32f` / oneMKL DFTI | *≈ 20–28 µs* | fastest on Intel; ~30 MB redistributable |
| `fftwf_plan_many_dft_r2c` (all channels in one call) | −1 µs / channel | batched execution, better cache use |
| `FFTW_PATIENT` via wisdom (background, once, ~3 s) | −2–5 % | free after 1.1's background planner |

#### 1.9 Magnitude only where it is needed
*Expected: 3 µs → 2 µs when Display Max < Nyquist.* `computeMagnitude` runs over all N/2+1 bins; the warp only reads
up to `fmax` (e.g. 16 kHz of 22.05 kHz = 73 %). Stop at the warp's max index.

#### 1.10 Cache footprint
Per channel: padded frame 128 KB + complex scratch 128 KB + magnitude 64 KB + warped 64 KB ≈ **400 KB**; 8 channels
≈ 3.2 MB — beyond L2, so per-channel cost grows with channel count. Options: in-place r2c (input buffer reused for the
complex output, −128 KB, costs a 118 KB memset of the zero region ≈ 3 µs), `float16`/`uint16` warp tables (−64 KB
shared), and **process one channel completely before the next** (already the case) so each channel's set stays hot.

### Tier 3 — larger channel counts / far options

- **Parallel channels** (exists, off by default): only worth it from ~8 channels; with 1.1 in place it becomes
  "workers per channel" and the question disappears.
- **cuFFT / VkFFT** for 32+ channels: batched 32768-pt FFTs cost ~1 µs each on GPU but the upload/download
  (128 KB + 64 KB per channel per frame) and sync dominate below ~16 channels. Not worth it for stereo.
- **Sliding/recursive DFT** on the log grid: O(bins) per sample — 16384 × 735 per frame — far slower. No.

---

## 2. What the plan buys, cumulatively (stereo, 60 fps, cook thread)

| step | per cook (2 ch) | comment |
|---|---|---|
| v2.2.2 today | ≈ 85–100 µs + parameter reads | measured DSP 42–50 µs/ch |
| + FTZ/DAZ (1.2) | same, **no more silence spikes** | |
| + param polling ÷4 (1.3) | −15…−75 µs on 3 of 4 frames | measure first |
| + Bins 4096 (1.4) | ≈ 75 µs | warp/memcpy ÷4, TD downstream ÷4 |
| + async worker (1.1) | **≈ 10–15 µs** | FFT etc. moved off the cook thread |
| + N = 16384 cubic / AVX2 FFTW (1.7 / 1.8) | worker load ÷2 | irrelevant to the cook thread, matters for CPU headroom |

Target: the FFT CHOP disappears from the frame budget (< 1 % of a 60 fps frame for stereo), with the same picture.

---

## 3. Things that do NOT need changing (measured, so nobody re-optimizes them)

- Stage-level `if`s and the option system: 0.02 µs per bypassed stage.
- Window multiply, weighting, dB, ballistics, peak search: all < 5 µs combined and already AVX2.
- Aligned vs unaligned loads on MSVC: identical code generation (`vmovups`).
- `rsqrt`/LUT tricks: magnitude 3 µs, dB 3.5 µs — already in the noise next to the FFT.
- `cookEveryFrame` vs `cookEveryFrameIfAsked`: no cost difference; the latter just avoids cooking when unused.

---

## 4. Measurement protocol (before/after every step)

1. `fft_bench.exe --channels 2 --eq 0 --db 0 --weight 0 --ball 0` — DSP per stage (this file's numbers).
2. Info CHOP on the node: `cook_time_us` (inside the plugin), `param_fetch_us`, `param_reads`.
3. TouchDesigner Performance Monitor: the node's cook time. **TD time − cook_time_us = host overhead** (output
   handling, downstream, UI). If that gap dominates, 1.3/1.4 matter more than any DSP work.
4. Null-plugin floor: a CHOP that outputs zeros with the same Bins × channels measures what TD itself charges for
   a 16384-sample CHOP; nothing in this plugin can go below that number.
5. Silence test: feed digital silence and watch for cook-time spikes (denormals → 1.2).

---

## 5. Suggested order of implementation

1. **1.2 FTZ/DAZ** (10 lines) and **1.5 silence short-circuit** (20 lines) — zero risk.
2. **1.3 Param Poll** + **1.4 Bins guidance/Auto** — small, measurable in TD immediately.
3. **1.1 Async worker** — the real change; ~250 lines (mailbox, double buffer, worker loop, `Async` toggle,
   hold-on-late), plus tests (sequence/tearing, hold behaviour) and bench (`--async` mode measuring cook-thread cost).
4. **1.7 cubic warp + smaller default N** — visual A/B, then decide the default.
5. **1.8 FFT backend** — build FFTW 3.3.10 AVX2 or drop in pffft behind `IFFTEngine`; keep whichever benches faster.
6. **1.9 / 1.10** polish.
