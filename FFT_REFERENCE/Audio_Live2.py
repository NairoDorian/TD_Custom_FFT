# Audio_Live2.py - High-Performance NumPy-Only Audio Spectrum Analyzer & DSP Engine for TouchDesigner
# Built for TouchDesigner (NumPy 2.1.2, SciPy-free, 60 FPS real-time processing)
#
# REFERENCE IMPLEMENTATION: the NumPy prototype the C++ plugin was derived from.
# Cross-references (by SYMBOL, for readers moving between the two):
#   BiquadEQ          <-> FFTDSP::BiquadEQ            (source/DSPModules.h, section 2)
#   AudioSpectrumAnalyzer._make_window <-> FFTDSP::WindowGenerator (section 3)
#   _target_axis_hz / warps           <-> FFTDSP::PerceptualWarping::computeTargetHzGrid (section 4)
#   _compute_weighting_curve          <-> FFTDSP::EqualLoudness::computeCurve (section 5)
#   power_to_db                       <-> FFTDSP::DecibelConverter::convertToDB (section 6c)
#   apply_ballistics                  <-> FFTDSP::BallisticsFilter (section 7)
#   compute_padded_rfft               <-> AnalysisPipeline::runChannel (AnalysisPipeline.cpp)
# The C++ side adds what this file has no notion of: the async worker, TripleBuffer
# handoff, cached-table keys and the wait-free cook path. Do not "port back" from here.

import numpy as np

# ---------------------------------------------------------------------------
# BiquadEQ (Upgraded from HighShelfFilter)
# ---------------------------------------------------------------------------
# Provides high-shelf, low-shelf, and spectral tilt audio equalization filters.
# Designed using classic Robert Bristow-Johnson (RBJ) audio EQ cookbook biquads.
# Applied via vectorised finite-impulse-response (FIR) truncation: the biquad's
# exponentially decaying impulse response is truncated to its effective length
# and applied using a single numpy convolve. Frame-to-frame click-free continuity
# is maintained by preserving an overlap history buffer (_fir_xhist).
#
# WHY AN FIR TRUNCATION RATHER THAN A DIRECT FORM II LOOP: NumPy has no fast
# per-sample IIR loop; truncating the impulse response once (in _build_fir) and
# convolving is O(n) with BLAS-speed inner products, and the _design_key guard
# means the expensive design only runs when a coefficient actually changed.
# The C++ port (FFTDSP::BiquadEQ) went back to a true per-sample Direct Form II
# Transposed IIR - one multiply-add per sample, state carried in the object -
# because on the cook thread that is cheaper than any convolution and needs no
# history buffer. Same RBJ coefficients; different execution strategy.
# ---------------------------------------------------------------------------
class BiquadEQ:
    def __init__(self, sampling_rate):
        self.sampling_rate = float(sampling_rate)
        self._fir_h = np.zeros(1, dtype=np.float32)       # truncated impulse response (the "filter")
        self._fir_xhist = np.zeros(0, dtype=np.float32)   # overlap history: last L-1 input samples, keeps convolve() continuous across frames
        self._design_key = None                           # cache key of the last (gain, cutoff, low-gain, low-cutoff, Q) design; None forces a redesign

    def design_high_shelf(self, cutoff_hz, gain_db, q_factor=0.707):
        # RBJ high-shelf biquad, coefficients normalised by a0 so a = [1, a1, a2].
        # A = 10^(gain_db/40) is the linear amplitude ratio (shelf gain is in dB/2 per the cookbook).
        # q_factor is floored at 0.01 to keep alpha (and therefore the poles) finite for a silly Q.
        # Cross-ref: the same formula lives in FFTDSP::BiquadEQ::designHighShelf (DSPModules.h).
        w0 = 2.0 * np.pi * cutoff_hz / self.sampling_rate
        A = 10.0 ** (gain_db / 40.0)
        alpha = np.sin(w0) / (2.0 * max(0.01, q_factor))
        cos_w0 = np.cos(w0)
        sqrt_A = np.sqrt(A)

        b0 = A * ((A + 1.0) + (A - 1.0) * cos_w0 + 2.0 * sqrt_A * alpha)
        b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cos_w0)
        b2 = A * ((A + 1.0) + (A - 1.0) * cos_w0 - 2.0 * sqrt_A * alpha)
        a0 = (A + 1.0) - (A - 1.0) * cos_w0 + 2.0 * sqrt_A * alpha
        a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cos_w0)
        a2 = (A + 1.0) - (A - 1.0) * cos_w0 - 2.0 * sqrt_A * alpha

        b = np.array([b0, b1, b2], dtype=np.float64) / a0
        a = np.array([1.0, a1 / a0, a2 / a0], dtype=np.float64)
        return b, a

    def design_low_shelf(self, cutoff_hz, gain_db, q_factor=0.707):
        # RBJ low-shelf biquad. Same structure as design_high_shelf with the shelf
        # tilted the other way (signs on the (A-1) terms flip). Not used by the C++
        # plugin's default path when Low Shelf is flat, but the parameter set matches
        # Parameters::Values::lowGainDb / lowCutoffHz in Parameters.h.
        w0 = 2.0 * np.pi * cutoff_hz / self.sampling_rate
        A = 10.0 ** (gain_db / 40.0)
        alpha = np.sin(w0) / (2.0 * max(0.01, q_factor))
        cos_w0 = np.cos(w0)
        sqrt_A = np.sqrt(A)

        b0 = A * ((A + 1.0) - (A - 1.0) * cos_w0 + 2.0 * sqrt_A * alpha)
        b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cos_w0)
        b2 = A * ((A + 1.0) - (A - 1.0) * cos_w0 - 2.0 * sqrt_A * alpha)
        a0 = (A + 1.0) + (A - 1.0) * cos_w0 + 2.0 * sqrt_A * alpha
        a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cos_w0)
        a2 = (A + 1.0) + (A - 1.0) * cos_w0 - 2.0 * sqrt_A * alpha

        b = np.array([b0, b1, b2], dtype=np.float64) / a0
        a = np.array([1.0, a1 / a0, a2 / a0], dtype=np.float64)
        return b, a

    def _build_fir(self, b, a, max_len=4096, tol=1e-7):
        # Emulate the biquad by stepping a Direct Form II recurrence over a unit impulse,
        # then TRUNCATE the tail where |y| first stays below tol * peak. Result: a finite
        # kernel _fir_h that apply_filter_time_domain can convolve with in one NumPy call.
        # max_len=4096 is the safety ceiling for a very low cutoff (long decay); tol=1e-7
        # is what keeps truncation error below float32's noise floor for typical settings.
        # Also sizes _fir_xhist to L-1 so the next frame's overlap is the right length.
        b0, b1, b2 = b
        a1, a2 = a[1], a[2]
        y = np.empty(max_len, dtype=np.float64)
        z1 = z2 = 0.0
        for n in range(max_len):
            x0 = 1.0 if n == 0 else 0.0          # unit impulse at n = 0
            y0 = b0 * x0 + z1
            z1 = b1 * x0 - a1 * y0 + z2
            z2 = b2 * x0 - a2 * y0
            y[n] = y0
        peak = np.max(np.abs(y)) or 1.0
        thresh = tol * peak
        idx = np.where(np.abs(y) > thresh)[0]
        L = int(idx[-1]) + 1 if idx.size else 1
        self._fir_h = y[:L].astype(np.float32)
        self._fir_xhist = np.zeros(L - 1, dtype=np.float32)

    def apply_filter_time_domain(self, audio_data):
        # Convolve with the truncated kernel, keeping the last L-1 inputs as history so
        # the filter is CONTINUOUS across cook frames (a fresh convolve each frame would
        # click at every block boundary). This overlap scheme is the Python-side
        # equivalent of the biquad's carried state in FFTDSP::BiquadEQ - the reason the
        # C++ ingest() applies the EQ to NEW samples only, in time order (see FFT::ingest).
        x = np.asarray(audio_data, dtype=np.float32)
        h = self._fir_h
        Lm1 = h.size - 1
        if Lm1 == 0:
            return x * h[0]                       # degenerate 1-tap "filter": pure gain, no history needed
        xx = np.concatenate((self._fir_xhist, x))
        yy = np.convolve(xx, h)[:xx.size]
        out = yy[Lm1:Lm1 + x.size]
        self._fir_xhist = x[-Lm1:]                # retain the tail for the next frame's overlap
        return out

    def process_audio(self, original_audio, gain_db=6.0, cutoff_hz=1000.0, low_gain_db=0.0, low_cutoff_hz=200.0, q_factor=0.707, amount=1.0):
        # One call: design (if needed) -> apply -> wet/dry blend.
        #   * digital-silence early-out (peak < 1e-5): identical to the C++ side's
        #     blockIsSilent / updateAndCheckActive short-circuits - silence costs nothing.
        #   * _design_key: only re-derives coefficients when a parameter moved; the C++
        #     equivalent is FFTDSP::BiquadEQ::updateAndCheckActive, which reports "flat or
        #     off, skip the whole pass" so EQ costs nothing when disabled.
        #   * low shelf is only folded in when |low_gain_db| > 0.01 (flat shelf = skip);
        #     convolving b/a of the two sections cascades them exactly.
        #   * amount is a straight linear blend: 0 = dry (bypass), 1 = fully filtered,
        #     and above 1 it keeps extrapolating - matching Parameters::Values::amount,
        #     whose slider reaches 5.0 and which eval() deliberately does not clamp.
        x = np.asarray(original_audio, dtype=np.float32)
        if x.size == 0:
            return x
        if np.max(np.abs(x)) < 1e-5:
            return np.zeros_like(x)

        key = (gain_db, cutoff_hz, low_gain_db, low_cutoff_hz, q_factor)
        if self._design_key != key:
            b_h, a_h = self.design_high_shelf(cutoff_hz, gain_db, q_factor)
            if abs(low_gain_db) > 0.01:
                b_l, a_l = self.design_low_shelf(low_cutoff_hz, low_gain_db, q_factor)
                b = np.convolve(b_h, b_l)         # cascade high * low = polynomial product of the sections
                a = np.convolve(a_h, a_l)
            else:
                b, a = b_h, a_h
            self._build_fir(b, a)
            self._design_key = key

        filtered = self.apply_filter_time_domain(x)
        return x + amount * (filtered - x)

# Alias for backward compatibility: older TOEs and scripts import HighShelfFilter
# by this name; process_audio's signature is unchanged for them.
HighShelfFilter = BiquadEQ

# ---------------------------------------------------------------------------
# AudioSpectrumAnalyzer
# ---------------------------------------------------------------------------
# High-performance perceptual FFT spectrum analyzer and equalizer.
# Transforms raw audio frames into perceptual spectra (Log, Mel, ERB, Bark, Chroma, Linear),
# applying equal-loudness weighting, decibel normalization, and asymmetric attack/release ballistics.
#
# This class is the prototype of AnalysisPipeline (source/AnalysisPipeline.h/.cpp):
# the same seven stages - window -> FFT -> magnitude -> warp -> weighting -> dB ->
# ballistics - in the same order. Key differences from the C++ port, for anyone
# reading both: here the warp/weighting tables are cached by a tuple signature
# (_warp_sig / _weighting_sig) rather than by the WindowKey/WarpKey/WeightKey
# structs in AnalysisPipeline.h (same rule, different encoding: any parameter that
# feeds a table MUST appear in its cache key, or the table goes stale); and there
# is no async worker - every call is synchronous on the calling thread.
# ---------------------------------------------------------------------------
class AudioSpectrumAnalyzer:
    def __init__(self, sr=44100, display_max_hz=20000.0, n_output_bins=16384,
                 scales=('log',), warp_blend=0.963, log_floor_hz=20.0,
                 window_type='kaiser', weighting='off'):
        # Defaults mirror Parameters::Values in Parameters.h so the reference and the
        # plugin start from the same configuration: warp_blend 0.963 (the taste default,
        # also hard-copied by tests/dsp_tests.cpp and bench/bench.cpp), log_floor 20 Hz
        # (conventional lowest audible frequency), Kaiser window, weighting off (flat -
        # a weighting curve is a measurement decision, not a display default).
        # display_max_hz is clamped to Nyquist here, exactly as updateWarp() clamps
        # Display Max in AnalysisPipeline.cpp - the axis can never claim bins the
        # input does not contain.
        self.sampling_rate = float(sr)
        self.display_max_hz = min(float(display_max_hz), self.sampling_rate / 2.0)
        self.n_output_bins = int(n_output_bins)
        self.scales = tuple(scales)
        self.warp_blend = float(warp_blend)
        self.log_floor_hz = float(log_floor_hz)
        self.window_type = window_type
        self.weighting = weighting

        self.analysis_window = None          # built lazily on the first compute_padded_rfft, length = frame length
        self._kaiser_beta = 15               # Kaiser Beta; 15 is the documented low-leakage default (DSPModules.h, WindowGenerator)

        # Precomputed integer lookup tables for fast 1-gather perceptual warping
        # _warp_sig is the cache key: a tuple of every input buildWarpTables depends on.
        # Same contract as WarpKey in AnalysisPipeline.h - miss a field and the table
        # silently keeps the old axis when that one parameter changes.
        self._warp_sig = None
        self._i0 = self._i1 = self._w = None  # linear-bin index pairs + fractional weight: one-gather lerp onto the target grid
        self._target_hz_grid = None           # the output axis in Hz (targetHz() in PerceptualWarping)

        # Precomputed frequency weighting curve
        # _weighting_sig keys on (len, top Hz, curve type) - the analogue of WeightKey,
        # which in the C++ side also carries myWarpVersion so a warp rebuild invalidates
        # the curve automatically. Here a change of axis moves freqs_hz[-1] or its length,
        # which is what plays that role.
        self._weighting_sig = None
        self._weighting_curve = None

    def _make_window(self, length):
        # Taper for one analysis window, then COHERENT-GAIN NORMALISED (divided by its
        # mean) so a sinusoid's measured magnitude is calibrated the same way whatever
        # the window shape - this is MagNorm::CoherentGain in Parameters.h, the shipped
        # default, and the same "mean(window) == 1" contract WindowGenerator::generateWindow
        # implements in DSPModules.h. (FullScale normalisation, the other mode, is a C++
        # -side option: sum(window) == 2, so a full-scale sine reads its own amplitude.)
        # Rectangular is reachable in C++ (WindowType::Rectangular) but not in this
        # prototype's menu - falling through to Kaiser here is intentional.
        wt = self.window_type
        if wt == 'hann':
            w = np.hanning(length)
        elif wt == 'hamming':
            w = np.hamming(length)
        elif wt == 'blackman':
            w = np.blackman(length)
        elif wt == 'blackmanharris':
            a0, a1, a2, a3 = 0.35875, 0.48829, 0.14128, 0.01168  # 92 dB sidelobe Blackman-Harris coefficients
            n = np.arange(length, dtype=np.float64)
            denom = max(1, length - 1)
            w = (a0 - a1 * np.cos(2 * np.pi * n / denom)
                 + a2 * np.cos(4 * np.pi * n / denom)
                 - a3 * np.cos(6 * np.pi * n / denom))
        else:  # 'kaiser'
            w = np.kaiser(length, self._kaiser_beta)
        
        # Coherent Gain Compensation: normalize by mean so sinusoidal magnitude calibration
        # remains identical regardless of window shape.
        mean_val = np.mean(w)
        return (w / mean_val).astype(np.float32) if mean_val > 0 else w.astype(np.float32)

    def update_window(self, beta=None, window_type=None):
        # Rebuild the taper in place when Kaiser Beta or the window type changed.
        # No-op when analysis_window is None (not yet built) - compute_padded_rfft will
        # build it with the current settings on the next call. The C++ equivalent is
        # AnalysisPipeline::updateWindow, which compares WindowKey first and returns
        # early in the steady state; here the guard is "is there a window to replace".
        if window_type is not None:
            self.window_type = window_type
        if beta is not None:
            self._kaiser_beta = beta
        if self.analysis_window is None:
            return
        self.analysis_window = self._make_window(len(self.analysis_window))

    # --- Perceptual Frequency Scale Math ------------------------------------
    # Forward/inverse pairs for each perceptual scale. Each pair maps Hz <-> a
    # quasi-linear "rate" axis (mel, ERB, bark, semitone) on which equal steps
    # approximate equal perceived pitch spacing. The C++ counterparts live in
    # FFTDSP::PerceptualWarping::computeTargetHzGrid (DSPModules.h, section 4)
    # and are the ones the shipped plugin actually runs; these are the reference
    # formulas the C++ tables were validated against.
    def htk_hz_to_mel(self, hz):
        # HTK mel: mel = 2595 * log10(1 + f/700). Piecewise-linear alternative exists
        # in the literature; HTK's log form is what the C++ Mel grid also uses.
        return 2595.0 * np.log10(1.0 + np.asarray(hz, dtype=float) / 700.0)

    def htk_mel_to_hz(self, mel):
        # Inverse of htk_hz_to_mel (algebraic rearrangement).
        return 700.0 * (10.0 ** (np.asarray(mel, dtype=float) / 2595.0) - 1.0)

    def erb_rate_glasberg(self, hz):
        # Glasberg & Moore ERB-rate: quadratic in f/123. One direction only is enough
        # for axis building (Hz -> ERB -> linspace -> Hz); the inverse is below.
        x = np.asarray(hz, dtype=float) / 123.0
        return 6.230 * (x ** 2) + 93.390 * x + 28.520

    def erb_rate_to_hz(self, erb):
        # Quadratic formula inverse of erb_rate_glasberg; maximum(0.0, ...) under the
        # sqrt guards against small negative radicands from float error, and clamps
        # non-physical negative rates to 0 Hz.
        erb = np.asarray(erb, dtype=float)
        a, b, c = 6.230, 93.390, 28.520
        x = (-b + np.sqrt(np.maximum(0.0, b * b - 4 * a * (c - erb)))) / (2 * a)
        return x * 123.0

    def hz_to_bark(self, hz):
        # Bark (Zwicker) with the standard endpoint corrections: the raw Tranter
        # formula drifts below z=2 and above z=20.1, so two linear nudges put the
        # ends back on the published curve. Chroma's C++ counterpart floor is 20 Hz;
        # Bark's natural bottom comes out near ~13 Hz (see axisBottom in status()).
        f = np.asarray(hz, dtype=float)
        z = (26.81 * f) / (1960.0 + f) - 0.53
        z = np.where(z < 2.0, z + 0.15 * (2.0 - z), z)
        z = np.where(z > 20.1, z + 0.22 * (z - 20.1), z)
        return z

    def bark_to_hz(self, bark):
        # Inverse of hz_to_bark, with the matching inverse corrections at both ends
        # and a floor at 0 Hz so the axis cannot go negative. The upper branch of hz_to_bark is
        # z' = 1.22*z - 4.422, so its inverse is (z + 4.422) / 1.22 - the same fix as the C++
        # (DSPModules.h, barkToHz); the old (z - 4.422) / 0.78 folded the axis over past ~6.5 kHz.
        z = np.asarray(bark, dtype=float)
        z = np.where(z < 2.0, (z - 0.3) / 0.85, z)
        z = np.where(z > 20.1, (z + 4.422) / 1.22, z)
        f = (1960.0 * (z + 0.53)) / (26.81 - (z + 0.53))
        return np.maximum(0.0, f)

    def hz_to_chroma(self, hz):
        # MIDI-semitone axis: 12 * log2(f / 440) + 69, i.e. A440 = note 69. The
        # reference builds its grid from 20 Hz up (see _target_axis_hz); the C++
        # computeTargetHzGrid floors Chroma at 20 Hz for the same reason - it is the
        # conventional lowest audible frequency, not a derived constant.
        f = np.maximum(1e-5, np.asarray(hz, dtype=float))
        return 12.0 * np.log2(f / 440.0) + 69.0

    def chroma_to_hz(self, chroma):
        # Inverse of hz_to_chroma (exponentiate the semitone offset back to Hz).
        m = np.asarray(chroma, dtype=float)
        return 440.0 * (2.0 ** ((m - 69.0) / 12.0))

    # --- Equal-Loudness Weighting Curves -----------------------------------
    def _compute_weighting_curve(self, freqs_hz, weighting_type):
        # Per-bin gain curve for the selected equal-loudness weighting, cached on
        # (length, top Hz, type) - see the _weighting_sig note in __init__.
        # All three curves are normalised so 1 kHz reads unity (A and C divide by
        # their own 1 kHz response; ITU-R 468 is already a ratio). Off/unknown types
        # fall through to ones - the same "w starts at 1.0, flat is the fallback"
        # contract EqualLoudness::computeCurve has in DSPModules.h, so a forgotten
        # curve code silently weights FLAT rather than producing garbage.
        #   a_weighting : IEC 61672 auditory filter pair, the conventional dBA.
        #   c_weighting : the flatter high-SPL curve, dBC.
        #   itu_468     : ITU-R 468 noise-weighting, the broadcasting/noise standard.
        sig = (len(freqs_hz), float(freqs_hz[-1]), weighting_type)
        if self._weighting_sig == sig:
            return self._weighting_curve

        f = np.maximum(1e-5, np.asarray(freqs_hz, dtype=np.float64))
        if weighting_type == 'a_weighting':
            f2 = f ** 2
            num = (12194.0 ** 2) * (f2 ** 2)
            den = (f2 + 20.6 ** 2) * np.sqrt((f2 + 107.7 ** 2) * (f2 + 737.9 ** 2)) * (f2 + 12194.0 ** 2)
            ra = num / np.maximum(1e-12, den)
            f1k = 1000.0 ** 2
            ra_1k = ((12194.0 ** 2) * (f1k ** 2)) / ((f1k + 20.6 ** 2) * np.sqrt((f1k + 107.7 ** 2) * (f1k + 737.9 ** 2)) * (f1k + 12194.0 ** 2))
            weights = ra / ra_1k
        elif weighting_type == 'c_weighting':
            f2 = f ** 2
            num = (12194.0 ** 2) * f2
            den = (f2 + 20.6 ** 2) * (f2 + 12194.0 ** 2)
            rc = num / np.maximum(1e-12, den)
            f1k = 1000.0 ** 2
            rc_1k = ((12194.0 ** 2) * f1k) / ((f1k + 20.6 ** 2) * (f1k + 12194.0 ** 2))
            weights = rc / rc_1k
        elif weighting_type == 'itu_468':
            # Two fitted polynomials (h1, h2) approximate the ITU-R 468 auditory
            # filter magnitude; the ratio is already referenced to its own curve so
            # no 1 kHz normalisation divide is needed.
            h1 = -4.737338981378384e-24 * (f ** 6) + 2.043828333266122e-15 * (f ** 4) - 1.363894795463638e-7 * (f ** 2) + 1.0
            h2 = 1.306612257412824e-19 * (f ** 5) - 2.118150887518656e-11 * (f ** 3) + 5.559488023498642e-4 * f
            r_itu = (1.246332637532143e-4 * f) / np.sqrt(np.maximum(1e-12, h1 ** 2 + h2 ** 2))
            weights = r_itu
        else:
            weights = np.ones_like(f)

        self._weighting_curve = weights.astype(np.float32)
        self._weighting_sig = sig
        return self._weighting_curve

    # --- Core Padded RFFT Computation --------------------------------------
    def compute_padded_rfft(self, audio_signal, fft_size, n_output_bins=16384):
        # THE CORE - the Python analogue of AnalysisPipeline::runChannel's steps 1-4:
        #   1. window     - multiply the frame by the taper (built lazily at frame length)
        #   2. zero-pad   - centre the windowed frame in a fft_size buffer (pad_start
        #                   centres it; the C++ rebuild() rounds the offset down to a
        #                   multiple of 8 to keep the SIMD input pointer aligned)
        #   3. rFFT + |X| - np.fft.rfft gives N/2+1 complex bins; abs() is the magnitude
        #   4. warp       - one-gather linear interpolation of the magnitudes onto the
        #                   perceptual axis (the _i0/_i1/_w tables from _ensure_warp_tables)
        #   5. weighting  - multiply by the equal-loudness curve when not 'off'
        # dB conversion and ballistics are deliberately NOT here: callbacks_2.py applies
        # them afterwards, mirroring how runChannel stages 5-6 sit after the warp.
        # n_output_bins is accepted for signature compatibility with callers but the
        # axis actually used is self.n_output_bins (set via constructor / onPulse).
        frame_length = len(audio_signal)
        if self.analysis_window is None or len(self.analysis_window) != frame_length:
            self.analysis_window = self._make_window(frame_length)

        windowed_frame = audio_signal * self.analysis_window
        padded_frame = np.zeros(fft_size, dtype=np.float32)
        pad_start = (fft_size - frame_length) // 2      # centre the frame: keeps transients off the FFT edges
        padded_frame[pad_start:pad_start + frame_length] = windowed_frame

        rfft = np.fft.rfft(padded_frame)
        magnitude_spectrum = np.abs(rfft)

        self._ensure_warp_tables(len(magnitude_spectrum))

        # Perform 1-gather perceptual interpolation
        # Linear (2-tap) lerp between adjacent linear FFT bins - WarpInterp::Linear in
        # Parameters.h. The C++ port adds a Cubic Catmull-Rom (4-tap) option for
        # smoother lobes on smaller FFTs; only the linear form exists here.
        spectrum = (magnitude_spectrum[self._i0] * (1.0 - self._w)
                    + magnitude_spectrum[self._i1] * self._w)

        # Apply frequency weighting curve if active
        if self.weighting != 'off':
            weights = self._compute_weighting_curve(self._target_hz_grid, self.weighting)
            spectrum = spectrum * weights

        return spectrum

    def _ensure_warp_tables(self, nlin):
        # Build (or reuse) the index/weight tables that map each output bin onto the
        # linear FFT spectrum. The signature tuple is the whole cache key - every
        # input buildWarpTables depends on must appear, or a change to a missing one
        # leaves a stale axis (the WindowKey/WarpKey rule from AnalysisPipeline.h,
        # encoded as a tuple instead of a struct). nlin = len(magnitude_spectrum) =
        # fft_size/2 + 1 enters the key because the frac formula is normalised by it.
        sig = (self.sampling_rate, self.display_max_hz, self.n_output_bins, nlin,
               self.scales, self.warp_blend, self.log_floor_hz)
        if self._warp_sig == sig:
            return

        target_hz = self._target_axis_hz(self.scales, self.display_max_hz,
                                         self.n_output_bins, self.sampling_rate / 2.0,
                                         self.warp_blend, self.log_floor_hz)
        self._target_hz_grid = target_hz
        nyquist = self.sampling_rate / 2.0
        # frac = where this output bin sits, expressed in linear-FFT-bin units;
        # i0/i1 bracket it and w is the fraction between - the gather/lerp operands.
        frac = target_hz / nyquist * (nlin - 1)
        i0 = np.clip(np.floor(frac).astype(np.intp), 0, nlin - 2)   # clipped so i1 = i0+1 is always a valid index
        self._i0, self._i1 = i0, i0 + 1
        self._w = (frac - i0).astype(np.float32)
        self._warp_sig = sig

    def _target_axis_hz(self, scales, fmax, n_out, nyquist, warp_blend=1.0, log_floor_hz=20.0):
        # Build the output frequency axis: one grid per requested scale, averaged
        # together, then BLENDED with the linear axis by warp_blend - the same
        # (1-blend)*linear + blend*perceptual construction PerceptualWarping does.
        #   blend 0 -> pure linear (Scale ignored, the identity/warp-bypass case)
        #   blend 1 -> pure perceptual
        #   default 0.963 -> mostly perceptual, a touch of linear (Parameters::Values::warp)
        # Every grid ends exactly on fmax, so the reported axis rate is 2*fmax - the
        # invariant AnalysisPipeline::updateWarp and RateModel::axisRate rely on.
        # Bottom ends differ per scale: Log starts at log_floor (default 20 Hz),
        # Mel/ERB/Linear at 0, Chroma at 20, Bark near ~13 - see status()'s axisBottom.
        grids = []
        for sc in scales:
            if sc == 'log':
                floor = max(1.0, float(log_floor_hz))
                fmin = min(floor, max(1.0, fmax * 0.1))
                fmin = min(fmin, 0.5 * fmax)
                grids.append(np.geomspace(fmin, fmax, n_out))
            elif sc == 'mel':
                p = self.htk_hz_to_mel(np.array([0.0, fmax]))
                grids.append(self.htk_mel_to_hz(np.linspace(p[0], p[1], n_out)))
            elif sc == 'erb':
                p = self.erb_rate_glasberg(np.array([0.0, fmax]))
                grids.append(self.erb_rate_to_hz(np.linspace(p[0], p[1], n_out)))
            elif sc == 'bark':
                p = self.hz_to_bark(np.array([0.0, fmax]))
                grids.append(self.bark_to_hz(np.linspace(p[0], p[1], n_out)))
            elif sc == 'chroma':
                p = self.hz_to_chroma(np.array([20.0, fmax]))
                grids.append(self.chroma_to_hz(np.linspace(p[0], p[1], n_out)))
            else:  # 'linear'
                grids.append(np.linspace(0.0, fmax, n_out))

        perceptual = np.mean(np.stack(grids), axis=0)   # multi-scale request (e.g. melog = mel+log) averages the grids
        linear = np.linspace(0.0, fmax, n_out)
        axis = (1.0 - warp_blend) * linear + warp_blend * perceptual
        return np.clip(axis, 0.0, fmax)                 # clamp: blend arithmetic can overshoot fmax by float error

    # --- Magnitude -> Decibel (dB) & Loudness Normalization ---------------
    def power_to_db(self, S, ref=None, amin=1e-12, top_db=80.0, mode='db'):
        # Magnitude -> dB, matching FFTDSP::DecibelConverter::convertToDB (DSPModules.h
        # section 6c): 20*log10(S/ref), floor at ref - top_db, and (for mode='db_norm')
        # rescale that window into 0..1. ref defaults to the frame peak, i.e.
        # DbRef::FramePeak - the previous version's behaviour and still the shipped
        # default in Parameters::Values::dbRef. amin keeps log10 away from 0/-inf.
        # mode 'db' stops at the floor clamp; 'db_norm' additionally maps to [0, 1].
        S = np.asarray(S, dtype=np.float64)
        np.maximum(amin, S, out=S)
        if ref is None:
            ref = S.max()
            if ref <= 0:
                ref = 1.0

        S /= ref
        np.log10(S, out=S)
        S *= 20.0

        if top_db is not None:
            peak = S.max()
            floor = peak - top_db
            np.maximum(S, floor, out=S)
            if mode == 'db_norm':
                S = (S - floor) / max(1e-6, top_db)
                np.clip(S, 0.0, 1.0, out=S)

        return S

    # --- Asymmetric Attack / Release Ballistics ---------------------------
    def apply_ballistics(self, current_spectrum, prev_spectrum, attack=0.0, release=0.0):
        # One-pole asymmetric smoother, exactly FFTDSP::BallisticsFilter's contract:
        # rising edges use (1 - attack), falling edges use (1 - release), coefficients
        # clamped to 0..0.99 (1.0 would freeze the state forever - the reason
        # Parameters::eval clamps the coefficient sliders there too). attack=release=0
        # means "follow instantly" (factor = 1), which is the shipped default: no
        # smoothing, not fast smoothing (see Parameters::Values::attack/release).
        # Shape-mismatch (first frame, or bins changed) returns the input untouched -
        # the C++ side clears DspState::prev_spectrum at the same events instead.
        if prev_spectrum is None or prev_spectrum.shape != current_spectrum.shape:
            return current_spectrum

        att_coef = np.clip(attack, 0.0, 0.99)
        rel_coef = np.clip(release, 0.0, 0.99)

        diff = current_spectrum - prev_spectrum
        factor = np.where(diff > 0.0, 1.0 - att_coef, 1.0 - rel_coef)  # per-bin: rise vs fall choose their own coefficient
        return prev_spectrum + factor * diff
