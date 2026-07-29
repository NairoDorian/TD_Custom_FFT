# Audio_Live2.py - High-Performance NumPy-Only Audio Spectrum Analyzer & DSP Engine for TouchDesigner
# Built for TouchDesigner (NumPy 2.1.2, SciPy-free, 60 FPS real-time processing)

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
# ---------------------------------------------------------------------------
class BiquadEQ:
    def __init__(self, sampling_rate):
        self.sampling_rate = float(sampling_rate)
        self._fir_h = np.zeros(1, dtype=np.float32)
        self._fir_xhist = np.zeros(0, dtype=np.float32)
        self._design_key = None

    def design_high_shelf(self, cutoff_hz, gain_db, q_factor=0.707):
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
        b0, b1, b2 = b
        a1, a2 = a[1], a[2]
        y = np.empty(max_len, dtype=np.float64)
        z1 = z2 = 0.0
        for n in range(max_len):
            x0 = 1.0 if n == 0 else 0.0
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
        x = np.asarray(audio_data, dtype=np.float32)
        h = self._fir_h
        Lm1 = h.size - 1
        if Lm1 == 0:
            return x * h[0]
        xx = np.concatenate((self._fir_xhist, x))
        yy = np.convolve(xx, h)[:xx.size]
        out = yy[Lm1:Lm1 + x.size]
        self._fir_xhist = x[-Lm1:]
        return out

    def process_audio(self, original_audio, gain_db=6.0, cutoff_hz=1000.0, low_gain_db=0.0, low_cutoff_hz=200.0, q_factor=0.707, amount=1.0):
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
                b = np.convolve(b_h, b_l)
                a = np.convolve(a_h, a_l)
            else:
                b, a = b_h, a_h
            self._build_fir(b, a)
            self._design_key = key

        filtered = self.apply_filter_time_domain(x)
        return x + amount * (filtered - x)

# Alias for backward compatibility
HighShelfFilter = BiquadEQ

# ---------------------------------------------------------------------------
# AudioSpectrumAnalyzer
# ---------------------------------------------------------------------------
# High-performance perceptual FFT spectrum analyzer and equalizer.
# Transforms raw audio frames into perceptual spectra (Log, Mel, ERB, Bark, Chroma, Linear),
# applying equal-loudness weighting, decibel normalization, and asymmetric attack/release ballistics.
# ---------------------------------------------------------------------------
class AudioSpectrumAnalyzer:
    def __init__(self, sr=44100, display_max_hz=20000.0, n_output_bins=16384,
                 scales=('log',), warp_blend=0.963, log_floor_hz=20.0,
                 window_type='kaiser', weighting='off'):
        self.sampling_rate = float(sr)
        self.display_max_hz = min(float(display_max_hz), self.sampling_rate / 2.0)
        self.n_output_bins = int(n_output_bins)
        self.scales = tuple(scales)
        self.warp_blend = float(warp_blend)
        self.log_floor_hz = float(log_floor_hz)
        self.window_type = window_type
        self.weighting = weighting

        self.analysis_window = None
        self._kaiser_beta = 15

        # Precomputed integer lookup tables for fast 1-gather perceptual warping
        self._warp_sig = None
        self._i0 = self._i1 = self._w = None
        self._target_hz_grid = None

        # Precomputed frequency weighting curve
        self._weighting_sig = None
        self._weighting_curve = None

    def _make_window(self, length):
        wt = self.window_type
        if wt == 'hann':
            w = np.hanning(length)
        elif wt == 'hamming':
            w = np.hamming(length)
        elif wt == 'blackman':
            w = np.blackman(length)
        elif wt == 'blackmanharris':
            a0, a1, a2, a3 = 0.35875, 0.48829, 0.14128, 0.01168
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
        if window_type is not None:
            self.window_type = window_type
        if beta is not None:
            self._kaiser_beta = beta
        if self.analysis_window is None:
            return
        self.analysis_window = self._make_window(len(self.analysis_window))

    # --- Perceptual Frequency Scale Math ------------------------------------
    def htk_hz_to_mel(self, hz):
        return 2595.0 * np.log10(1.0 + np.asarray(hz, dtype=float) / 700.0)

    def htk_mel_to_hz(self, mel):
        return 700.0 * (10.0 ** (np.asarray(mel, dtype=float) / 2595.0) - 1.0)

    def erb_rate_glasberg(self, hz):
        x = np.asarray(hz, dtype=float) / 123.0
        return 6.230 * (x ** 2) + 93.390 * x + 28.520

    def erb_rate_to_hz(self, erb):
        erb = np.asarray(erb, dtype=float)
        a, b, c = 6.230, 93.390, 28.520
        x = (-b + np.sqrt(np.maximum(0.0, b * b - 4 * a * (c - erb)))) / (2 * a)
        return x * 123.0

    def hz_to_bark(self, hz):
        f = np.asarray(hz, dtype=float)
        z = (26.81 * f) / (1960.0 + f) - 0.53
        z = np.where(z < 2.0, z + 0.15 * (2.0 - z), z)
        z = np.where(z > 20.1, z + 0.22 * (z - 20.1), z)
        return z

    def bark_to_hz(self, bark):
        z = np.asarray(bark, dtype=float)
        z = np.where(z < 2.0, (z - 0.3) / 0.85, z)
        z = np.where(z > 20.1, (z - 4.422) / 0.78, z)
        f = (1960.0 * (z + 0.53)) / (26.81 - (z + 0.53))
        return np.maximum(0.0, f)

    def hz_to_chroma(self, hz):
        f = np.maximum(1e-5, np.asarray(hz, dtype=float))
        return 12.0 * np.log2(f / 440.0) + 69.0

    def chroma_to_hz(self, chroma):
        m = np.asarray(chroma, dtype=float)
        return 440.0 * (2.0 ** ((m - 69.0) / 12.0))

    # --- Equal-Loudness Weighting Curves -----------------------------------
    def _compute_weighting_curve(self, freqs_hz, weighting_type):
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
        frame_length = len(audio_signal)
        if self.analysis_window is None or len(self.analysis_window) != frame_length:
            self.analysis_window = self._make_window(frame_length)

        windowed_frame = audio_signal * self.analysis_window
        padded_frame = np.zeros(fft_size, dtype=np.float32)
        pad_start = (fft_size - frame_length) // 2
        padded_frame[pad_start:pad_start + frame_length] = windowed_frame

        rfft = np.fft.rfft(padded_frame)
        magnitude_spectrum = np.abs(rfft)

        self._ensure_warp_tables(len(magnitude_spectrum))

        # Perform 1-gather perceptual interpolation
        spectrum = (magnitude_spectrum[self._i0] * (1.0 - self._w)
                    + magnitude_spectrum[self._i1] * self._w)

        # Apply frequency weighting curve if active
        if self.weighting != 'off':
            weights = self._compute_weighting_curve(self._target_hz_grid, self.weighting)
            spectrum = spectrum * weights

        return spectrum

    def _ensure_warp_tables(self, nlin):
        sig = (self.sampling_rate, self.display_max_hz, self.n_output_bins, nlin,
               self.scales, self.warp_blend, self.log_floor_hz)
        if self._warp_sig == sig:
            return

        target_hz = self._target_axis_hz(self.scales, self.display_max_hz,
                                         self.n_output_bins, self.sampling_rate / 2.0,
                                         self.warp_blend, self.log_floor_hz)
        self._target_hz_grid = target_hz
        nyquist = self.sampling_rate / 2.0
        frac = target_hz / nyquist * (nlin - 1)
        i0 = np.clip(np.floor(frac).astype(np.intp), 0, nlin - 2)
        self._i0, self._i1 = i0, i0 + 1
        self._w = (frac - i0).astype(np.float32)
        self._warp_sig = sig

    def _target_axis_hz(self, scales, fmax, n_out, nyquist, warp_blend=1.0, log_floor_hz=20.0):
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

        perceptual = np.mean(np.stack(grids), axis=0)
        linear = np.linspace(0.0, fmax, n_out)
        axis = (1.0 - warp_blend) * linear + warp_blend * perceptual
        return np.clip(axis, 0.0, fmax)

    # --- Magnitude -> Decibel (dB) & Loudness Normalization ---------------
    def power_to_db(self, S, ref=None, amin=1e-12, top_db=80.0, mode='db'):
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
        if prev_spectrum is None or prev_spectrum.shape != current_spectrum.shape:
            return current_spectrum

        att_coef = np.clip(attack, 0.0, 0.99)
        rel_coef = np.clip(release, 0.0, 0.99)

        diff = current_spectrum - prev_spectrum
        factor = np.where(diff > 0.0, 1.0 - att_coef, 1.0 - rel_coef)
        return prev_spectrum + factor * diff
