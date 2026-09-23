# TouchDesigner cook script (Exec DAT callbacks) for live audio spectral analysis.
# Imports the analyser + BiquadEQ defined in Audio_Live2.py.
# Every cook frame, captures audio, applies EQ, computes a perceptual spectrum,
# applies optional equal-loudness weighting, decibel normalization, ballistics,
# and outputs the single 'RFFT' channel.
#
# REFERENCE IMPLEMENTATION: this Exec DAT is the prototype the C++ plugin was derived from.
# Cross-references (by SYMBOL, for readers moving between the two):
#   onCook                 <-> FFT::execute + AnalysisPipeline::runChannel (source/FFT.cpp)
#   rebuild_dsp            <-> AnalysisPipeline::rebuild (source/AnalysisPipeline.cpp)
#   _param_cache key set   <-> WindowKey/WarpKey/WeightKey in AnalysisPipeline.h
#                             (any parameter feeding a table must join its cache key -
#                              this file does the same with explicit tuples per group)
#   _prev_spectrum         <-> DspState::prev_spectrum (ballistics memory)
#   get_sample_rate        <-> plan (sr is part of the FFT plan / window design)
# The C++ side adds what this file has no notion of: the async worker thread, TripleBuffer
# handoff, the wait-free cook path (~11us mean / 17us p99 at 16384 bins) and the popup /
# Info CHOP telemetry. Do not "port back" from here.

from Audio_Live2 import AudioSpectrumAnalyzer, BiquadEQ
import numpy as np

# ANALYSIS_SECONDS is the analysis TIME window (seconds).
# Default 0.072 s = ~72 ms of audio per frame; at 44.1 kHz that is ~3175 samples,
# which sets the FIFO capacity and thus the minimum fft_size (see rebuild_dsp).
# The C++ analogue is Parameters::Values::winMs converted to samples in the plan
# (AnalysisPipeline::rebuild), not a fixed compile-time constant.
ANALYSIS_SECONDS = 0.072

# Fixed default zero-pad length for high-density display interpolation
# 2**15 = 32768: zero-padding above the analysis length interpolates the display
# (finer sampling of the same spectrum - no new information, just smoother curves).
# The C++ side exposes this as the Pad menu (Parameters::Values::padChoice) and
# FFTW sizes the plan to max(pad, capacity).
MAX_FFT_SIZE = 2**15

# Number of output bins written to the 'RFFT' channel
# 16384 is the shipped default Output Bins (Parameters::Values::nOutputBins) and
# the bin count of the Info CHOP / output channel in the C++ plugin too.
OUTPUT_BINS = 16384

# Top of the frequency range shown in the spectrum (Hz)
# 24 kHz here; the C++ Display Max default is also 24000 (clamped to Nyquist in
# updateWarp). Chosen above the 20 kHz hearing limit so the whole audible range
# sits well inside the display even when the axis is warped.
DISPLAY_MAX_HZ = 24000.0

# Default perceptual scale
# 'log' matches Parameters::Values::scale = Scale::Log, the shipped default.
SCALES = ('log',)

def get_sample_rate():
    # Read the input CHOP's sample rate, with a safe fallback to 44100.
    # try/except around op() because the referenced audiodevin1 may not exist yet
    # (first cook / renamed node) - a missing operator must not raise into onCook.
    # Validity window 8000..384000 Hz mirrors the C++ plan's sanity clamp; anything
    # outside (or 0/NaN from a disconnected device) is treated as "use CD rate".
    # Cross-ref: FFT::getInputInfo / plan building reject the same out-of-range rates.
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
# These are module-level globals (not locals) so onCook / onPulse / rebuild_dsp
# share one DSP instance across cooks - the Python analogue of the node-owned
# AnalysisPipeline + DspState in FFT.h. rebuild_dsp() is the ONLY writer of the
# first six; onCook only reassigns fft_size when the Pad choice grows it.
sample_rate = 0
analyzer = None
eq_booster = None
fifo = None
buffer_capacity = 0
fft_size = MAX_FFT_SIZE

# Persistent spectrum memory across cook frames
# _prev_spectrum: ballistics state (DspState::prev_spectrum in the C++ plugin);
#                 None on first cook or after a loudness-mode change (see onCook).
# _prev_loudness: remembers last frame's loudness mode so a db<->magnitude switch
#                 can invalidate _prev_spectrum (units changed - smoothing across
#                 the boundary would produce one garbage frame).
# _param_cache:   change-detection dict; the onCook equivalent of the C++ cache
#                 keys - only push new values into the analyzer when a key moved.
_prev_spectrum = None
_prev_loudness = 'off'
_param_cache = {}

class FIFOBuffer:
    # Fixed-capacity circular buffer holding exactly the last `capacity` samples.
    # This is the Python prototype of the sample FIFO in FFT::ingest: latest
    # `capacity` samples win, older samples fall off the back. When a single add()
    # is larger than capacity, only the newest `capacity` samples are kept
    # (the data[:] = sig[-capacity:] branch) - "latest wins", same as TripleBuffer.
    def __init__(self, capacity):
        self.capacity = max(1, int(capacity))           # floor at 1: a 0-sample window is never a valid plan
        self.data = np.zeros(self.capacity, dtype=np.float32)
        self.idx = 0                                     # next write position (circular)
        self.filled = 0                                  # how many valid samples are stored (<= capacity)

    def add(self, signal):
        sig = np.asarray(signal, dtype=np.float32)
        n = sig.size
        if n >= self.capacity:
            # Overflow in one shot: keep only the tail (newest capacity samples),
            # reset the ring to "just wrote from the start". Matches the C++ FIFO's
            # behaviour when a block is larger than the window.
            self.data[:] = sig[-self.capacity:]
            self.idx = 0
            self.filled = self.capacity
            return
        end = self.idx + n
        if end <= self.capacity:
            # Common path: write fits contiguously before the end of the buffer.
            self.data[self.idx:end] = sig
            self.idx = end
        else:
            # Wrap path: split the write across the end and the start of the ring.
            first = self.capacity - self.idx
            self.data[self.idx:] = sig[:first]
            self.data[:n - first] = sig[first:]
            self.idx = n - first
        if self.filled < self.capacity:
            self.filled = min(self.capacity, self.filled + n)

    def get(self):
        # Oldest-to-newest view of the ring, always length `capacity`.
        # Before the ring is full: zero-pad the FRONT (oldest side) so the window
        # is always the same length - transient zeros at startup, same as the C++
        # pipeline starting with an empty FIFO. After full: rotate so idx is the
        # start (concatenate data[idx:] + data[:idx] = chronological order).
        if self.filled < self.capacity:
            out = np.zeros(self.capacity, dtype=np.float32)
            if self.filled > 0:
                ordered = np.concatenate((self.data[self.idx:], self.data[:self.idx]))
                out[self.capacity - self.filled:] = ordered[-self.filled:]
            return out
        if self.idx == 0:
            return self.data                            # already in order, no copy of the ring needed
        return np.concatenate((self.data[self.idx:], self.data[:self.idx]))

def rebuild_dsp(sr):
    # Rebuild the whole DSP chain for a new sample rate (or a new window length).
    # The Python analogue of AnalysisPipeline::rebuild: recompute capacity from
    # ANALYSIS_SECONDS * sr, pick the next power-of-two fft_size that fits the
    # window (FFTW's preferred sizes - the C++ plan uses fftwf_plan_dft_r2c_1d on
    # the same class of sizes), then throw away and recreate analyzer / EQ / FIFO.
    # Called at import time (line below) and again from onCook when sr or
    # Winsec changed - never on a normal cook (the _param_cache guards that).
    global sample_rate, analyzer, eq_booster, fifo, buffer_capacity, fft_size
    sample_rate = sr
    buffer_capacity = max(1, int(round(ANALYSIS_SECONDS * sr)))
    fft_size = MAX_FFT_SIZE
    needed = 1
    while needed < buffer_capacity:
        needed *= 2                                     # next power of two >= window length
    if fft_size < needed:
        fft_size = needed                               # never FFT smaller than the window we feed it
    fft_size = max(fft_size, 2)                         # rFFT needs at least 2 samples
    analyzer = AudioSpectrumAnalyzer(sr, display_max_hz=DISPLAY_MAX_HZ, n_output_bins=OUTPUT_BINS, scales=SCALES)
    eq_booster = BiquadEQ(sr)
    fifo = FIFOBuffer(buffer_capacity)

# Import-time initialization: build the chain once so the first onCook already
# has a valid analyzer even if no parameter callback has run yet.
rebuild_dsp(get_sample_rate())

THIS_DAT = None

SCALE_MAP = {
    # Menu token -> tuple of grids for AudioSpectrumAnalyzer._target_axis_hz.
    # 'melog' is the only multi-scale entry: mel + log averaged together (the C++
    # Scale menu's Mel+Log; _target_axis_hz stacks and means the grids).
    'log': ('log',),
    'mel': ('mel',),
    'erb': ('erb',),
    'bark': ('bark',),
    'chroma': ('chroma',),
    'linear': ('linear',),
    'melog': ('mel', 'log'),
}

def _par(name, default):
    # Read one custom parameter by name with a fallback default.
    # THIS_DAT is set in onSetupParameters so helpers work after the DAT is renamed
    # or before setup has run (falls back to the global `me`). try/except because
    # a missing/misspelled par name must not kill the cook - same philosophy as the
    # C++ Parameters::eval defaults: a bad parameter yields the default value.
    dat = THIS_DAT if THIS_DAT is not None else me
    try:
        return dat.par[name].eval()
    except Exception:
        return default

def _par_fb(name, old_name, default):
    # _par with a FALLBACK NAME: tries `name`, then `old_name`, then default.
    # Exists because some parameters were renamed (e.g. Displaymax vs DisplayMax);
    # older .toe files still carry the old names, so both are accepted on read.
    # The C++ plugin has no equivalent - its parameter names are fixed by
    # Parameters.cpp and never renamed without a migration note.
    dat = THIS_DAT if THIS_DAT is not None else me
    for nm in (name, old_name):
        try:
            return dat.par[nm].eval()
        except Exception:
            continue
    return default

def onSetupParameters(scriptOp):
    # TouchDesigner calls this ONCE when the DAT is created / first cooked:
    # stash THIS_DAT, then build the custom 'Spectrum' parameter page.
    # This page is the prototype of the C++ Parameters.cpp definition - same
    # parameter names (Scale, Displaymax, Bins, Warp, ...) and same defaults,
    # so a .toe configured against either implementation reads identically.
    # The add_menu / add_float / add_int closures are local builders: each
    # appends one parameter with a label, default and norm range, and sets the
    # live value so the page is usable immediately (try/except around .val for
    # the case where replace=True left a read-only stub).
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

    # --- Spectrum / axis ---
    # Scale: perceptual axis choice (see SCALE_MAP). Displaymax / Bins / Warp /
    # Logfloor / Winsec: the axis + plan parameters - defaults match
    # Parameters::Values (24000, 16384, 0.963, 20, 0.072s) and ANALYSIS_SECONDS.
    add_menu('Scale', 'Scale', ['log', 'mel', 'erb', 'bark', 'chroma', 'linear', 'melog'],
             ['Log', 'Mel', 'ERB', 'Bark', 'Chroma/Pitch', 'Linear', 'Mel+Log'], 'log')
    add_float('Displaymax', 'Display Max Hz', DISPLAY_MAX_HZ, 100.0, 48000.0)
    add_int('Bins', 'Output Bins', OUTPUT_BINS, 256, 32768)
    add_float('Warp', 'Warp Blend', 0.963, 0.0, 1.0)
    add_float('Logfloor', 'Log Floor Hz', 20.0, 1.0, 500.0)
    add_float('Winsec', 'Window Sec', ANALYSIS_SECONDS, 0.01, 0.5)

    # Equalizer controls - names/defaults mirror Parameters::Values:
    # gainDb 6.0, cutoffHz 1000, lowGainDb 0.0, lowCutoffHz 200, qFactor 0.707,
    # amount 1.0 (normMax 5.0 = the slider can exceed 1.0; eval does not clamp it,
    # and process_audio extrapolates linearly above 1.0 - same as the C++ amount).
    add_float('Gaindb', 'High Boost dB', 6.0, -24.0, 24.0)
    add_float('Cutoffhz', 'High Cutoff Hz', 1000.0, 20.0, 20000.0)
    add_float('Lowgaindb', 'Low Boost dB', 0.0, -24.0, 24.0)
    add_float('Lowcutoffhz', 'Low Cutoff Hz', 200.0, 20.0, 5000.0)
    add_float('Q', 'EQ Q Factor', 0.707, 0.1, 4.0)
    add_float('Amount', 'EQ Blend Amount', 1.0, 0.0, 5.0)

    # Window & Weighting - Kaiser Beta 1..55 (default 15, same floor/ceiling as
    # Parameters::Values::kaiserBeta), window type menu (C++ adds 'rectangular',
    # which falls through to kaiser in _make_window - see Audio_Live2.py),
    # weighting menu off/A/C/itu_468 (Weighting::Off is the shipped default;
    # a weighting curve is a measurement decision, not a display default).
    add_int('Kaiser', 'Kaiser Beta', 15, 1, 55)
    add_menu('Window', 'Window Type',
             ['kaiser', 'hann', 'hamming', 'blackman', 'blackmanharris'],
             ['Kaiser', 'Hann', 'Hamming', 'Blackman', 'Blackman-Harris'], 'kaiser')
    add_menu('Weighting', 'Loudness Weighting',
             ['off', 'a_weighting', 'c_weighting', 'itu_468'],
             ['Off', 'A-Weighting', 'C-Weighting', 'ITU-R 468'], 'off')

    # Loudness & Decibels - loudness off/db/db_norm maps to the C++ DbMode
    # (Magnitude / dB / Normalized); Dbrange 80 = the dB floor width, i.e.
    # top_db in power_to_db and DbRange in Parameters::Values.
    add_menu('Loudness', 'Loudness Mode', ['off', 'db', 'db_norm'],
             ['Off (Magnitude)', 'dB (Decibels)', 'dB Normalized (0..1)'], 'off')
    add_float('Dbrange', 'dB Range Floor', 80.0, 10.0, 160.0)

    # Asymmetric Ballistics - Attack/Release are the one-pole coefficients
    # (0..0.99, same clamp as FFTDSP::BallisticsFilter and Parameters::eval);
    # Smooth is a LEGACY knob: kept for old files, folded into release in onCook
    # when Release is still 0 (see the comment there). Defaults 0.0 = no
    # smoothing (follow instantly), not "fast smoothing".
    add_float('Attack', 'Attack Speed', 0.0, 0.0, 0.99)
    add_float('Release', 'Release Speed', 0.0, 0.0, 0.99)
    add_float('Smooth', 'Legacy Smoothing', 0.0, 0.0, 0.95)

    # Zero-pad length menu: the fft_size override (MAX_FFT_SIZE default lives in
    # the menu default '32768' = 2**15). Larger pad = smoother interpolated
    # display, no extra information. Apply pulse forces parameter re-read (onPulse).
    add_menu('Pad', 'Zero-Pad Len',
             ['1024', '2048', '4096', '8192', '16384', '32768', '65536'],
             ['1K', '2K', '4K', '8K', '16K', '32K', '64K'], '32768')
    page.appendPulse('Apply', label='Apply', replace=True)
    return

def onPulse(par):
    # Apply pulse: re-read every axis/window/weighting parameter from the UI and
    # push it into the analyzer in one shot, then invalidate both caches
    # (EQ _design_key = None forces a coefficient redesign; _param_cache.clear()
    # makes the next onCook re-sync everything). onCook already does the same
    # change-detection continuously - this pulse is the manual "force refresh"
    # for when a parameter was edited through a path onCook does not poll.
    # display_max_hz is clamped to Nyquist here (min with sample_rate/2) exactly
    # as updateWarp clamps Display Max in the C++ plugin.
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

    analyzer.scales         = SCALE_MAP.get(scale_name, ('log',))  # unknown token -> log, same fallback as onCook
    analyzer.display_max_hz = min(disp_max, sample_rate / 2.0)
    analyzer.n_output_bins  = n_out
    analyzer.warp_blend     = warp
    analyzer.log_floor_hz   = log_floor
    analyzer.weighting     = weighting
    analyzer.update_window(beta=kaiser_beta, window_type=window_type)

    eq_booster._design_key = None     # next process_audio redesigns the FIR
    _param_cache.clear()              # next onCook re-applies everything unconditionally
    return

def onCook(scriptOp):
    # THE PER-FRAME PATH - the Python prototype of FFT::execute (ingest) +
    # AnalysisPipeline::runChannel. Stage order, one cook:
    #   1. read params (the ~20 getPar* calls eval() does on the C++ side)
    #   2. legacy-smooth fold; rebuild if sr/Winsec moved
    #   3. change-detect via _param_cache / eq_key (the WindowKey/WarpKey rule)
    #   4. tag output CHOP (isTimeSlice=False, rate=sample_rate)
    #   5. ingest: read input -> fifo.add/get (latest capacity samples)
    #   6. EQ: process_audio (skip when flat/off - see BiquadEQ.process_audio)
    #   7. fft_size update if Pad/buffer grew
    #   8. compute_padded_rfft: window -> rFFT -> |X| -> warp -> weighting
    #   9. nan_to_num guard (FFTW can emit NaN on pathological input)
    #  10. optional power_to_db (clears _prev_spectrum on loudness-mode change)
    #  11. ballistics (apply_ballistics) + store _prev_spectrum
    #  12. write RFFT channel (resize numSamples if bin count changed)
    # Steady-state invariant to preserve when editing: steps 3's guards must
    # keep expensive rebuilds off the hot path - only a MOVED parameter may
    # trigger a redesign, never every cook.
    global buffer_capacity, fft_size, analyzer, fifo, eq_booster, sample_rate
    global ANALYSIS_SECONDS, OUTPUT_BINS, SCALES, DISPLAY_MAX_HZ
    global _prev_spectrum, _prev_loudness, _param_cache

    # --- 1. parameter snapshot (same names/defaults as onSetupParameters) ---
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
    # Back-compat only: files saved before Release existed carried Smooth as the
    # fall-side coefficient. Once the user touches Release (non-zero), Smooth is
    # ignored - never blend the two. The C++ plugin has no Smooth parameter; it
    # ships only Attack/Release.
    if release == 0.0 and smooth > 0.0:
        release = smooth

    # --- 2. rebuild the chain only when the plan inputs actually moved ---
    # sr change (device switch, rate edit) or Winsec change (window length) both
    # invalidate capacity, FIFO and window - same trigger set as
    # AnalysisPipeline::rebuild in the C++ plugin.
    sr = get_sample_rate()
    rebuild = False
    if sr != sample_rate:
        rebuild = True
    if abs(win_sec - ANALYSIS_SECONDS) > 1e-9:
        ANALYSIS_SECONDS = win_sec
        rebuild = True
    if rebuild:
        rebuild_dsp(sr)
        _param_cache.clear()          # full resync on the next cook

    # --- 3. change-detect: only touch the analyzer when a keyed value moved ---
    # _param_cache holds exactly the inputs that feed the warp/window tables -
    # the tuple encoding of WindowKey/WarpKey/WeightKey (any parameter that
    # reaches a table MUST appear in this key or the table goes stale).
    # Steady state: all keys equal -> the whole block is skipped.
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
        analyzer.display_max_hz = min(disp_max, sample_rate / 2.0)  # Nyquist clamp, as always
        analyzer.n_output_bins  = n_out
        analyzer.warp_blend     = warp
        analyzer.log_floor_hz   = log_floor
        analyzer.weighting     = weighting
        analyzer.update_window(beta=kaiser_beta, window_type=window_type)

    # EQ design key: all five coefficients inputs (+amount, which only affects
    # the blend not the FIR, but is cheap to compare here). A move forces
    # _design_key = None so process_audio rebuilds the truncated FIR once.
    # Analogue: FFTDSP::BiquadEQ::updateAndCheckActive returns true only on change.
    eq_key = (gain_db, cutoff_hz, low_gain_db, low_cutoff_hz, q_factor, amount)
    if pc.get('eq_key') != eq_key:
        pc['eq_key'] = eq_key
        eq_booster._design_key = None

    # --- 4. output CHOP tagging ---
    # isTimeSlice=False: this DAT produces a full spectrum per cook, not a rolling
    # time slice; rate = sample_rate keeps downstream timing honest.
    scriptOp.isTimeSlice = False
    scriptOp.rate = sample_rate
    scriptOp.start = 0

    # Ensure single 'RFFT' channel exists
    # One channel named RFFT (the C++ plugin emits one channel per analysis
    # channel, Mono Mix default = exactly this single channel).
    if scriptOp['RFFT'] is None:
        scriptOp.clear()
        out_chan = scriptOp.appendChan('RFFT')
    else:
        out_chan = scriptOp['RFFT']

    # Read live audio input
    # inputs[0][0] = first input CHOP, channel 0. All exception paths fall back
    # to a zero buffer of window length - a missing input must still produce a
    # valid-shaped spectrum (silence), never raise into TouchDesigner's cook.
    try:
        input_samples = scriptOp.inputs[0][0].numpyArray()
    except Exception:
        input_samples = np.zeros(buffer_capacity, dtype=np.float32)
    if input_samples is None or input_samples.size == 0:
        input_samples = np.zeros(buffer_capacity, dtype=np.float32)

    # --- 5. ingest: append to ring, read back the latest window ---
    fifo.add(input_samples)
    captured_signal = fifo.get()

    # Apply biquad equalizer
    # --- 6. EQ on the window (Python convolves the whole window; C++ FFT::ingest
    # applies the IIR sample-by-sample as data arrives - equivalent output,
    # different execution strategy; see Audio_Live2.BiquadEQ notes).
    processed_signal = eq_booster.process_audio(
        captured_signal, gain_db=gain_db, cutoff_hz=cutoff_hz,
        low_gain_db=low_gain_db, low_cutoff_hz=low_cutoff_hz,
        q_factor=q_factor, amount=amount)

    # --- 7. grow fft_size if the Pad choice or the window outgrew it ---
    # Only grows (max), never shrinks below the current plan - shrinking would
    # require a new rFFT and is left to rebuild_dsp.
    if pc.get('pad_choice') != pad_choice or pc.get('buffer_capacity') != buffer_capacity:
        pc['pad_choice'] = pad_choice
        pc['buffer_capacity'] = buffer_capacity
        fft_size = max(pad_choice, buffer_capacity, 2)

    # Core spectrum calculation
    # --- 8. window -> zero-pad -> rFFT -> |X| -> warp -> weighting ---
    spectrum = analyzer.compute_padded_rfft(processed_signal, fft_size=fft_size, n_output_bins=n_out)
    # --- 9. sanitize: NaN/Inf -> 0 so one bad bin cannot poison ballistics or
    # downstream CHOP->DAT conversion (the C++ side guards similarly before
    # publishing the result TripleBuffer slot).
    spectrum = np.nan_to_num(np.asarray(spectrum, dtype=np.float64), nan=0.0, posinf=0.0, neginf=0.0)

    # Optional Loudness / dB conversion
    # --- 10. magnitude -> dB (power_to_db, top_db = Dbrange). Mode changes
    # invalidate _prev_spectrum: smoothing across a units change (linear mag ->
    # dB or back) would produce one garbage frame - same reason DspState::prev
    # is cleared on mode/plan changes in the C++ pipeline.
    if loudness in ('db', 'db_norm'):
        spectrum = analyzer.power_to_db(spectrum, top_db=db_range, mode=loudness)
        if _prev_loudness not in ('db', 'db_norm'):
            _prev_spectrum = None
    else:
        if _prev_loudness in ('db', 'db_norm'):
            _prev_spectrum = None
    _prev_loudness = loudness

    # Apply asymmetric attack/release ballistics
    # --- 11. one-pole asymmetric smoother; skipped entirely when both
    # coefficients are 0 (the default) or on the first frame (_prev is None).
    # _prev_spectrum is stored AFTER the filter so the next cook smooths from
    # the smoothed value - the same recursion DspState::prev_spectrum implements.
    if (attack > 0.0 or release > 0.0) and _prev_spectrum is not None:
        spectrum = analyzer.apply_ballistics(spectrum, _prev_spectrum, attack=attack, release=release)
    _prev_spectrum = spectrum

    # Set sample count and write to single 'RFFT' channel
    # --- 12. resize only when the bin count actually changed (numSamples
    # assignment reallocates), then write the whole curve in one shot - the
    # Python analogue of FFT::getOutput writing the result block once per cook.
    out_vals = spectrum.astype(np.float32).flatten()
    n_out_samples = len(out_vals)
    if scriptOp.numSamples != n_out_samples:
        scriptOp.numSamples = n_out_samples
    
    out_chan = scriptOp['RFFT']
    out_chan.vals = out_vals

    return
