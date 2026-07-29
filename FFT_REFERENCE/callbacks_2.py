# TouchDesigner cook script (Exec DAT callbacks) for live audio spectral analysis.
# Imports the analyser + BiquadEQ defined in Audio_Live2.py.
# Every cook frame, captures audio, applies EQ, computes a perceptual spectrum,
# applies optional equal-loudness weighting, decibel normalization, ballistics,
# and outputs the single 'RFFT' channel.

from Audio_Live2 import AudioSpectrumAnalyzer, BiquadEQ
import numpy as np

# ANALYSIS_SECONDS is the analysis TIME window (seconds).
ANALYSIS_SECONDS = 0.072

# Fixed default zero-pad length for high-density display interpolation
MAX_FFT_SIZE = 2**15

# Number of output bins written to the 'RFFT' channel
OUTPUT_BINS = 16384

# Top of the frequency range shown in the spectrum (Hz)
DISPLAY_MAX_HZ = 24000.0

# Default perceptual scale
SCALES = ('log',)

def get_sample_rate():
    try:
        rate = op('audiodevin1').rate
    except Exception:
        rate = 0
    try:
        rate = float(rate)
    except (TypeError, ValueError):
        rate = 0.0
    if not (8000 <= rate <= 384000):
        rate = 44100.0
    return rate

# State variables managed by rebuild_dsp()
sample_rate = 0
analyzer = None
eq_booster = None
fifo = None
buffer_capacity = 0
fft_size = MAX_FFT_SIZE

# Persistent spectrum memory across cook frames
_prev_spectrum = None
_prev_loudness = 'off'
_param_cache = {}

class FIFOBuffer:
    def __init__(self, capacity):
        self.capacity = max(1, int(capacity))
        self.data = np.zeros(self.capacity, dtype=np.float32)
        self.idx = 0
        self.filled = 0

    def add(self, signal):
        sig = np.asarray(signal, dtype=np.float32)
        n = sig.size
        if n >= self.capacity:
            self.data[:] = sig[-self.capacity:]
            self.idx = 0
            self.filled = self.capacity
            return
        end = self.idx + n
        if end <= self.capacity:
            self.data[self.idx:end] = sig
            self.idx = end
        else:
            first = self.capacity - self.idx
            self.data[self.idx:] = sig[:first]
            self.data[:n - first] = sig[first:]
            self.idx = n - first
        if self.filled < self.capacity:
            self.filled = min(self.capacity, self.filled + n)

    def get(self):
        if self.filled < self.capacity:
            out = np.zeros(self.capacity, dtype=np.float32)
            if self.filled > 0:
                ordered = np.concatenate((self.data[self.idx:], self.data[:self.idx]))
                out[self.capacity - self.filled:] = ordered[-self.filled:]
            return out
        if self.idx == 0:
            return self.data
        return np.concatenate((self.data[self.idx:], self.data[:self.idx]))

def rebuild_dsp(sr):
    global sample_rate, analyzer, eq_booster, fifo, buffer_capacity, fft_size
    sample_rate = sr
    buffer_capacity = max(1, int(round(ANALYSIS_SECONDS * sr)))
    fft_size = MAX_FFT_SIZE
    needed = 1
    while needed < buffer_capacity:
        needed *= 2
    if fft_size < needed:
        fft_size = needed
    fft_size = max(fft_size, 2)
    analyzer = AudioSpectrumAnalyzer(sr, display_max_hz=DISPLAY_MAX_HZ, n_output_bins=OUTPUT_BINS, scales=SCALES)
    eq_booster = BiquadEQ(sr)
    fifo = FIFOBuffer(buffer_capacity)

rebuild_dsp(get_sample_rate())

THIS_DAT = None

SCALE_MAP = {
    'log': ('log',),
    'mel': ('mel',),
    'erb': ('erb',),
    'bark': ('bark',),
    'chroma': ('chroma',),
    'linear': ('linear',),
    'melog': ('mel', 'log'),
}

def _par(name, default):
    dat = THIS_DAT if THIS_DAT is not None else me
    try:
        return dat.par[name].eval()
    except Exception:
        return default

def _par_fb(name, old_name, default):
    dat = THIS_DAT if THIS_DAT is not None else me
    for nm in (name, old_name):
        try:
            return dat.par[nm].eval()
        except Exception:
            continue
    return default

def onSetupParameters(scriptOp):
    global THIS_DAT
    THIS_DAT = scriptOp
    page = scriptOp.appendCustomPage('Spectrum')

    def add_menu(name, label, names, labels, value):
        p = page.appendMenu(name, label=label, replace=True)
        p[0].menuNames = names
        p[0].menuLabels = labels
        p[0].default = value
        try:
            scriptOp.par[name].val = value
        except Exception:
            pass

    def add_float(name, label, value, nmin, nmax):
        p = page.appendFloat(name, label=label, replace=True)
        p[0].default = value
        p[0].normMin = nmin
        p[0].normMax = nmax
        try:
            scriptOp.par[name].val = value
        except Exception:
            pass

    def add_int(name, label, value, nmin, nmax):
        p = page.appendInt(name, label=label, replace=True)
        p[0].default = value
        p[0].normMin = nmin
        p[0].normMax = nmax
        try:
            scriptOp.par[name].val = value
        except Exception:
            pass

    add_menu('Scale', 'Scale', ['log', 'mel', 'erb', 'bark', 'chroma', 'linear', 'melog'],
             ['Log', 'Mel', 'ERB', 'Bark', 'Chroma/Pitch', 'Linear', 'Mel+Log'], 'log')
    add_float('Displaymax', 'Display Max Hz', DISPLAY_MAX_HZ, 100.0, 48000.0)
    add_int('Bins', 'Output Bins', OUTPUT_BINS, 256, 32768)
    add_float('Warp', 'Warp Blend', 0.963, 0.0, 1.0)
    add_float('Logfloor', 'Log Floor Hz', 20.0, 1.0, 500.0)
    add_float('Winsec', 'Window Sec', ANALYSIS_SECONDS, 0.01, 0.5)

    # Equalizer controls
    add_float('Gaindb', 'High Boost dB', 6.0, -24.0, 24.0)
    add_float('Cutoffhz', 'High Cutoff Hz', 1000.0, 20.0, 20000.0)
    add_float('Lowgaindb', 'Low Boost dB', 0.0, -24.0, 24.0)
    add_float('Lowcutoffhz', 'Low Cutoff Hz', 200.0, 20.0, 5000.0)
    add_float('Q', 'EQ Q Factor', 0.707, 0.1, 4.0)
    add_float('Amount', 'EQ Blend Amount', 1.0, 0.0, 5.0)

    # Window & Weighting
    add_int('Kaiser', 'Kaiser Beta', 15, 1, 55)
    add_menu('Window', 'Window Type',
             ['kaiser', 'hann', 'hamming', 'blackman', 'blackmanharris'],
             ['Kaiser', 'Hann', 'Hamming', 'Blackman', 'Blackman-Harris'], 'kaiser')
    add_menu('Weighting', 'Loudness Weighting',
             ['off', 'a_weighting', 'c_weighting', 'itu_468'],
             ['Off', 'A-Weighting', 'C-Weighting', 'ITU-R 468'], 'off')

    # Loudness & Decibels
    add_menu('Loudness', 'Loudness Mode', ['off', 'db', 'db_norm'],
             ['Off (Magnitude)', 'dB (Decibels)', 'dB Normalized (0..1)'], 'off')
    add_float('Dbrange', 'dB Range Floor', 80.0, 10.0, 160.0)

    # Asymmetric Ballistics
    add_float('Attack', 'Attack Speed', 0.0, 0.0, 0.99)
    add_float('Release', 'Release Speed', 0.0, 0.0, 0.99)
    add_float('Smooth', 'Legacy Smoothing', 0.0, 0.0, 0.95)

    add_menu('Pad', 'Zero-Pad Len',
             ['1024', '2048', '4096', '8192', '16384', '32768', '65536'],
             ['1K', '2K', '4K', '8K', '16K', '32K', '64K'], '32768')
    page.appendPulse('Apply', label='Apply', replace=True)
    return

def onPulse(par):
    global analyzer, eq_booster, sample_rate, OUTPUT_BINS, SCALES, DISPLAY_MAX_HZ, _param_cache
    dat = par.owner
    scale_name  = dat.par['Scale'].eval()
    disp_max    = max(1.0, float(_par_fb('Displaymax', 'DisplayMax', DISPLAY_MAX_HZ)))
    n_out       = int(dat.par['Bins'].eval())
    warp        = float(dat.par['Warp'].eval())
    log_floor   = max(1.0, float(_par_fb('Logfloor', 'LogFloor', 20.0)))
    kaiser_beta = int(dat.par['Kaiser'].eval())
    window_type = dat.par['Window'].eval()
    weighting   = dat.par['Weighting'].eval()

    analyzer.scales         = SCALE_MAP.get(scale_name, ('log',))
    analyzer.display_max_hz = min(disp_max, sample_rate / 2.0)
    analyzer.n_output_bins  = n_out
    analyzer.warp_blend     = warp
    analyzer.log_floor_hz   = log_floor
    analyzer.weighting     = weighting
    analyzer.update_window(beta=kaiser_beta, window_type=window_type)

    eq_booster._design_key = None
    _param_cache.clear()
    return

def onCook(scriptOp):
    global buffer_capacity, fft_size, analyzer, fifo, eq_booster, sample_rate
    global ANALYSIS_SECONDS, OUTPUT_BINS, SCALES, DISPLAY_MAX_HZ
    global _prev_spectrum, _prev_loudness, _param_cache

    scale_name   = _par('Scale', 'log')
    disp_max     = max(1.0, float(_par_fb('Displaymax', 'DisplayMax', DISPLAY_MAX_HZ)))
    n_out        = int(_par('Bins', OUTPUT_BINS))
    warp         = float(_par('Warp', 0.963))
    log_floor    = max(1.0, float(_par_fb('Logfloor', 'LogFloor', 20.0)))
    win_sec      = max(1e-3, float(_par_fb('Winsec', 'WinSec', ANALYSIS_SECONDS)))
    gain_db      = float(_par_fb('Gaindb', 'GainDb', 6.0))
    cutoff_hz    = float(_par_fb('Cutoffhz', 'CutoffHz', 1000.0))
    low_gain_db  = float(_par('Lowgaindb', 0.0))
    low_cutoff_hz = float(_par('Lowcutoffhz', 200.0))
    q_factor     = float(_par('Q', 0.707))
    amount       = float(_par('Amount', 1.0))
    kaiser_beta  = int(_par('Kaiser', 15))
    window_type  = _par('Window', 'kaiser')
    weighting    = _par('Weighting', 'off')
    loudness     = _par('Loudness', 'off')
    db_range     = float(_par('Dbrange', 80.0))
    attack       = float(_par('Attack', 0.0))
    release      = float(_par('Release', 0.0))
    smooth       = float(_par('Smooth', 0.0))
    pad_choice   = int(_par('Pad', MAX_FFT_SIZE))

    # Use legacy smooth as release if release is 0
    if release == 0.0 and smooth > 0.0:
        release = smooth

    sr = get_sample_rate()
    rebuild = False
    if sr != sample_rate:
        rebuild = True
    if abs(win_sec - ANALYSIS_SECONDS) > 1e-9:
        ANALYSIS_SECONDS = win_sec
        rebuild = True
    if rebuild:
        rebuild_dsp(sr)
        _param_cache.clear()

    pc = _param_cache
    if (pc.get('scale_name') != scale_name or pc.get('disp_max') != disp_max or
        pc.get('n_out') != n_out or pc.get('warp') != warp or
        pc.get('log_floor') != log_floor or pc.get('kaiser_beta') != kaiser_beta or
        pc.get('window_type') != window_type or pc.get('weighting') != weighting):
        
        pc['scale_name']  = scale_name
        pc['disp_max']    = disp_max
        pc['n_out']       = n_out
        pc['warp']        = warp
        pc['log_floor']   = log_floor
        pc['kaiser_beta'] = kaiser_beta
        pc['window_type'] = window_type
        pc['weighting']   = weighting

        analyzer.scales         = SCALE_MAP.get(scale_name, ('log',))
        analyzer.display_max_hz = min(disp_max, sample_rate / 2.0)
        analyzer.n_output_bins  = n_out
        analyzer.warp_blend     = warp
        analyzer.log_floor_hz   = log_floor
        analyzer.weighting     = weighting
        analyzer.update_window(beta=kaiser_beta, window_type=window_type)

    eq_key = (gain_db, cutoff_hz, low_gain_db, low_cutoff_hz, q_factor, amount)
    if pc.get('eq_key') != eq_key:
        pc['eq_key'] = eq_key
        eq_booster._design_key = None

    scriptOp.isTimeSlice = False
    scriptOp.rate = sample_rate
    scriptOp.start = 0

    # Ensure single 'RFFT' channel exists
    if scriptOp['RFFT'] is None:
        scriptOp.clear()
        out_chan = scriptOp.appendChan('RFFT')
    else:
        out_chan = scriptOp['RFFT']

    # Read live audio input
    try:
        input_samples = scriptOp.inputs[0][0].numpyArray()
    except Exception:
        input_samples = np.zeros(buffer_capacity, dtype=np.float32)
    if input_samples is None or input_samples.size == 0:
        input_samples = np.zeros(buffer_capacity, dtype=np.float32)

    fifo.add(input_samples)
    captured_signal = fifo.get()

    # Apply biquad equalizer
    processed_signal = eq_booster.process_audio(
        captured_signal, gain_db=gain_db, cutoff_hz=cutoff_hz,
        low_gain_db=low_gain_db, low_cutoff_hz=low_cutoff_hz,
        q_factor=q_factor, amount=amount)

    if pc.get('pad_choice') != pad_choice or pc.get('buffer_capacity') != buffer_capacity:
        pc['pad_choice'] = pad_choice
        pc['buffer_capacity'] = buffer_capacity
        fft_size = max(pad_choice, buffer_capacity, 2)

    # Core spectrum calculation
    spectrum = analyzer.compute_padded_rfft(processed_signal, fft_size=fft_size, n_output_bins=n_out)
    spectrum = np.nan_to_num(np.asarray(spectrum, dtype=np.float64), nan=0.0, posinf=0.0, neginf=0.0)

    # Optional Loudness / dB conversion
    if loudness in ('db', 'db_norm'):
        spectrum = analyzer.power_to_db(spectrum, top_db=db_range, mode=loudness)
        if _prev_loudness not in ('db', 'db_norm'):
            _prev_spectrum = None
    else:
        if _prev_loudness in ('db', 'db_norm'):
            _prev_spectrum = None
    _prev_loudness = loudness

    # Apply asymmetric attack/release ballistics
    if (attack > 0.0 or release > 0.0) and _prev_spectrum is not None:
        spectrum = analyzer.apply_ballistics(spectrum, _prev_spectrum, attack=attack, release=release)
    _prev_spectrum = spectrum

    # Set sample count and write to single 'RFFT' channel
    out_vals = spectrum.astype(np.float32).flatten()
    n_out_samples = len(out_vals)
    if scriptOp.numSamples != n_out_samples:
        scriptOp.numSamples = n_out_samples
    
    out_chan = scriptOp['RFFT']
    out_chan.vals = out_vals

    return
