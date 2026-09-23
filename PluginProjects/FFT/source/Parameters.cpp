/* Shared Use License: This file is owned by Derivative Inc. (Derivative)
* and can only be used, and/or modified for use, in conjunction with
* Derivative's TouchDesigner software, and only if you are a licensee who has
* accepted Derivative's TouchDesigner license or assignment agreement
* (which also govern the use of this file). You may share or redistribute
* a modified version of this file provided the following conditions are met:
*
* 1. The shared file or redistribution must retain the information set out
* above and this list of conditions.
* 2. Derivative's name (Derivative Inc.) or its trademarks may not be used
* to endorse or promote products derived from this file without specific
* prior written permission from Derivative.
*/

/*
===========================================================================
                  PARAMETER SETUP IMPLEMENTATION
===========================================================================
Source File: Parameters.cpp

Registers the custom parameters on 5 pages:
1. Spectrum:               scale, display range, output bins, warp, log floor,
                           window length (samples or ms), zero-pad size, FFT planner policy.
2. EQ:                     high/low shelf gain + cutoff, Q, wet/dry.
3. Window & Weighting:     window type, Kaiser beta, equal-loudness weighting, magnitude normalization.
4. Loudness & Ballistics:  linear/dB/dB-normalized, dB reference, dB floor, attack/release
                           (per-frame coefficient or milliseconds), reset.
5. Performance:            the async worker thread, and which FFT library the node plans
                           and transforms with (FFTW3 or Intel oneMKL). This node is one mono
                           channel per instance, so there is nothing else to schedule.

Defaults reproduce the behaviour of the previous version exactly (Window Length Mode = Samples,
Magnitude Normalization = Coherent Gain, dB Reference = Frame Peak, Ballistics Mode = Coefficient).
===========================================================================
*/

/*
===========================================================================
WHAT THIS FILE IS (plain version)
===========================================================================
It is the parameter DEFINITION TABLES TouchDesigner reads, plus the code that reads
the user's values back out. Nothing here produces audio or a spectrum; nothing here
runs per-sample. Concretely this file holds:

  1. the helpers that register one control at a time (appendMenu/appendFloat/
     appendInt/appendToggle), in the anonymous namespace below;
  2. the menu string tables - the text of every menu, in menu order;
  3. setup(), which registers every parameter and page with TouchDesigner; and
  4. eval(), which fetches the current value of every parameter into a
     Parameters::Values snapshot, once per cook.

The parameter metadata (enums, hard limits, name/label constants, the Values struct)
lives in Parameters.h. See the top of that file for how the pieces fit together.

THE SINGLE MOST COMMON WAY TO BREAK THIS FILE - and it is silent:

    Inserting a menu entry in Parameters.h (or in one of the tables here) without
    inserting its twin at the SAME POSITION in the other file.

A menu's stored value is the ARRAY INDEX, so the Nth string here is what the enum
value N means. The static_asserts below compare only the array LENGTH against the
enum's COUNT, so a one-sided insert leaves the count correct and every entry after
the insertion point off by one: the user picks "Hann" and the DSP builds a Hamming
window, with no error anywhere. The same applies to reordering. There is no way to
make this safe by inspection - the only defence is that both edits are one edit, so
insert in Parameters.h and the matching table here in the same change.

Two smaller traps in the same family, both silent:
  - a parameter registered here but never read in eval() looks live and does nothing;
  - a Values field filled by eval() but never registered here appears in no dialog.
    The three edits for a new parameter - enum or constant, table entry + append*
    call, and the eval() read - are separate and nothing checks that all three were
    made.

NOTHING IN THIS FILE CHANGES WHAT A NUMBER MEANS. It decides what is offered, what
the default is, and how wide the slider is; the meaning of each value lives in the
DSP (see the section comments in DSPModules.h). So a change here can make a control
harder or easier to reach, and can move a default, but it cannot make the analysis
more accurate.
===========================================================================
*/

#include "Parameters.h"

#include "FftBackend.h"   // FFTDSP::backendCount()/backendById() - the menu-value contract below
#include "RateModel.h"    // applyPreset (pure, shared with the tests and the bench)

#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------------------------
// Menu value <-> backend registry: the compile-time half of the contract.
// ---------------------------------------------------------------------------------------------
// Parameters::Backend is a plain int-typed enum because TouchDesigner menus are integers, while the
// registry in FftBackend.h is a switch over static descriptors. Nothing but these asserts stops the
// two from drifting apart - and a drift here is silent and expensive: the toggle would select a
// backend the engine never looks up, and the node would keep transforming with FFTW3 while claiming
// oneMKL. Asserting the count and both endpoints pins the whole mapping, since the enum is dense
// (COUNT == 2 and Fftw3 == 0, OneMkl == 1 forces the one legal correspondence).
static_assert(static_cast<int>(Parameters::Backend::COUNT) == FFTDSP::backendCount(),
              "Parameters::Backend and the FftBackend.h registry disagree on how many backends exist: "
              "extend both, and give the new backend a menu value in appendToggle()");
static_assert(static_cast<int>(Parameters::Backend::Fftw3) == 0 && static_cast<int>(Parameters::Backend::OneMkl) == 1,
              "backend menu values are the registry indices: Backend::Fftw3 must index kFftw3Backend (0) "
              "and Backend::OneMkl must index kMklBackend (1) - see backendById() in FftBackend.h");
static_assert(FFTDSP::backendById(static_cast<int>(Parameters::Backend::Fftw3)).honoursPolicy,
              "the FFTW3 backend honours FFTW's plan rigour flags; if this ever becomes false the "
              "Planner Policy menu is being offered for a library that ignores it");
static_assert(FFTDSP::backendById(static_cast<int>(Parameters::Backend::OneMkl)).expectedVersion == nullptr,
              "the oneMKL backend deliberately pins no version: MKL ships under its own version scheme, "
              "so a pinned string here would report a false mismatch on every load");

namespace Parameters {

namespace {

// ---------------------------------------------------------------------------------------------
// Registration helpers - one control per call.
//
// WHAT THEY ARE FOR: each one fills in the one or two fields TouchDesigner actually requires
// (page, name, label, default, and a slider range where it applies) and forwards to the manager.
// They exist so that setup() below reads as a table of parameters rather than as thirty repetitions
// of the same five assignments.
//
// WHY 'page' IS A PARAMETER: every call names its page as a string literal ("Spectrum", "EQ", ...).
// Those strings are the dialog's tab names. A typo does not fail - TouchDesigner creates a second
// page with the misspelt name, so the symptom is a stray tab with one control on it.
//
// WHAT THE SLIDER RANGE IS AND IS NOT: the min/max handed to appendFloat/appendInt become the
// control's slider limits, which is what a user can drag to. They are NOT validation - a value typed
// into the field or set by a script can go outside them. The hard bounds are the k* constants in
// Parameters.h, applied by std::clamp in eval(). The two are set independently, and several
// parameters deliberately have a wider hard bound than slider bound (see the notes on Output Bins
// and Window Sampling in setup()).
// ---------------------------------------------------------------------------------------------

// Registers one menu: N strings, N labels, and the index the menu opens on. N is deduced from the
// names array, so a menu cannot be registered with a length that disagrees with its own table -
// which is the only reason the length is not passed as an argument.
template <size_t N>
void appendMenu(TD::OP_ParameterManager* manager, const char* page, const char* name, const char* label,
                const char* (&names)[N], const char* (&labels)[N], int defaultIndex)
{
	TD::OP_StringParameter sp;
	sp.page = page;
	sp.name = name;
	sp.label = label;
	sp.defaultValue = names[defaultIndex];
	manager->appendMenu(sp, static_cast<int32_t>(N), names, labels);
}

// One float (double) control. def is what the parameter starts at in a fresh node; sliderMin/sliderMax
// are drag limits only (see the note above) - eval() is what enforces the hard bounds.
void appendFloat(TD::OP_ParameterManager* manager, const char* page, const char* name, const char* label,
                 double def, double sliderMin, double sliderMax)
{
	TD::OP_NumericParameter np;
	np.page = page;
	np.name = name;
	np.label = label;
	np.defaultValues[0] = def;
	np.minSliders[0] = sliderMin;
	np.maxSliders[0] = sliderMax;
	manager->appendFloat(np);
}

// One integer control, same contract as appendFloat. Integers are used where a fractional value is
// meaningless (Output Bins, Window Sampling) rather than as an optimization.
void appendInt(TD::OP_ParameterManager* manager, const char* page, const char* name, const char* label,
               int def, int sliderMin, int sliderMax)
{
	TD::OP_NumericParameter np;
	np.page = page;
	np.name = name;
	np.label = label;
	np.defaultValues[0] = def;
	np.minSliders[0] = sliderMin;
	np.maxSliders[0] = sliderMax;
	manager->appendInt(np);
}

// One on/off toggle. TouchDesigner stores a toggle as a number, so the bool is converted to 1.0/0.0
// here; eval() reads it back with getParInt() and compares against 0, never as a bool.
void appendToggle(TD::OP_ParameterManager* manager, const char* page, const char* name, const char* label, bool def)
{
	TD::OP_NumericParameter np;
	np.page = page;
	np.name = name;
	np.label = label;
	np.defaultValues[0] = def ? 1.0 : 0.0;
	manager->appendToggle(np);
}

// ---------------------------------------------------------------------------------------------
// THE MENU TABLES - the text of every menu, in menu order.
//
// WHAT THE TWO ARRAYS ARE: Names are the values TouchDesigner stores and hands back to eval(); they
// are what actually round-trips. Labels are what the user reads in the dialog. Both are indexed by
// the stored parameter value, so position IS meaning.
//
// THE INVARIANT: for every table below, table[i] is the entry for enum value i, and the table length
// equals the enum's COUNT. The static_asserts after the tables check the LENGTH half only - they are
// a typo guard, not a correctness proof. The POSITION half is checked by nothing at all.
//
// WHO ELSE MUST AGREE WITH THESE TABLES (verified by reading each consumer, not assumed):
//   kScaleNames      -> Scale in Parameters.h, and the frequency grid itself in
//                       PerceptualWarping::computeTargetHzGrid() (source/DSPModules.h), a switch over
//                       the code whose `case 5: default:` arm is LINEAR.
//   kWindowNames     -> WindowType in Parameters.h, and WindowGenerator::generateWindow()
//                       (source/DSPModules.h), whose `case 0: default:` arm is KAISER.
//   kWeightingNames  -> Weighting in Parameters.h, and EqualLoudness::computeCurve()
//                       (source/DSPModules.h), whose per-bin weight starts at 1.0 = FLAT.
//   kLoudnessNames   -> Loudness in Parameters.h, and DecibelConverter::convertToDB()
//                       (source/DSPModules.h): mode 0 passes through, mode 2 rescales to 0..1, and
//                       EVERYTHING ELSE is plain dB.
//   kWinmodeNames    -> WinMode in Parameters.h, and windowSamplesFrom() (source/RateModel.h), whose
//                       if/else converts only Milliseconds and passes Samples through.
//   kPlannerNames    -> Planner in Parameters.h, which must ALSO stay index-aligned with
//                       FFTDSP::PlannerPolicy (source/DSPModules.h). AnalysisPipeline casts one to
//                       the other and nothing asserts it - the only such pairing here without an
//                       assert. Contrast the Backend toggle, which the static_asserts above pin.
//   kMagnormNames    -> MagNorm in Parameters.h, and the WindowNorm mapping in
//                       AnalysisPipeline::updateWindow() (source/AnalysisPipeline.cpp).
//   kDbrefNames      -> DbRef in Parameters.h, and the reference switch in
//                       AnalysisPipeline::runChannel() (source/AnalysisPipeline.cpp), whose
//                       `case FramePeak: default:` arm is the frame peak.
//   kBallmodeNames   -> BallisticsMode in Parameters.h, and BOTH branches on it: eval() below (which
//                       decides which pair of parameters is read) and runChannel() (which converts
//                       it to a coefficient).
//   kChanmodeNames   -> ChanMode in Parameters.h, and source/FFT.cpp's analysisChannelCount() and
//                       ingest(), which test for MonoMix and AllChannels explicitly.
//   kWarpinterpNames -> WarpInterp in Parameters.h, and PerceptualWarping::setInterpolation()
//                       (source/DSPModules.h), which is `(mode == 1) ? 1 : 0`.
//   kPadNames        -> NO enum. It is tied to kPadCount in Parameters.h by a static_assert, and the
//                       pad LENGTHS live there in kPadValues - this table holds only the displayed
//                       text. It is the one table with no enum, which is why its assert compares
//                       against kPadCount rather than a COUNT sentinel.
//
// The tables are file-scope but in the anonymous namespace, so they are not visible to any other
// translation unit: the only consumer of every table below is setup(), a few lines down.
//
// Menu tables (order == enum order in Parameters.h)
const char* kScaleNames[]      = { "Log", "Mel", "ERB", "Bark", "Chroma", "Linear", "Melog" };
const char* kScaleLabels[]     = { "Logarithmic", "Mel Scale", "ERB Scale", "Bark Scale", "Chroma / Pitch", "Linear Scale", "Mel + Log Blend" };
const char* kWindowNames[]     = { "Kaiser", "Hann", "Hamming", "Blackman", "Blackmanharris", "Rectangular" };
const char* kWindowLabels[]    = { "Kaiser (Beta control)", "Hann Window", "Hamming Window", "Blackman Window", "Blackman-Harris (92dB)", "Rectangular (Flat)" };
const char* kWeightingNames[]  = { "Off", "Aweighting", "Cweighting", "Itu468" };
const char* kWeightingLabels[] = { "Off (Flat)", "A-Weighting (IEC 61672)", "C-Weighting (High SPL)", "ITU-R 468 (Noise standard)" };
const char* kLoudnessNames[]   = { "Off", "Db", "Dbnorm" };
const char* kLoudnessLabels[]  = { "Off (Linear Magnitude)", "dB (Decibels)", "dB Normalized (0.0 to 1.0)" };
const char* kWinmodeNames[]    = { "Samples", "Milliseconds" };
const char* kWinmodeLabels[]   = { "Samples (Window Sampling)", "Milliseconds (Window Length ms)" };
const char* kPlannerNames[]    = { "Auto", "Fast", "Measured", "Patient" };
const char* kPlannerLabels[]   = { "Auto (instant plan, measured plan upgraded in background)", "Fast (Estimate only, never stalls)", "Measured (blocking measure once per size, wisdom cached)",
                                   "Patient (instant plan, FFTW_PATIENT measured in background, capped at 1.5 s once per size, wisdom cached)" };
const char* kMagnormNames[]    = { "Coherentgain", "Fullscale" };
const char* kMagnormLabels[]   = { "Coherent Gain (mean(window) = 1)", "Full Scale (sine amplitude 1 -> 1.0)" };
const char* kDbrefNames[]      = { "Framepeak", "Dbfs", "Agc" };
const char* kDbrefLabels[]     = { "Frame Peak (0 dB = loudest bin)", "0 dBFS (absolute)", "Slow AGC (peak follower)" };
const char* kBallmodeNames[]   = { "Coefficient", "Milliseconds" };
const char* kBallmodeLabels[]  = { "Per-frame Coefficient", "Milliseconds (frame-rate independent)" };
const char* kChanmodeNames[]   = { "Monomix", "Firstchannel", "Allchannels" };
const char* kChanmodeLabels[]  = { "Mono Mix (average all inputs -> 1 channel)", "First Channel Only", "All Channels (one FFT per channel)" };
const char* kWarpinterpNames[] = { "Linear", "Cubic" };
const char* kWarpinterpLabels[]= { "Linear (2 taps)", "Cubic Catmull-Rom (4 taps, smoother with smaller FFT)" };
const char* kPresetNames[]     = { "Custom", "Visual60", "Visual120", "Analysis" };
const char* kPresetLabels[]    = { "Custom (use the parameters below)", "Visual 60 fps (N 8192, cubic, auto beta)",
                                   "Visual 120 fps (N 4096, cubic, auto beta)", "Analysis (N 32768, auto beta, RMS aggregation)" };
const char* kBinsmodeNames[]   = { "Fixed", "Auto" };
const char* kBinsmodeLabels[]  = { "Fixed (Output Bins = output samples)", "Auto (rfft bin count N/2+1 of the zero-padded FFT)" };
const char* kWarpaggNames[]    = { "Off", "Peak", "Rms" };
const char* kWarpaggLabels[]   = { "Off (interpolate, may skip peaks)", "Peak (never drops a peak)", "RMS (power mean)" };
const char* kIngestNames[]     = { "Auto", "Appendall" };
const char* kIngestLabels[]    = { "Auto (only new samples, from start index)", "Append All (every block, legacy)" };
const char* kKaisermodeNames[] = { "Manual", "Auto" };
const char* kKaisermodeLabels[]= { "Manual (Kaiser Beta)", "Auto (from dB Range Floor)" };
const char* kWorkerwakeNames[] = { "Poll", "Signal" };
const char* kWorkerwakeLabels[]= { "Poll (2 ms timer, no cook-thread kernel call)", "Signal (wake every cook, lowest latency)" };
const char* kWorkerprioNames[] = { "Highest", "Mmcss" };
const char* kWorkerprioLabels[]= { "Highest (thread priority)", "MMCSS Pro Audio (OS multimedia scheduler)" };
const char* kPadNames[]        = { "1024", "2048", "4096", "8192", "16384", "32768", "65536" };
const char* kPadLabels[]       = { "1K Bins", "2K Bins", "4K Bins", "8K Bins", "16K Bins", "32K Bins", "64K Bins" };

static_assert(sizeof(kScaleNames) / sizeof(kScaleNames[0]) == static_cast<size_t>(Scale::COUNT), "Scale menu/enum mismatch");
static_assert(sizeof(kWindowNames) / sizeof(kWindowNames[0]) == static_cast<size_t>(WindowType::COUNT), "Window menu/enum mismatch");
static_assert(sizeof(kWeightingNames) / sizeof(kWeightingNames[0]) == static_cast<size_t>(Weighting::COUNT), "Weighting menu/enum mismatch");
static_assert(sizeof(kLoudnessNames) / sizeof(kLoudnessNames[0]) == static_cast<size_t>(Loudness::COUNT), "Loudness menu/enum mismatch");
static_assert(sizeof(kWinmodeNames) / sizeof(kWinmodeNames[0]) == static_cast<size_t>(WinMode::COUNT), "WinMode menu/enum mismatch");
static_assert(sizeof(kPlannerNames) / sizeof(kPlannerNames[0]) == static_cast<size_t>(Planner::COUNT), "Planner menu/enum mismatch");
static_assert(sizeof(kMagnormNames) / sizeof(kMagnormNames[0]) == static_cast<size_t>(MagNorm::COUNT), "MagNorm menu/enum mismatch");
static_assert(sizeof(kDbrefNames) / sizeof(kDbrefNames[0]) == static_cast<size_t>(DbRef::COUNT), "DbRef menu/enum mismatch");
static_assert(sizeof(kBallmodeNames) / sizeof(kBallmodeNames[0]) == static_cast<size_t>(BallisticsMode::COUNT), "BallisticsMode menu/enum mismatch");
static_assert(sizeof(kPadNames) / sizeof(kPadNames[0]) == static_cast<size_t>(kPadCount), "Pad menu/table mismatch");
static_assert(sizeof(kChanmodeNames) / sizeof(kChanmodeNames[0]) == static_cast<size_t>(ChanMode::COUNT), "ChanMode menu/enum mismatch");
static_assert(sizeof(kWarpinterpNames) / sizeof(kWarpinterpNames[0]) == static_cast<size_t>(WarpInterp::COUNT), "WarpInterp menu/enum mismatch");
static_assert(sizeof(kPresetNames) / sizeof(kPresetNames[0]) == static_cast<size_t>(Preset::COUNT), "Preset menu/enum mismatch");
static_assert(sizeof(kBinsmodeNames) / sizeof(kBinsmodeNames[0]) == static_cast<size_t>(BinsMode::COUNT), "BinsMode menu/enum mismatch");
static_assert(sizeof(kWarpaggNames) / sizeof(kWarpaggNames[0]) == static_cast<size_t>(WarpAggregate::COUNT), "WarpAggregate menu/enum mismatch");
static_assert(sizeof(kIngestNames) / sizeof(kIngestNames[0]) == static_cast<size_t>(IngestMode::COUNT), "IngestMode menu/enum mismatch");
static_assert(sizeof(kKaisermodeNames) / sizeof(kKaisermodeNames[0]) == static_cast<size_t>(BetaMode::COUNT), "BetaMode menu/enum mismatch");
static_assert(sizeof(kWorkerwakeNames) / sizeof(kWorkerwakeNames[0]) == static_cast<size_t>(WorkerWake::COUNT), "WorkerWake menu/enum mismatch");
static_assert(sizeof(kWorkerprioNames) / sizeof(kWorkerprioNames[0]) == static_cast<size_t>(WorkerPriority::COUNT), "WorkerPriority menu/enum mismatch");

} // namespace

/*
setup() - REGISTERS THE UI. Runs once, not per cook.

WHAT IT DOES: creates every page, control, default and slider range the node has, by calling the
append* helpers above and (once, at the end of page 4) TouchDesigner's appendPulse for Reset. It
reads nothing and computes nothing: the arguments are the parameter metadata.

WHO CALLS IT: FFT::setupParameters() (source/FFT.cpp), which TouchDesigner calls when the node is
created / when the plugin DLL is loaded. It is the ONLY registration point - a parameter not
registered here does not exist, and eval() asking for it would silently get a default.

WHAT IT DOES NOT DO: it does not validate. The default passed here is what a NEW node starts with;
the clamp that protects the DSP from an out-of-range value is in eval(), against the k* constants in
Parameters.h. Keeping those two in step is a manual job - the numbers appear twice.

ORDER: the call order sets the parameter order inside a page, and the first appearance of a page
string creates the page (so the page tabs come out in the order the pages are first mentioned).
Reordering the calls reorders the dialog; it changes no behaviour, because eval() resolves every
parameter by name.

THE DEFAULTS HERE MUST MATCH THE Values DEFAULTS IN Parameters.h. They are the same numbers written
twice, nothing checks that they agree, and a mismatch shows up as the node behaving differently
before and after the first cook. The header comment at the top of this file records which defaults
are there specifically to reproduce the previous version's behaviour.
*/
void setup(TD::OP_ParameterManager* manager)
{
	// --- Page 1: Spectrum ---
	// Note the append* calls are column-aligned by hand so the shared arguments line up down the
	// page. That alignment is cosmetic; reflowing it is safe, but a change to any argument is not -
	// each one is either a registered default (must match Parameters.h) or a slider bound (see the
	// per-page notes below).
	// The defaults on this page all restate the Parameters::Values defaults (see that struct); the
	// slider ranges are UI only. Two of them are deliberately NARROWER than the hard bounds eval()
	// enforces, so that a typed value can exceed what the slider reaches:
	//   Output Bins       slider 256..65536, hard 8..262144   (kMinBins/kMaxBins)
	//   Window Sampling   slider 1..32768,   hard 1..65536    (kMaxWinSamples)
	// and one, Window Length ms, is narrower on BOTH ends - the slider is 1..1000 ms while eval()
	// accepts 0.1..5000 ms.
	// The values themselves: 24000 Hz is the 48 kHz Nyquist case (not derived in this file), 16384
	// bins is both the Values default and kPadDefault's index-4 length, 3175 samples is the default
	// window and 72 ms is that same window expressed at 44.1 kHz, and 20 Hz is the conventional
	// lowest audible frequency - computeTargetHzGrid() in DSPModules.h uses 20 Hz for the same reason
	// when it floors the Chroma axis.
	// v2.10: the Quality Preset heads the page - when it is not Custom it overrides Zero-Pad Len, Warp
	// Interpolation, Kaiser Beta Mode and Warp Aggregation (greyed out accordingly); never the output count.
	appendMenu (manager, "Spectrum", PresetName,     PresetLabel,     kPresetNames,   kPresetLabels,   static_cast<int>(Preset::Custom));
	appendMenu (manager, "Spectrum", ChanmodeName,   ChanmodeLabel,   kChanmodeNames, kChanmodeLabels, static_cast<int>(ChanMode::MonoMix));
	appendToggle(manager, "Spectrum", RawbinsName,   RawbinsLabel,    false);
	appendMenu (manager, "Spectrum", ScaleName,      ScaleLabel,      kScaleNames,   kScaleLabels,   static_cast<int>(Scale::Log));
	// Display Max and Output Bins reach past the widest input the node accepts (384 kHz -> 192 kHz
	// Nyquist) and past the largest pad (64K -> 32769 bins) so that the linear/no-resample output is
	// reachable from the UI at every setting, without a dedicated toggle for it.
	appendFloat(manager, "Spectrum", DisplaymaxName, DisplaymaxLabel, 24000.0, 100.0, 192000.0);
	appendMenu (manager, "Spectrum", BinsmodeName,   BinsmodeLabel,   kBinsmodeNames, kBinsmodeLabels, static_cast<int>(BinsMode::Auto));
	appendInt  (manager, "Spectrum", BinsName,       BinsLabel,       16384, 256, 65536);   // the output sample count in Fixed
	appendFloat(manager, "Spectrum", WarpName,       WarpLabel,       0.963, 0.0, 1.0);
	appendMenu (manager, "Spectrum", WarpinterpName, WarpinterpLabel, kWarpinterpNames, kWarpinterpLabels, static_cast<int>(WarpInterp::Linear));
	appendMenu (manager, "Spectrum", WarpaggName,    WarpaggLabel,    kWarpaggNames,  kWarpaggLabels,  static_cast<int>(WarpAggregate::Peak));
	appendFloat(manager, "Spectrum", LogfloorName,   LogfloorLabel,   20.0, 1.0, 500.0);
	appendMenu (manager, "Spectrum", WinmodeName,    WinmodeLabel,    kWinmodeNames, kWinmodeLabels, static_cast<int>(WinMode::Samples));
	appendInt  (manager, "Spectrum", WinsamplesName, WinsamplesLabel, 3175, 1, 32768);
	appendFloat(manager, "Spectrum", WinmsName,      WinmsLabel,      72.0, 1.0, 1000.0);
	// The Pad menu opens on INDEX 4, not on a length: the menu stores an index into kPadValues
	// (Parameters.h), which is where the actual sample counts live. Index 4 is 16384 == kPadDefault,
	// and the two must stay equal - eval() falls back to kPadDefault for an out-of-range index, so a
	// mismatch would make "the default pad" mean two different lengths.
	appendToggle(manager, "Spectrum", ZeropadName,   ZeropadLabel,    true);
	appendMenu (manager, "Spectrum", PadName,        PadLabel,        kPadNames,     kPadLabels,     4 /* 16384 */);
	// Planner::Auto is the Values default. The defaultIndex is a static_cast of the enum rather than
	// a literal index, which is the pattern to keep: it is the one place a table position is written
	// in terms of the enum, so reordering the enum moves this with it instead of silently opening the
	// menu on a different entry.
	appendMenu (manager, "Spectrum", PlannerName,    PlannerLabel,    kPlannerNames, kPlannerLabels, static_cast<int>(Planner::Auto));
	appendMenu (manager, "Spectrum", IngestName,     IngestLabel,     kIngestNames,  kIngestLabels,  static_cast<int>(IngestMode::Auto));
	// (v2.8.0) "Raw Linear Bins (no resampling)" was removed: it was a fourth way to say what Scale +
	// Display Max + Output Bins already say. Its only effect was to skip the frequency warp, and the
	// warp already detects the identity case and memcpy's the linear magnitude straight through
	// (see PerceptualWarping::isIdentity), so the toggle changed nothing the sliders cannot produce -
	// it only gave two spellings of one setting the chance to disagree.

	// --- Page 2: EQ (off by default: the 6 dB default boost is a prototype leftover and costs real-time budget) ---
	// Two things to know about the numbers on this page:
	//   - The blend Amount slider reaches 5.0, not 1.0, and eval() does not clamp it. The DSP treats
	//     it as a straight multiplier on the filter's change, so above 1.0 the shelf is
	//     over-applied rather than merely fully wet. Whether that is intended is not stated anywhere;
	//     if the slider is ever narrowed, eval() has no bound to narrow with it.
	//   - Q opens at 0.707, which is 1/sqrt(2) - the Butterworth (maximally flat) value, and the value
	//     bench/bench.cpp uses when it calls this the plugin default.
	// The cutoffs (1 kHz high, 200 Hz low, slider ranges spanning the nominal audio band) and the
	// +/-24 dB gains are ordinary shelf settings; none of them is derived in this file.
	appendToggle(manager, "EQ", EqenableName,  EqenableLabel,  false);
	appendToggle(manager, "EQ", HighshelfName, HighshelfLabel, true);
	appendToggle(manager, "EQ", LowshelfName,  LowshelfLabel,  true);
	appendFloat(manager, "EQ", GaindbName,      GaindbLabel,      6.0,   -24.0, 24.0);
	appendFloat(manager, "EQ", CutoffhzName,    CutoffhzLabel,    1000.0, 20.0, 20000.0);
	appendFloat(manager, "EQ", LowgaindbName,   LowgaindbLabel,   0.0,   -24.0, 24.0);
	appendFloat(manager, "EQ", LowcutoffhzName, LowcutoffhzLabel, 200.0,  20.0, 5000.0);
	appendFloat(manager, "EQ", QName,           QLabel,           0.707,  0.1,  4.0);
	appendFloat(manager, "EQ", AmountName,      AmountLabel,      1.0,    0.0,  5.0);

	// --- Page 3: Window & Weighting ---
	// Kaiser Beta opens at 15.0 and its slider stops at 1.0..55.0, but eval() clamps a typed value to
	// 0.0..100.0 - a wider range than the slider on both ends, on purpose (DSPModules.h calls 15 the
	// value that makes Kaiser the low-leakage choice; the ceiling is about aliasing the Bessel
	// approximation, not about the window's shape). Kaiser Beta is only read by eval() when the
	// window type is Kaiser, so the value is inert for the other five types.
	appendMenu (manager, "Window & Weighting", WindowName,    WindowLabel,    kWindowNames,    kWindowLabels,    static_cast<int>(WindowType::Kaiser));
	appendMenu (manager, "Window & Weighting", KaisermodeName, KaisermodeLabel, kKaisermodeNames, kKaisermodeLabels, static_cast<int>(BetaMode::Manual));
	appendFloat(manager, "Window & Weighting", KaiserName,    KaiserLabel,    15.0, 1.0, 55.0);
	appendMenu (manager, "Window & Weighting", WeightingName, WeightingLabel, kWeightingNames, kWeightingLabels, static_cast<int>(Weighting::Off));
	appendMenu (manager, "Window & Weighting", MagnormName,   MagnormLabel,   kMagnormNames,   kMagnormLabels,   static_cast<int>(MagNorm::CoherentGain));

	// --- Page 4: Loudness & Ballistics ---
	// The dB group (dB Reference, dB Range Floor) is only READ by eval() when Loudness Mode is not
	// Off, and the attack/release pair is only read when Ballistics Enable is on - that gating is why
	// a disabled section costs no TouchDesigner parameter fetches. It also means a value can sit on
	// this page looking meaningful while nothing consults it.
	// The numbers: the 80 dB floor and the 50/200 ms attack/release are the same defaults as the
	// Values struct and are ordinary starting points - none is derived in this file. The slider ranges
	// deserve a look before you trust them: the ms sliders stop at 2000/5000 ms while eval() applies
	// only a lower bound (std::max(0.0, ...)) and no upper bound at all, and the coefficient sliders
	// stop exactly where eval() clamps (0.99), which DSPModules.h says is deliberate.
	appendMenu (manager, "Loudness & Ballistics", LoudnessName,  LoudnessLabel,  kLoudnessNames, kLoudnessLabels, static_cast<int>(Loudness::Off));
	appendMenu (manager, "Loudness & Ballistics", DbrefName,     DbrefLabel,     kDbrefNames,    kDbrefLabels,    static_cast<int>(DbRef::FramePeak));
	appendFloat(manager, "Loudness & Ballistics", DbrangeName,   DbrangeLabel,   80.0, 10.0, 160.0);
	appendToggle(manager, "Loudness & Ballistics", BallenableName, BallenableLabel, false);
	appendMenu (manager, "Loudness & Ballistics", BallmodeName,  BallmodeLabel,  kBallmodeNames, kBallmodeLabels, static_cast<int>(BallisticsMode::Coefficient));
	appendFloat(manager, "Loudness & Ballistics", AttackName,    AttackLabel,    0.0, 0.0, 0.99);
	appendFloat(manager, "Loudness & Ballistics", ReleaseName,   ReleaseLabel,   0.0, 0.0, 0.99);
	appendFloat(manager, "Loudness & Ballistics", AttackmsName,  AttackmsLabel,  50.0, 0.0, 2000.0);
	appendFloat(manager, "Loudness & Ballistics", ReleasemsName, ReleasemsLabel, 200.0, 0.0, 5000.0);
	// Reset is a PULSE, not a toggle: it has no stored value and no entry in Parameters::Values.
	// TouchDesigner calls the operator's pulse callback when the user presses it, and that callback
	// (FFT::pulsePressed(), source/FFT.cpp) compares the incoming name against Parameters::ResetName
	// - one of the few places a parameter name is referenced by string outside this file, and the
	// reason the Name text must not be edited. Reset clears the EQ filters' history, asks for a
	// pipeline reset and clears the error text; it needs no default and no eval() read.
	// It is written out long-hand rather than through a helper because a pulse is the only control
	// that takes a name/label and nothing else - a helper for one call site would be noise.
	{
		TD::OP_NumericParameter np;
		np.page = "Loudness & Ballistics";
		np.name = ResetName;
		np.label = ResetLabel;
		manager->appendPulse(np);
	}

	// --- Page 5: Performance ---
	// Both controls on this page are toggles, and both default to the shipped library / the on state:
	// Async on (analysis off the cook thread) and FFT Backend off (the vendored FFTW3 build). Neither
	// choice can make the node stop producing a spectrum - see the backend note below.
	appendToggle(manager, "Performance", AsyncName,       AsyncLabel,       true);
	// Off = the vendored FFTW3 build (libfftw3f-3.3.11-avx2.dll), which is what ships with the plugin.
	// On = Intel oneMKL's FFTW3 interface (mkl_rt.3.dll first, then mkl_rt.2.dll / mkl_rt.dll as
	// fallbacks - see the dlls[] list in FftBackend.h), which dispatches to Intel-optimised kernels
	// on Intel CPUs. Neither library is linked in: both are resolved with LoadLibrary + GetProcAddress
	// at run time, because they export the same fftwf_* names and would collide at link time. If the
	// selected library is not present the plugin logs which file it looked for and falls back to FFTW3,
	// so this toggle can never make the node stop producing a spectrum.
	appendToggle(manager, "Performance", FftbackendName,  FftbackendLabel,  false);
	appendMenu (manager, "Performance", WorkerwakeName,  WorkerwakeLabel,  kWorkerwakeNames, kWorkerwakeLabels, static_cast<int>(WorkerWake::Poll));
	appendMenu (manager, "Performance", WorkerprioName,  WorkerprioLabel,  kWorkerprioNames, kWorkerprioLabels, static_cast<int>(WorkerPriority::Highest));
	// Spectral features go to the Info CHOP (feature_* channels), computed on the worker from the linear
	// magnitude. Off by default: ~2-4 us per analysis when on.
	appendToggle(manager, "Performance", FeaturesName,    FeaturesLabel,    false);
	// (v2.7.0) "Update Every N Cooks" / "Parameter Poll Every N Cooks" removed: both made the node
	// strictly worse. Update Every N Cooks halved the spectrum's effective update rate to save a cost
	// that the async worker already took off the cook thread; Parameter Poll Every N Cooks saved
	// ~18 getPar* calls (~20 us) on the cooks in between, at the price of a parameter change taking up
	// to N frames to show up. eval() now runs on every cook, so a parameter change lands on the next
	// frame. (eval still skips the blocks a section toggle has switched off - see the early-outs below.)
	// (v2.8.0) "FFT Threads" was tried here and withdrawn the same day. FFTW cannot split a single 1-D
	// transform across threads - measured, nthreads > 1 makes one transform 13-51 % slower - so it
	// bought nothing for the default Mono Mix. Worse, fftwf_plan_with_nthreads sets process-global
	// sticky state: the value set by this node leaked into every plan created afterwards, in this node
	// and in every other FFT node in the session, so changing the menu crashed TouchDesigner.
	// (v2.8.0) "Parallel Channels" / "Parallel Min Channels" came back with it and went again for the
	// same reason in the other direction: this node is one mono channel per node instance, so several
	// channels means several nodes, and a per-channel fan-out knob can never fire. The fan-out itself
	// stays (unconditional, >1 channel only) for Channels = All Channels, which is the one mode that
	// still produces more than one transform per cook.
	// (v2.9.0) FFT Backend is the second control on this page, and the only one on it that is not
	// about threading. Job scheduling stays here rather than on Spectrum because the backend and the
	// async worker interact: the parallel fan-out and the background plan both run inside whichever
	// library is selected, so the toggle and the worker belong side by side.
}

/*
eval() - READS THE UI. Runs once per cook.

WHAT IT DOES: fetches the current value of every parameter and returns them packed into a
Parameters::Values snapshot. The DSP core only ever sees that snapshot, which is what keeps
AnalysisPipeline/DSPModules free of TouchDesigner types and testable headlessly.

WHO CALLS IT: FFT::pollParameters() (source/FFT.cpp), from getOutputInfo(), which TouchDesigner
always calls immediately before execute() - so the snapshot is fresh for every cook and execute()
normally does not re-read. The Info DAT/popup callbacks read the snapshot or cached telemetry, never
the UI. Contrast setup() (above), which runs once at load and only registers.

HOW IT IS ORGANIZED: one block per page, in the same order as setup() so the two read side by side.
Three patterns run through all of it:

  * THE COUNTER. `n` counts every getPar* call and is reported through `reads` at the end. It is
    telemetry only - the Info DAT shows it - but it is what makes "a disabled section costs zero
    fetches" measurable rather than a claim, so keep the increments inside the getI/getD lambdas
    rather than at the call sites. The two numbers to know: 20 getPar* calls at the shipped
    defaults (every optional section off), 33 with EQ + a dB mode + ballistics + the Kaiser-beta
    read all switched on. The live count is Info CHOP channel 13, `param_reads` (see
    FFT::getInfoCHOPChan in FFT.cpp), and the elapsed time is channel 12, `param_fetch_us`;
    divide one by the other for the real per-read cost instead of an estimate.
  * THE MENU GUARD. menu() falls back to a named entry when the stored index is out of range. This is
    the defence against a corrupt project or a stale parameter file, and it is why adding a menu
    entry needs no change here - the fallback is a value, not an index.
  * THE CLAMPS. Every clamp below restates a bound from Parameters.h, and the same numbers appear
    again as slider limits in setup(). The three copies (Values default, slider range, clamp) are
    maintained by hand.

WHAT IT DELIBERATELY DOES NOT DO: it does not validate for the DSP's benefit beyond those clamps.
Values that have no upper bound in Parameters.h (Log Floor, Display Max, the ms times, Q, Amount,
the shelf gains and cutoffs) are passed through as typed - see the hard-limits block in Parameters.h
for the list of what is bounded and what is not.

A NULL `inputs` RETURNS THE DEFAULTS. That early-out is what makes the Values defaults load-bearing
on a cook with no input; see the note on the struct in Parameters.h.

NOTE ON THE DISABLED SECTIONS: when a section toggle is off, the fields behind it keep their
defaults rather than being zeroed, and nothing downstream reads them. So the snapshot is not "the
state of the dialog" for a disabled section - it is "whatever the defaults say", which is fine
because the DSP branches on the toggle first.
*/
Values eval(const TD::OP_Inputs* inputs, int* reads)
{
	Values v;
	if (!inputs) return v;
	int n = 0;
	auto getI = [&](const char* name) { ++n; return inputs->getParInt(name); };
	auto getD = [&](const char* name) { ++n; return inputs->getParDouble(name); };
	auto menu = [&](auto fallback, const char* name) {
		using E = decltype(fallback);
		int m = getI(name);
		return (m < 0 || m >= static_cast<int>(E::COUNT)) ? fallback : static_cast<E>(m);
	};

	// --- Spectrum (always) ---
	// This block is read on EVERY cook - no toggle gates it, because it defines the transform itself.
	// The literal fallbacks below (24000.0, 16384, 3175) are a THIRD copy of the defaults: the same
	// numbers appear as the Values field initializers in Parameters.h and as the registered defaults
	// in setup() above. They are repeated rather than referenced because they are guarding a
	// DIFFERENT failure - not "the user left it alone" but "the stored value is missing or garbage"
	// (a parameter file from an older version). Change a default and all three must move together.
	// The numeric literals have no derivation in this file; they are the documented defaults.
	v.preset     = menu(Preset::Custom, PresetName);
	v.chanMode   = menu(ChanMode::MonoMix, ChanmodeName);
	v.rawBins    = getI(RawbinsName) != 0;
	v.scale      = menu(Scale::Log, ScaleName);
	v.displayMax = getD(DisplaymaxName);
	if (!(v.displayMax > 0.0)) v.displayMax = 24000.0;   // written as a negated comparison so a NaN also lands on the default
	{
		int bins = getI(BinsName);
		v.bins = (bins <= 0) ? 16384 : std::clamp(bins, kMinBins, kMaxBins);   // a hard clamp, not the slider: a typed or scripted value can reach 8..262144
	}
	v.binsMode   = menu(BinsMode::Auto, BinsmodeName);
	v.warp       = std::clamp(getD(WarpName), 0.0, 1.0);
	v.warpInterp = menu(WarpInterp::Linear, WarpinterpName);
	v.warpAggregate = menu(WarpAggregate::Peak, WarpaggName);
	v.logFloor   = std::max(1.0, getD(LogfloorName));
	v.winMode    = menu(WinMode::Samples, WinmodeName);
	if (v.winMode == WinMode::Milliseconds) {
		v.winMs = std::clamp(getD(WinmsName), 0.1, 5000.0);   // the hard range is WIDER than the slider (1..1000 ms) on both ends
	} else {
		int ws = getI(WinsamplesName);
		v.winSamples = (ws <= 0) ? 3175 : std::clamp(ws, 1, kMaxWinSamples);   // 3175 is the default window (72 ms at 44.1 kHz); only the lower bound is a clamp, so a huge typed value still stops at kMaxWinSamples
	}
	v.zeroPad    = getI(ZeropadName) != 0;
	v.padIndex   = getI(PadName);
	v.padSize    = (v.padIndex >= 0 && v.padIndex < kPadCount) ? kPadValues[v.padIndex] : kPadDefault;
	v.planner    = menu(Planner::Auto, PlannerName);
	v.ingestMode = menu(IngestMode::Auto, IngestName);

	// --- EQ (only when enabled) ---
	// eqEnable itself is always fetched - it is the gate. Everything inside is skipped when it is
	// off, which is the "no EQ parameter reads" promise the field comment makes. Note the shelf
	// offsets are also skipped per shelf: with High Shelf off, gainDb is set to 0.0 (flat) and
	// cutoffHz back to 1000.0, i.e. the parameter's default - so turning a shelf off does not just
	// mute its effect, it discards the value, and turning it back on restores the default rather
	// than what the user had. The 0.0/1000.0/200.0 here are the Values defaults for those fields.
	v.eqEnable = getI(EqenableName) != 0;
	if (v.eqEnable) {
		v.highShelf   = getI(HighshelfName) != 0;
		v.lowShelf    = getI(LowshelfName) != 0;
		v.gainDb      = v.highShelf ? getD(GaindbName) : 0.0;
		v.cutoffHz    = v.highShelf ? getD(CutoffhzName) : 1000.0;
		v.lowGainDb   = v.lowShelf ? getD(LowgaindbName) : 0.0;
		v.lowCutoffHz = v.lowShelf ? getD(LowcutoffhzName) : 200.0;
		v.q           = getD(QName);        // no clamp and no bounds check: the slider (0.1..4.0) is the only limit
		v.amount      = getD(AmountName);   // likewise; the slider reaches 5.0 and the DSP treats it as a plain multiplier
	}

	// --- Window & weighting ---
	// No toggle gates this block: the window and the weighting curve are part of the analysis
	// definition. Kaiser Beta is fetched only for the Kaiser window (the other five types ignore the
	// value, so the fetch would be pure cost); for them the field keeps its Values default. The clamp
	// is 0..100, wider than the 1..55 slider, and DSPModules.h explains the range.
	v.window     = menu(WindowType::Kaiser, WindowName);
	if (v.window == WindowType::Kaiser) {
		v.betaMode = menu(BetaMode::Manual, KaisermodeName);
		if (v.betaMode == BetaMode::Manual) v.kaiserBeta = std::clamp(getD(KaiserName), 0.0, 100.0);
	}
	v.weighting  = menu(Weighting::Off, WeightingName);
	v.magNorm    = menu(MagNorm::CoherentGain, MagnormName);

	// --- Loudness (dB options only when a dB mode is active) ---
	// Loudness Mode itself is always fetched. The dB reference and the dB floor are only meaningful
	// in a dB mode, so when Loudness is Off they are not read at all and the fields keep their
	// defaults - which is why the Info DAT can show a dB floor that no mode is using.
	v.loudness = menu(Loudness::Off, LoudnessName);
	if (v.loudness != Loudness::Off) {
		v.dbRef   = menu(DbRef::FramePeak, DbrefName);
	}
	// dB Range Floor is also what Kaiser Beta Mode = Auto designs the window for (the sidelobes only need
	// to sit below the floor the display shows), so it is read whenever either consumer is live.
	if (v.loudness != Loudness::Off || (v.window == WindowType::Kaiser && v.betaMode == BetaMode::Auto)) {
		v.dbRange = std::max(1e-3, getD(DbrangeName));   // lower guard only: DecibelConverter takes 1/top_db
	}

	// --- Ballistics (only when enabled) ---
	// Same shape as the EQ gate: ballEnable is always fetched, and the mode plus the two values it
	// selects are read only when it is on. Which pair is read depends on the mode, so a stale value
	// in the other unit survives in the snapshot - harmless, because runChannel() converts whichever
	// pair the mode names.
	v.ballEnable = getI(BallenableName) != 0;
	if (v.ballEnable) {
		v.ballMode = menu(BallisticsMode::Coefficient, BallmodeName);
		if (v.ballMode == BallisticsMode::Milliseconds) {
			v.attackMs  = std::max(0.0, getD(AttackmsName));    // lower bound only; the 2000/5000 ms sliders are the only upper limit
			v.releaseMs = std::max(0.0, getD(ReleasemsName));
		} else {
			v.attack  = std::clamp(getD(AttackName), 0.0, 0.99);    // the 0.99 clamp is required, not cosmetic: DSPModules.h explains that a coefficient of 1 freezes the state forever
			v.release = std::clamp(getD(ReleaseName), 0.0, 0.99);
		}
	}

	// --- Performance ---
	v.async       = getI(AsyncName) != 0;
	// Read unconditionally (one fetch) so switching it takes effect on the next cook in both the async
	// and the synchronous path. See Parameters.h -> Backend for why this is a toggle and not a menu.
	v.backend     = (getI(FftbackendName) != 0) ? Backend::OneMkl : Backend::Fftw3;
	v.workerWake  = menu(WorkerWake::Poll, WorkerwakeName);
	v.workerPriority = menu(WorkerPriority::Highest, WorkerprioName);
	v.features    = getI(FeaturesName) != 0;

	// A Quality Preset overrides the parameters it owns (RateModel.h, applyPreset) - applied last, on
	// top of what was just read, so the individual parameters keep their stored values for Custom.
	applyPreset(v);

	if (reads) *reads = n;
	return v;
}

// ---------------------------------------------------------------------------------------------
// setEnableStates - grey out what the current settings make inert (C++ API Common 3).
// TouchDesigner may call this outside a cook; it reads only through `inputs` and writes only enable
// states, so it cannot disturb the analysis. Every name used here is registered in setup().
// ---------------------------------------------------------------------------------------------
void setEnableStates(const TD::OP_Inputs* inputs, TD::OP_ParEnableState* state)
{
	if (!inputs || !state) return;
	auto en = [&](const char* name, bool on) { state->setEnableState(name, on); };
	const int preset = inputs->getParInt(PresetName);
	const bool custom = preset == static_cast<int>(Preset::Custom);
	const bool binsAuto = inputs->getParInt(BinsmodeName) == static_cast<int>(BinsMode::Auto);
	const bool raw = inputs->getParInt(RawbinsName) != 0;
	const bool zeroPad = inputs->getParInt(ZeropadName) != 0;
	const bool kaiser = inputs->getParInt(WindowName) == static_cast<int>(WindowType::Kaiser);
	const bool betaAuto = inputs->getParInt(KaisermodeName) == static_cast<int>(BetaMode::Auto);
	const bool winMs = inputs->getParInt(WinmodeName) == static_cast<int>(WinMode::Milliseconds);
	const bool loud = inputs->getParInt(LoudnessName) != static_cast<int>(Loudness::Off);
	const bool eq = inputs->getParInt(EqenableName) != 0;
	const bool ball = inputs->getParInt(BallenableName) != 0;
	const bool ballMs = inputs->getParInt(BallmodeName) == static_cast<int>(BallisticsMode::Milliseconds);
	const bool async = inputs->getParInt(AsyncName) != 0;

	// what a preset owns (and what Raw RFFT Bins / Zero-Padding off make inert)
	en(PadName, custom && zeroPad);
	en(WarpinterpName, custom && !raw);
	en(WarpaggName, custom && !raw);
	// the output axis: Raw RFFT Bins bypasses all of it; Output Bins is the count only in Fixed
	en(ScaleName, !raw);
	en(DisplaymaxName, !raw);
	en(WarpName, !raw);
	en(LogfloorName, !raw);
	en(BinsmodeName, !raw);
	en(BinsName, !raw && !binsAuto);
	en(KaisermodeName, custom && kaiser);
	// window length: the unit that is not selected
	en(WinsamplesName, !winMs);
	en(WinmsName, winMs);
	// Kaiser beta only for a manual Kaiser window
	en(KaiserName, kaiser && custom && !betaAuto);
	// dB group: Loudness on, or the Auto beta designs from the dB floor
	en(DbrefName, loud);
	en(DbrangeName, loud || (kaiser && (betaAuto || !custom)));
	// EQ sub-parameters
	en(HighshelfName, eq); en(LowshelfName, eq);
	const bool hs = eq && inputs->getParInt(HighshelfName) != 0;
	const bool ls = eq && inputs->getParInt(LowshelfName) != 0;
	en(GaindbName, hs); en(CutoffhzName, hs);
	en(LowgaindbName, ls); en(LowcutoffhzName, ls);
	en(QName, eq); en(AmountName, eq);
	// ballistics: the pair in the selected unit
	en(BallmodeName, ball);
	en(AttackName, ball && !ballMs); en(ReleaseName, ball && !ballMs);
	en(AttackmsName, ball && ballMs); en(ReleasemsName, ball && ballMs);
	// worker controls only matter with the worker
	en(WorkerwakeName, async);
	en(WorkerprioName, async);
}

} // namespace Parameters
