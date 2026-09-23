#ifndef PARAMETERS_H
#define PARAMETERS_H

/*
===========================================================================
                  PARAMETER MANAGEMENT SYSTEM
===========================================================================
Header File: Parameters.h

Follows the official TouchDesigner 2025/2026 "cppParsTemplateGen" layout:
  - constexpr parameter names / labels
  - enum classes for every menu (menu order is the single source of truth)
  - Parameters::eval() fetches every parameter ONCE per cook into a plain struct
  - Parameters::setup() registers the UI

Adding a menu entry:  add it to the enum AND to the names/labels arrays in
Parameters.cpp in the same position; static_asserts keep them in sync.
===========================================================================
*/

/*
---------------------------------------------------------------------------
HOW THIS FILE AND Parameters.cpp FIT TOGETHER (plain version)
---------------------------------------------------------------------------
Two files, two jobs:

  Parameters.h  (this file)  declares WHAT the parameters are: the menu enums,
                             the name/label text, the hard limits, and the
                             Values struct that holds one cook's readings.
  Parameters.cpp             does the two things that need code: registers the
                             UI with TouchDesigner (setup) and reads the current
                             values into a Values struct (eval).

WHAT "cppParsTemplateGen" IS: it is a Palette component inside TouchDesigner
(see the CustomOperatorSamples README and docs.derivative.ca/Palette:
cppParsTemplateGen) that generates a Parameters class mimicking the custom
parameters of a reference COMP. The official generated layout is a set of
`constexpr char <Name>Name[] / <Label>Label[]` pairs, a #pragma region for the
menus, and a setup() plus one evalX() accessor per parameter. This project keeps
the naming convention (Name/Label pairs, setup(), eval()) but collapses the
per-parameter accessors into one eval() that fills a Values struct, because a
cook needs all the parameters at once and ~18 separate virtual calls would cost
more than one pass. The official layout is a starting point here, not a
standard this file is measured against.

THE PARAMETER PAGES (the strings passed as `page` in Parameters.cpp::setup -
they are the tab names in the parameter dialog):
  Spectrum               the analysis itself: channels, Scale, Display Max,
                         Output Bins, Warp Blend + interpolation, Log Floor,
                         window length (mode + samples/ms), Zero-Pad Len, FFT Planner.
  EQ                     an optional pre-filter: enable, high/low shelf on/off,
                         gain + cutoff each, Q, and wet/dry Amount.
  Window & Weighting     the taper (Window Type, Kaiser Beta) and the
                         equal-loudness curve (Weighting, Magnitude Normalization).
  Loudness & Ballistics  the display unit (Loudness Mode, dB Reference, dB Range
                         Floor), the smoother (Ballistics Enable/Mode, attack and
                         release in either unit) and the Reset pulse.
  Performance            the async worker thread and which FFT library is used.

HOW THE NAME/LABEL CONSTANT PAIRS RELATE TO THE Values STRUCT (verified, not
assumed): in this file the Name/Label pairs are declared in the same order as
the corresponding fields in Values, group for group (ChanmodeName .. PlannerName
mirrors chanMode .. planner, and so on down to FftbackendName / backend). Three
things are worth being precise about, because it is easy to overstate this:

  * The order alignment is a READING AID, not an enforced rule. eval() in
    Parameters.cpp fetches every parameter by its NAME string, so shuffling the
    Values fields would not break anything functionally - it would only make the
    two files stop reading alike.
  * What IS enforced is a different pairing: each menu array in Parameters.cpp
    (kScaleNames and so on) is tied to its enum's COUNT by a static_assert, and
    the array's ORDER is the menu's order, which must equal the enum's order
    because the stored parameter value is the array index. Reorder an enum or an
    array without the other and the static_assert still passes while every menu
    shows the wrong label - the asserts catch a count mismatch, not a reorder.
  * What must never change is the TEXT of a Name. TouchDesigner identifies a
    custom parameter by that string, so a saved .toe stores it and setup() looks
    it up again on load: a rename orphans the stored value. The one place in this
    repo that refers to a parameter by name outside Parameters.cpp is
    FFT::pulsePressed(), which compares the incoming pulse name against
    Parameters::ResetName. (The Info DAT, popup and Info CHOP callbacks are NOT
    such a place - they read the Values snapshot and the cached telemetry, and
    never call into the parameter system at all - but a script or an exported
    parameter list outside this repo can still hold the old string.)

HOW TO CHANGE A MENU: add the entry to the enum here, add the matching string to
both arrays in Parameters.cpp at the same POSITION, then add the case in the
switch/if-chain named in that enum's comment below. The enum's comment says which
file that is and what happens silently if you forget it.
---------------------------------------------------------------------------
*/

#include "CHOP_CPlusPlusBase.h"

namespace Parameters {

// ---------------------------------------------------------------------------
// Menus (order == menu order in Parameters.cpp)
// ---------------------------------------------------------------------------
// A menu's stored value is the INDEX of the entry, so the enum order IS the menu
// order and the two cannot be reordered independently. Every entry below names what
// it means and the one other place that must gain a matching case. Each of these
// enums also needs its names+labels arrays in Parameters.cpp extended at the same
// position; for all 11 menus that is enforced by the static_asserts there, and they
// are the only thing that catches a forgotten array entry. The COUNT sentinel is
// not a menu entry - it is the array length the asserts compare against. eval() rejects
// an out-of-range stored value for every one of these menus (its menu() helper falls
// back on value < 0 or value >= COUNT), so a corrupt parameter file cannot select a
// non-existent entry; the Pad index, which has no enum, gets the same treatment from
// its own range check.
//
// WHAT ALL OF THESE HAVE IN COMMON when a case is missing: almost every consumer
// switch in DSPModules.h / AnalysisPipeline.cpp has a default arm that means "first
// or safest entry", so a forgotten case does not fail - it silently behaves as some
// other entry. The per-enum notes below say which one.

// How the FFT's linear frequency grid is retargeted onto the output bins.
// Log = log-spaced (musical), Mel/ERB/Bark = perceptual bands, Chroma = pitch
// classes (a log2 axis from a fixed 20 Hz floor), Linear = no retargeting at all,
// Melog = a 50/50 blend of Mel and Log used as the "smoothed log" default family.
// ADDING ONE: give computeTargetHzGrid() in source/DSPModules.h a `case` for the
//   new code, or the new scale silently produces a LINEAR grid (its `case 5:
//   default:` arm). The warp blend still applies, so the symptom is a menu entry
//   that looks like Scale = Linear rather than an error. Also update the
//   Scale == Linear check in FFT.cpp (the "uniform grid" Info label).
enum class Scale : int { Log = 0, Mel, ERB, Bark, Chroma, Linear, Melog, COUNT };

// The taper multiplied over each analysis window before the transform. Kaiser is
// the default because it is the only one with a continuous leakage control
// (Kaiser Beta); Rectangular is no taper at all. See the WindowGenerator section
// comment in DSPModules.h for what each one is for and why the coefficients are
// normalized by measurement.
// ADDING ONE: give the switch in WindowGenerator::generateWindow()
//   (source/DSPModules.h) a `case` matching the new position, or the entry
//   silently builds a KAISER window - that switch's `case 0: default:` arm is
//   Kaiser, and the "HOW TO CHANGE - adding a window type" checklist at the head of
//   the WindowGenerator section in DSPModules.h records the same warning.
enum class WindowType : int { Kaiser = 0, Hann, Hamming, Blackman, BlackmanHarris, Rectangular, COUNT };

// The equal-loudness curve multiplied into the spectrum. Off = flat (no curve).
// See the EqualLoudness section in DSPModules.h; A and C are physical filter
// responses normalized to 0 dB at 1 kHz, ITU-R 468 is a fitted polynomial pair.
// ADDING ONE: add a branch to EqualLoudness::computeCurve() (source/DSPModules.h).
//   Its per-bin `w` starts at 1.0 and is commented as "the fallback for an unknown
//   code", so a forgotten branch silently weights FLAT - identical to Off.
enum class Weighting : int { Off = 0, AWeighting, CWeighting, ITU468, COUNT };

// The unit the spectrum is displayed in. Off = raw linear magnitude, Db =
// decibels against a reference, DbNorm = decibels rescaled into 0.0..1.0.
// See DecibelConverter::convertToDB() in DSPModules.h.
// ADDING ONE: convertToDB() branches on `mode == 0` (pass through) and
//   `mode == 2` (0..1 rescale) and treats EVERYTHING ELSE as plain dB, so a new
//   entry silently becomes another dB mode unless convertToDB and
//   AnalysisPipeline::runChannel() both learn about it.
enum class Loudness : int { Off = 0, Db, DbNorm, COUNT };

// Which of the two window-length parameters is live: Samples reads Winsamples,
// Milliseconds reads Winms and converts with the input rate. Both fields exist in
// Values at all times; eval() only refreshes the one this selects.
// ADDING ONE: windowSamplesFrom() in source/RateModel.h is an if/else on
//   Milliseconds and returns the raw sample count otherwise, so a new mode
//   silently behaves as Samples. There is no case to add anywhere else - the
//   conversion is that one function.
enum class WinMode : int { Samples = 0, Milliseconds, COUNT };

// How hard FFTW is asked to work when it plans a transform of a new size: Auto =
// instant plan, upgraded by a background thread; Fast = estimate only; Measured =
// block once per size; Patient = like Auto but the background upgrade is
// FFTW_PATIENT. All four are cached in a wisdom file, so only the first run of a
// size pays.
// ADDING ONE: this enum and FFTDSP::PlannerPolicy in source/DSPModules.h are the
//   same numbering - AnalysisPipeline.cpp casts one to the other - but NOTHING
//   asserts that (contrast Backend, which is pinned by static_assert). Add the
//   entry at the same INDEX in PlannerPolicy as well, or the node silently uses a
//   different policy than the menu says. The FFTW3 backend checks honoursPolicy
//   (see the static_assert in Parameters.cpp): the menu is meaningless for a
//   backend that ignores it.
enum class Planner : int { Auto = 0, Fast, Measured, Patient, COUNT };   // == FFTDSP::PlannerPolicy (static_assert in AnalysisPipeline.cpp)

// The scale a bin's magnitude is normalized to, which is what sets the absolute
// level of every reading downstream. CoherentGain = mean(window) == 1 (legacy:
// a full-scale sine reads N_win/2); FullScale = sum(window) == 2 (the same sine
// reads its own amplitude). DSPModules.h explains why this is a compatibility
// knob, not a tuning knob.
// ADDING ONE: two edits, in step. The mapping to FFTDSP::WindowNorm lives in
//   AnalysisPipeline::updateWindow() (source/AnalysisPipeline.cpp), and
//   WindowGenerator::generateWindow()'s `target` selection in DSPModules.h decides
//   what the value means. Its normalization branch is an if/else on FullScale, so
//   a new entry silently normalizes as CoherentGain.
enum class MagNorm : int { CoherentGain = 0, FullScale, COUNT };

// What 0 dB means when a dB mode is active. FramePeak = this frame's own loudest
// bin (level-independent, good for shape); Dbfs = absolute (a full-scale sine
// reads ~0 dB); Agc = a slow peak follower per channel.
// ADDING ONE: add a `case` to the reference switch in
//   AnalysisPipeline::runChannel() (source/AnalysisPipeline.cpp), whose
//   `case FramePeak: default:` arm means a forgotten entry silently uses the
//   frame peak. Note the reference only applies when Loudness != Off: with
//   Loudness = Off this menu is not even read.
enum class DbRef : int { FramePeak = 0, Dbfs, Agc, COUNT };

// How attack/release are expressed. Coefficient = the per-frame smoothing factor
// directly; Milliseconds = a time constant converted with the actual frame delta,
// which is the frame-rate independent form (see coefFromMs in DSPModules.h).
// ADDING ONE: the mode check is `p.ballMode == Milliseconds` in BOTH
//   Parameters::eval() (which decides which pair of parameters to read) and
//   AnalysisPipeline::runChannel() (which decides how to turn it into
//   coefficients), so a forgotten entry silently behaves as Coefficient - and
//   because eval()'s branch also gates the parameter READS, the milliseconds
//   fields would not even be refreshed. Neither file has a default arm to remind
//   you.
enum class BallisticsMode : int { Coefficient = 0, Milliseconds, COUNT };

// How the input CHOP's channels are turned into analysis channels. MonoMix =
// average every input channel into one; FirstChannel = analyse input 0 only;
// AllChannels = one FFT per input channel, capped at kMaxChannels.
// ADDING ONE: the behaviour lives in two places in source/FFT.cpp -
//   analysisChannelCount() (how many output channels) and ingest() (which input
//   feeds which analysis channel). Both test for MonoMix and AllChannels
//   explicitly and fall through to "input 0" otherwise, so a new entry silently
//   behaves as FirstChannel. AllChannels is also the only mode that produces more
//   than one transform per cook (the parallel fan-out). getChannelName() branches on
//   MonoMix as well, but only to pick the output channel's name - cosmetic.
enum class ChanMode : int { MonoMix = 0, FirstChannel, AllChannels, COUNT };

// Interpolation kernel used when an output bin reads between two of the FFT's own
// linear bins: Linear = 2 taps, Cubic = Catmull-Rom over 4 taps (smoother lobes,
// which lets a smaller FFT be used, at two extra gathers per bin).
// ADDING ONE: the kernel selector is PerceptualWarping::setInterpolation()
//   (source/DSPModules.h), whose body is `(mode == 1) ? 1 : 0` - anything that is
//   not exactly 1 becomes linear. Unlike the menus above there is no per-entry
//   case to add, so the work is in the gather kernel itself; and the mode is part
//   of WarpKey in AnalysisPipeline.h, which is what makes the change take effect.
enum class WarpInterp : int { Linear = 0, Cubic, COUNT };
// Which FFT library the transform runs in. A toggle rather than a menu because there are exactly two
// today (the user's rule: a menu once there are three). The two libraries export the *same* fftwf_*
// symbols, so they cannot both be linked into one binary; the plugin loads whichever is selected at
// run time through FFTDSP::FftApi (see FftBackend.h). Values match backendById() in that header.
//
// WHAT THE ENTRIES ARE: Fftw3 = the vendored FFTW3 build (libfftw3f-3.3.11-avx2.dll); OneMkl = Intel
// oneMKL's FFTW3 interface (mkl_rt.3.dll - oneMKL 2026.x; the loader also accepts mkl_rt.2.dll for
// 2025.x and an unversioned mkl_rt.dll, in that order - see the dlls[] list in FftBackend.h), which
// dispatches to Intel-optimised kernels on Intel CPUs.
// VALUES ARE REGISTRY INDICES, not just labels: backendById() in FftBackend.h is a switch over them.
// Even though this is used as a toggle and not a menu, the static_asserts in Parameters.cpp pin both
// the count and both values to that registry - unlike the Planner enum above, which is pinned by
// nothing. ADDING A BACKEND: extend this enum, raise backendCount() and add the case in
// backendById() (FftBackend.h), and raise kMaxBackends there; the assert in Parameters.cpp will tell
// you if you missed the count. See FftBackend.h's HOW TO CHANGE for the rest (the dlls[] list and
// the descriptor).
enum class Backend : int { Fftw3 = 0, OneMkl = 1, COUNT = 2 };

// ---- v2.10 real-time controls ------------------------------------------------------------------
// Quality presets: a preset overrides Zero-Pad Len, Warp Interpolation, Kaiser Beta Mode and Warp
// Aggregation together (applyPreset in RateModel.h); Custom leaves every individual parameter in charge.
// A preset NEVER changes the output sample count: Output Bins (and Output Bins Mode) stay the user's.
enum class Preset : int { Custom = 0, Visual60, Visual120, Analysis, COUNT };

// Output Bins Mode. Auto (default) = the rfft's own bin count, N/2+1 of the transform actually run (the
// Zero-Pad Len frame, or the window with Zero-Padding off); Output Bins has no effect. The bins are laid
// out on the chosen Scale / Display Max - for the untouched rfft bins use Raw RFFT Bins as well.
// Fixed = exactly `Output Bins` output samples, any count (more than N/2+1 included: interpolated).
enum class BinsMode : int { Fixed = 0, Auto, COUNT };

// What an output bin reports when it covers more than one FFT bin (the coarse, usually high-frequency,
// part of a perceptual axis). Off = interpolate between two FFT bins (legacy: narrow peaks between the
// taps are skipped). Peak = the largest FFT bin in the range (display: no peak is ever dropped).
// Rms = the power mean of the range (analysis: energy-faithful). PerceptualWarping::applyWarp.
enum class WarpAggregate : int { Off = 0, Peak, Rms, COUNT };

// Kaiser Beta Mode. Manual = `Kaiser Beta`. Auto = the smallest beta whose sidelobes sit below
// `dB Range Floor` (Kaiser's design relation), i.e. the sharpest main lobe the display can use.
enum class BetaMode : int { Manual = 0, Auto, COUNT };

// How each cook's input block becomes new samples. Auto = only samples that are new since the last
// cook, from the input's start index and cook count (so an overlapping or re-delivered buffer is not
// appended twice). AppendAll = the pre-2.10 behaviour: append the newest block every cook.
enum class IngestMode : int { Auto = 0, AppendAll, COUNT };

// How the async worker learns about a new job. Poll = it checks the job slot every 2 ms (no kernel
// call on the cook thread; pickup 0-2 ms). Signal = the cook wakes it every cook (one SetEvent,
// ~5-25 us on the cook thread; pickup ~0.02 ms). The worker also goes dormant after 500 ms idle.
enum class WorkerWake : int { Poll = 0, Signal, COUNT };

// Worker thread scheduling. Highest = THREAD_PRIORITY_HIGHEST. Mmcss = registered with the Multimedia
// Class Scheduler ("Pro Audio"), which Windows schedules ahead of normal threads and exempts from
// power throttling - the setting for 120+ fps projects on a busy machine.
enum class WorkerPriority : int { Highest = 0, Mmcss, COUNT };

// Zero-padded FFT transform lengths, indexed by the "Pad" menu (menu order in Parameters.cpp)
//
// WHAT: the seven zero-pad lengths offered by the Zero-Pad Len menu, in samples. The menu stores
//       the INDEX, and eval() turns that index into the sample count in Values::padSize.
// UNITS: samples of the transform (a power of two by construction, which fftSizeFrom relies on).
// WHY HERE AND NOT IN Parameters.cpp: only the strings live in Parameters.cpp. The numbers are here
//       so the header can be included by anything that needs them without pulling in a .cpp - and so
//       the name/label arrays can be length-checked against kPadCount by a static_assert.
// HOW TO CHANGE: kPadValues is declared with a literal 7, NOT with kPadCount, so the two are only
//       kept equal by the static_assert on kPadNames in Parameters.cpp. Change one and forget the
//       other and the build stops rather than the menu silently losing or gaining an entry.
// SIBLING COPY: bench/fftw_version_probe.cpp declares its own kPadValues/kPadCount because that
//       probe deliberately includes nothing from source/. It is a copy, not a shared header: extend
//       this list and that one must be extended by hand. Nothing checks it.
// CALLED BY: Parameters::eval() (source/Parameters.cpp), via the pad index, and the static_assert
//       on kPadNames. Not used by FFT.cpp, AnalysisPipeline or tests/dsp_tests.cpp.
inline constexpr int kPadValues[7] = { 1024, 2048, 4096, 8192, 16384, 32768, 65536 };
// Number of entries in kPadValues, and the bound the stored pad index is checked against in eval().
// It is BOTH the length of the Pad menu (the static_assert) and an internal sanity bound - the only
// constant here that plays both roles.
inline constexpr int kPadCount = 7;
// What an out-of-range pad index falls back to (eval(), source/Parameters.cpp), and the default of
// Values::padSize. Despite the name this is a FALLBACK, not the value the Pad menu opens on: the menu
// opens on index 4, which happens to be this same length. 16384 is the shipped default transform
// length (bench/fftw_version_probe.cpp notes it is the default it benchmarks against), chosen as a
// practical balance of frequency resolution against plan time and memory - it is not derived in this
// file, and changing it changes the default sharpness of every plot.
inline constexpr int kPadDefault = 16384;

// Hard limits (defensive: the sliders are soft limits, typed values are not)
//
// These are the only five quantities this project pins an absolute bound on. Two different kinds of
// bound live in this list, and they are enforced in different places:
//
//   LIMITS ON PARAMETER VALUES (every one of these is also a slider range in Parameters.cpp::setup,
//   and the constant is the WIDER of the two - the slider is what a user normally cannot exceed, and
//   this is what a typed or scripted value cannot exceed):
//     kMinBins / kMaxBins      - Output Bins. Enforced by std::clamp in eval().
//     kMaxWinSamples           - window length, in BOTH of its units: eval() clamps the sample-mode
//                                value, and windowSamplesFrom() (source/RateModel.h) clamps the
//                                value it computes from milliseconds.
//
//   INTERNAL BOUNDS (not a parameter at all - these guard the plugin against its own arithmetic):
//     kMaxChannels             - the most analysis channels the fan-out will build. Enforced in
//                                FFT.cpp (analysisChannelCount and execute), which clamps the input
//                                channel count. There is no parameter whose slider this mirrors.
//     kMinSampleRate/kMaxSampleRate - the accepted range of the INPUT rate. Enforced in FFT.cpp's
//                                execute(), which clamps the rate reported by the input CHOP. Note
//                                the upper end (384 kHz, i.e. 192 kHz Nyquist) is the number the
//                                Display Max slider range is chosen around - see the comment on the
//                                Spectrum page in Parameters.cpp.
//
// WHAT IS NOT HERE, and matters as much: only the quantities above have a hard clamp. Display Max,
// Log Floor, Kaiser Beta, dB Range Floor, Q, Amount, the shelf gains and cutoffs and the ms-mode
// attack/release are held only by their slider ranges (eval() applies a lower-bound sanity check to
// a few of them, but no upper bound). A typed or scripted value outside the slider range reaches the
// DSP for those. Do not read this block as "every parameter is bounded" - it is the list of the ones
// that are.
inline constexpr int    kMinBins        = 8;
inline constexpr int    kMaxBins        = 262144;
inline constexpr int    kMaxWinSamples  = 65536;
inline constexpr int    kMaxChannels    = 64;
inline constexpr double kMinSampleRate  = 1.0;
inline constexpr double kMaxSampleRate  = 384000.0;

// ---------------------------------------------------------------------------
// Names & labels
// ---------------------------------------------------------------------------
// Two strings per parameter, with two different jobs:
//   <X>Name   the IDENTIFIER TouchDesigner stores. It is what eval() looks up, what a saved .toe
//             refers to, and what FFT::pulsePressed() compares against for the Reset pulse. Names
//             are single lowercased words by TouchDesigner convention ("Displaymax", "Lowcutoffhz")
//             - they are not shown to the user, and changing one is a silent break, not a rename.
//   <X>Label  the text shown in the parameter dialog. Free-form and user-visible: change a label
//             freely, it is cosmetic. The one exception is that a label is also what shows in the
//             Info DAT/popup parameter listings.
// The DECLARATION ORDER of these pairs follows the field order of Values below, group for group.
// That is a reading aid only - eval() resolves each parameter by name, so reordering both files
// together is harmless and reordering one alone is harmless too (just harder to read). What is not
// harmless is reordering a menu's ENUM without its names array, or vice versa.
// EVERY string in this section is registered with TouchDesigner by setup() in Parameters.cpp and is
// what a saved project stores; nothing here is used by the DSP core.
//
// Page 1: Spectrum
constexpr char ChanmodeName[]     = "Chanmode";     constexpr char ChanmodeLabel[]     = "Channels";
constexpr char ScaleName[]        = "Scale";        constexpr char ScaleLabel[]        = "Scale";
constexpr char DisplaymaxName[]   = "Displaymax";   constexpr char DisplaymaxLabel[]   = "Display Max Hz";
constexpr char BinsName[]         = "Bins";         constexpr char BinsLabel[]         = "Output Bins";
constexpr char WarpName[]         = "Warp";         constexpr char WarpLabel[]         = "Warp Blend";
constexpr char WarpinterpName[]   = "Warpinterp";   constexpr char WarpinterpLabel[]   = "Warp Interpolation";
constexpr char LogfloorName[]     = "Logfloor";     constexpr char LogfloorLabel[]     = "Log Floor Hz";
constexpr char WinmodeName[]      = "Winmode";      constexpr char WinmodeLabel[]      = "Window Length Mode";
constexpr char WinsamplesName[]   = "Winsamples";   constexpr char WinsamplesLabel[]   = "Window Sampling";
constexpr char WinmsName[]        = "Winms";        constexpr char WinmsLabel[]        = "Window Length ms";
constexpr char PadName[]          = "Pad";          constexpr char PadLabel[]          = "Zero-Pad Len";
constexpr char PlannerName[]      = "Planner";      constexpr char PlannerLabel[]      = "FFT Planner";

// Page 2: EQ  (Eqenable off = EQ code AND its parameter reads are skipped entirely)
constexpr char EqenableName[]     = "Eqenable";     constexpr char EqenableLabel[]     = "EQ Enable";
constexpr char HighshelfName[]    = "Highshelf";    constexpr char HighshelfLabel[]    = "High Shelf";
constexpr char LowshelfName[]     = "Lowshelf";     constexpr char LowshelfLabel[]     = "Low Shelf";
constexpr char GaindbName[]       = "Gaindb";       constexpr char GaindbLabel[]       = "High Boost dB";
constexpr char CutoffhzName[]     = "Cutoffhz";     constexpr char CutoffhzLabel[]     = "High Cutoff Hz";
constexpr char LowgaindbName[]    = "Lowgaindb";    constexpr char LowgaindbLabel[]    = "Low Boost dB";
constexpr char LowcutoffhzName[]  = "Lowcutoffhz";  constexpr char LowcutoffhzLabel[]  = "Low Cutoff Hz";
constexpr char QName[]            = "Q";            constexpr char QLabel[]            = "EQ Q Factor";
constexpr char AmountName[]       = "Amount";       constexpr char AmountLabel[]       = "EQ Blend Amount";

// Page 3: Window & Weighting
constexpr char WindowName[]       = "Window";       constexpr char WindowLabel[]       = "Window Type";
constexpr char KaiserName[]       = "Kaiser";       constexpr char KaiserLabel[]       = "Kaiser Beta";
constexpr char WeightingName[]    = "Weighting";    constexpr char WeightingLabel[]    = "Loudness Weighting";
constexpr char MagnormName[]      = "Magnorm";      constexpr char MagnormLabel[]      = "Magnitude Normalization";

// Page 4: Loudness & Ballistics
constexpr char LoudnessName[]     = "Loudness";     constexpr char LoudnessLabel[]     = "Loudness Mode";
constexpr char DbrefName[]        = "Dbref";        constexpr char DbrefLabel[]        = "dB Reference";
constexpr char DbrangeName[]      = "Dbrange";      constexpr char DbrangeLabel[]      = "dB Range Floor";
constexpr char BallenableName[]   = "Ballenable";   constexpr char BallenableLabel[]   = "Ballistics Enable";
constexpr char BallmodeName[]     = "Ballmode";     constexpr char BallmodeLabel[]     = "Ballistics Mode";
constexpr char AttackName[]       = "Attack";       constexpr char AttackLabel[]       = "Attack Speed";
constexpr char ReleaseName[]      = "Release";      constexpr char ReleaseLabel[]      = "Release Speed";
constexpr char AttackmsName[]     = "Attackms";     constexpr char AttackmsLabel[]     = "Attack ms";
constexpr char ReleasemsName[]    = "Releasems";    constexpr char ReleasemsLabel[]    = "Release ms";
constexpr char ResetName[]        = "Reset";        constexpr char ResetLabel[]        = "Reset";

// Page 5: Performance
constexpr char AsyncName[]        = "Async";        constexpr char AsyncLabel[]        = "Async Analysis (worker thread)";
constexpr char FftbackendName[]   = "Fftbackend";   constexpr char FftbackendLabel[]   = "FFT Backend (off: FFTW3 / on: Intel oneMKL)";

// v2.10 additions (names are new, so existing .toe files simply get the new defaults)
constexpr char PresetName[]       = "Preset";       constexpr char PresetLabel[]       = "Quality Preset";
constexpr char BinsmodeName[]     = "Binsmode";     constexpr char BinsmodeLabel[]     = "Output Bins Mode";
constexpr char WarpaggName[]      = "Warpagg";      constexpr char WarpaggLabel[]      = "Warp Aggregation";
constexpr char IngestName[]       = "Ingest";       constexpr char IngestLabel[]       = "Input Ingest";
constexpr char KaisermodeName[]   = "Kaisermode";   constexpr char KaisermodeLabel[]   = "Kaiser Beta Mode";
constexpr char FeaturesName[]     = "Features";     constexpr char FeaturesLabel[]     = "Spectral Features (Info CHOP)";
constexpr char WorkerwakeName[]   = "Workerwake";   constexpr char WorkerwakeLabel[]   = "Worker Wake";
constexpr char WorkerprioName[]   = "Workerprio";   constexpr char WorkerprioLabel[]   = "Worker Priority";
// v2.11
constexpr char RawbinsName[]      = "Rawbins";      constexpr char RawbinsLabel[]      = "Raw RFFT Bins (no interpolation)";
constexpr char ZeropadName[]      = "Zeropad";      constexpr char ZeropadLabel[]      = "Zero-Padding";

// ---------------------------------------------------------------------------
// Snapshot of every parameter for one cook
// ---------------------------------------------------------------------------
// WHAT: one plain struct holding the value of every parameter as it was at the start of a cook. The
// DSP core (AnalysisPipeline, DSPModules) sees only this - it never touches TouchDesigner's
// parameter system, which is what makes it testable headlessly.
// PROVIDED BY: Parameters::eval(), once per cook, from FFT::pollParameters().
//
// THE DEFAULTS HERE ARE LOAD-BEARING, in three separate ways - read this before changing one:
//   1. THEY ARE A SECOND COPY OF THE REGISTERED DEFAULTS. The same numbers are written again as
//      arguments in Parameters.cpp::setup(). Nothing checks that the two agree; if they drift, the
//      node behaves one way before the first eval() and another way after it, which looks like a
//      random one-frame difference rather than a bug.
//   2. THEY ARE WHAT A COOK WITH NO INPUTS USES. eval() returns a default-constructed Values
//      unchanged when `inputs` is null (Parameters.cpp), so on such a cook every number below is
//      what the node reports and executes with.
//   3. THE TESTS TREAT THEM AS "THE DEFAULTS". tests/dsp_tests.cpp constructs a bare
//      Parameters::Values and asserts against it (test_rate_model(), which says so in its own HOW TO
//      CHANGE note). Moving a default here moves those expectations with it.
// Three of the numbers below are also written down in other files and must move together with them:
// winSamples (3175, the FIFO's default capacity in DSPModules.h/AnalysisPipeline.h/FFT.h),
// winMs (72 ms, which IS 3175 samples at 44.1 kHz) and displayMax (24000 Hz, which is the 48 kHz
// Nyquist the tests reason about). The per-group notes below name each one.
//
// NOT A VALIDATION POINT: these are defaults, not clamps. eval() is where a stored value is
// sanitized (see the hard-limit block above).
struct Values {
    // Spectrum
    // What the analysis produces and how its axis is laid out. Units are noted per field, because
    // this group mixes counts, Hz and a unitless 0..1 blend.
    ChanMode   chanMode     = ChanMode::MonoMix;   // mono is the 99 % use case: one analysis channel; the mix is an average (sum / channel count), not a sum, so it does not get louder with more inputs
    Scale      scale        = Scale::Log;          // how the frequency axis is spaced (a menu index - see the Scale enum above)
    double     displayMax   = 24000.0;             // Hz, the top of the axis; 24000 is 48 kHz / 2, the Nyquist the tests reason about. Not derived in this file - it is simply the common 48 kHz case
    int        bins         = 16384;      // output bins; the FFT's own grid is pad/2 + 1 = 8193 at the default 16K pad. Values ABOVE that grid are legal: the warp interpolates, it does not require one FFT bin per output bin
    double     warp         = 0.963;      // 0 = linear axis whatever Scale says, 1 = the Scale's own axis. 0.963 is a taste default (mostly the perceptual axis, a little linear) - the value is not derived in this file. tests/dsp_tests.cpp and bench/bench.cpp both copy this literal
    WarpInterp warpInterp   = WarpInterp::Linear;  // interpolation kernel when an output bin falls between FFT bins (see WarpInterp above)
    double     logFloor     = 20.0;                // Hz, the bottom of a Log or Mel+Log axis. Ignored by Chroma, which uses a fixed 20 Hz floor of its own (see computeTargetHzGrid)
    WinMode    winMode      = WinMode::Samples;    // which of the two fields below is live; the other keeps its default (see WinMode above)
    int        winSamples   = 3175;                // samples. LOAD-BEARING: 3175 is also the default FIFO capacity written in DSPModules.h, AnalysisPipeline.h and FFT.h, and Parameters.cpp registers it again as the slider default and the fallback - four other copies
    double     winMs        = 72.0;                // ms. LOAD-BEARING with winSamples: 72 ms IS 3175 samples at 44.1 kHz, so the two defaults describe the same window and moving one without the other makes the mode switch change the analysis
    int        padIndex     = 4;          // index into kPadValues (16384); the menu stores an index, not a length
    int        padSize      = kPadDefault;         // samples, resolved from padIndex by eval(). The invariant is padSize == kPadValues[padIndex] - nothing recomputes it from the index at use sites
    Planner    planner      = Planner::Auto;       // must stay INDEX-ALIGNED with FFTDSP::PlannerPolicy (source/DSPModules.h): AnalysisPipeline casts the two, and nothing asserts it
    // EQ
    // An optional biquad shelf pair applied to the audio BEFORE the window (see the EQ section of
    // Parameters.cpp for why it is off by default). Gains are dB, cutoffs are Hz, and the whole
    // group is dead weight when eqEnable is false: eval() does not read any of the fields below it.
    bool       eqEnable     = false;      // off: no EQ code, no EQ parameter reads
    bool       highShelf    = true;       // which shelf sections are built; a disabled shelf forces its gain to 0 and its cutoff back to the default in eval(), so a stale value cannot leak in when it is re-enabled
    bool       lowShelf     = true;
    double     gainDb       = 6.0;        // dB of high-shelf boost. 6.0 is a prototype leftover (see the EQ page comment in Parameters.cpp), not a derived value
    double     cutoffHz     = 1000.0;     // Hz, high-shelf corner
    double     lowGainDb    = 0.0;        // dB of low-shelf boost (flat by default)
    double     lowCutoffHz  = 200.0;      // Hz, low-shelf corner
    double     q            = 0.707;      // shelf resonance; 0.707 is 1/sqrt(2), the Butterworth (maximally flat) value - bench/bench.cpp names it the plugin default
    double     amount       = 1.0;        // dry/wet blend: the EQ change is scaled by this, so 0 = the filter is bypassed (updateAndCheckActive() returns false), 1 = fully filtered, and above 1 the same linear blend keeps extrapolating. NOTE the registered slider goes to 5.0 and eval() does not clamp it, so values above 1 are reachable: this reads like a 0..1 control but is not bounded at 1
    // Window & weighting
    // The taper (window + its beta) and the equal-loudness curve. These feed the two things that
    // set absolute level: window normalization and the weighting curve.
    WindowType window       = WindowType::Kaiser;   // the only type with a continuous leakage control; see the WindowType enum above for what a missing case does
    double     kaiserBeta   = 15.0;                 // only read when window == Kaiser. 15 is the documented low-leakage default (DSPModules.h), and the slider range is 1..55 while eval() clamps a typed value to 0..100 - the wider hard bound is deliberate
    Weighting  weighting    = Weighting::Off;       // flat by default: a weighting curve is a measurement decision, not a display default
    MagNorm    magNorm      = MagNorm::CoherentGain;  // LOAD-BEARING for compatibility, not a preference: Parameters.cpp's header records that the defaults here reproduce the previous version's behaviour exactly, and the coherent-gain reading is what existing projects tuned their dB offsets against (DSPModules.h explains the trade)
    // Loudness & ballistics
    // The display unit and the smoothing. loudness == Off means the whole dB group below is not read
    // (so dbRef and dbRange keep their defaults), and ballEnable == false means the smoother is off.
    Loudness   loudness     = Loudness::Off;        // units of the output; Off = linear magnitude
    DbRef      dbRef        = DbRef::FramePeak;     // what 0 dB means; only read when loudness != Off. FramePeak is the previous version's behaviour (see the Parameters.cpp header)
    double     dbRange      = 80.0;                 // dB, how far below the reference the floor sits. 80 is the conventional display floor; not derived in this file. eval() only guards against a non-positive value
    bool       ballEnable   = false;      // off: no ballistics code, no attack/release parameter reads
    BallisticsMode ballMode = BallisticsMode::Coefficient;   // the previous version's behaviour (see the Parameters.cpp header); Coefficient is also what a forgotten menu case falls back to
    double     attack       = 0.0;        // per-frame coefficient, 0..0.99. 0 means "follow the new value instantly", i.e. no attack smoothing - the default is no smoothing, not fast smoothing
    double     release      = 0.0;        // same, for the falling direction
    double     attackMs     = 50.0;       // ms, used only in Milliseconds mode; converted per cook by coefFromMs (DSPModules.h) so the time is frame-rate independent. 50/200 ms are a taste default - not derived in this file
    double     releaseMs    = 200.0;      // ms, same; the release is the one that matters for how the display decays
    // Performance
    // Not sound, but scheduling: which thread analyses and which library transforms.
    bool       async        = true;       // analysis on a worker thread; the cook only ingests + copies
    Backend    backend      = Backend::Fftw3;  // FFTW3 by default: it is the library this build vendors. Both libraries are loaded at run time and a missing one falls back to FFTW3, so this cannot silence the node
    // v2.10 controls (see the enums above). Output Bins Mode defaults to Auto: the output is the zero-
    // padded rfft's N/2+1 bins (Zero-Pad Len 16384 -> 8193 samples); Fixed makes Output Bins the count.
    // The window is the pre-2.10 Kaiser beta 15; Auto beta is opt-in. Peak aggregation only acts where an
    // output bin covers several FFT bins, so it never changes the count.
    Preset         preset        = Preset::Custom;
    BinsMode       binsMode      = BinsMode::Auto;            // Auto: N/2+1 of the (zero-padded) FFT
    WarpAggregate  warpAggregate = WarpAggregate::Peak;
    BetaMode       betaMode      = BetaMode::Manual;          // Kaiser Beta (15) unless Auto is chosen
    IngestMode     ingestMode    = IngestMode::Auto;
    bool           features      = false;                     // spectral features on the Info CHOP (worker cost ~2-4 us)
    // v2.11. rawBins: output the rfft magnitude bins untouched - N/2+1 samples, DC..Nyquist, linear, no
    // Scale / Display Max / Output Bins / interpolation (the warp is the identity and runs as a memcpy).
    // zeroPad off: the transform runs on the window itself (N = window length, rounded up to even so the
    // last bin is exactly Nyquist) instead of on the Zero-Pad Len frame.
    bool           rawBins       = false;
    bool           zeroPad       = true;
    WorkerWake     workerWake    = WorkerWake::Poll;
    WorkerPriority workerPriority = WorkerPriority::Highest;
};

// ---------------------------------------------------------------------------
// The two phases: setup() once at load, eval() once per cook
// ---------------------------------------------------------------------------
// TouchDesigner's custom-operator API drives both:
//
//   setup()  runs ONCE, when TouchDesigner asks the plugin to declare its parameters (the
//            OP_ParameterManager callbacks). It is the only place anything is registered, so a
//            parameter that is not registered here does not exist as far as the node is concerned -
//            and eval() asking for it would return a default rather than fail loudly.
//            CALLED BY: FFT::setupParameters() (source/FFT.cpp), which TouchDesigner calls once when
//            the node is created or the DLL is (re)loaded.
//
//   eval()   runs ONCE PER COOK and returns the value of every parameter in the Values snapshot.
//            It is called from FFT::pollParameters(), which FFT::getOutputInfo() calls on every
//            cook - getOutputInfo() always precedes execute(), so execute() normally reuses the
//            snapshot instead of re-reading. Nothing else in the cook path touches a parameter, and
//            the Info DAT/popup callbacks read the snapshot or the cached telemetry, never the UI.
//            CALLED BY: FFT::pollParameters() in source/FFT.cpp.
//
// WHY THE PHASES ARE SEPARATE AT ALL: setup() is UI registration and touches no values; eval() reads
// values and registers nothing. Keeping them apart is what lets eval() be cheap enough to run every
// cook (no allocation, no strings) and is why the DSP core can be exercised headlessly with a
// hand-built Values (tests/dsp_tests.cpp does exactly that, and never links Parameters.cpp).

// Fetch every parameter in one pass. Optional sections are only read when enabled, so a
// disabled section costs zero TouchDesigner parameter fetches (they are not free: each
// getPar* is a virtual call into TouchDesigner's parameter system, ~1-5 us).
// The operator calls this from getOutputInfo() (which precedes every execute()) on EVERY
// cook; nothing else in the cook path reads a parameter.
// 'reads' receives the number of getPar* calls performed (telemetry).
// A null `inputs` returns a default-constructed Values untouched rather than crashing - which is
// why the defaults documented above are load-bearing. It performs NO validation of its own beyond
// the clamps listed against the hard limits above: an out-of-range stored value becomes the
// documented fallback, never a rejected cook.
Values eval(const TD::OP_Inputs* inputs, int* reads = nullptr);

// Registers custom parameters and UI controls with TouchDesigner's parameter manager
// Order matters only for the page tabs and the parameter order inside a page; the name strings are
// what everything else resolves against. Add a parameter here AND as a Values field AND to eval()'s
// reads - those three are three separate edits and nothing checks that all three were made.
void setup(TD::OP_ParameterManager* manager);

// Greys out the parameters that the current settings make inert (EQ sub-parameters with EQ off, the dB
// group with Loudness = Off, the ballistics pair in the unused unit, Kaiser Beta for other windows or in
// Auto, everything a Quality Preset overrides). Called by TouchDesigner through
// FFT::setParameterEnableStates (C++ API Common 3), which may happen outside a cook - it reads
// parameters only through `inputs` and touches no node state, so it costs the cook nothing.
void setEnableStates(const TD::OP_Inputs* inputs, TD::OP_ParEnableState* state);

} // namespace Parameters

#endif // PARAMETERS_H
