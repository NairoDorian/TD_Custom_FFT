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
1. Spectrum:               scale, display range, output bins, warp, log floor, window length
                           (samples or ms), zero-pad size, FFT planner policy.
2. EQ:                     high/low shelf gain + cutoff, Q, wet/dry.
3. Window & Weighting:     window type, Kaiser beta, equal-loudness weighting, magnitude normalization.
4. Loudness & Ballistics:  linear/dB/dB-normalized, dB reference, dB floor, attack/release
                           (per-frame coefficient or milliseconds), reset.
5. Performance:            per-channel multithreading.

Defaults reproduce the behaviour of the previous version exactly (Window Length Mode = Samples,
Magnitude Normalization = Coherent Gain, dB Reference = Frame Peak, Ballistics Mode = Coefficient).
===========================================================================
*/

#include "Parameters.h"

#include <algorithm>
#include <cmath>

namespace Parameters {

namespace {

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

void appendToggle(TD::OP_ParameterManager* manager, const char* page, const char* name, const char* label, bool def)
{
	TD::OP_NumericParameter np;
	np.page = page;
	np.name = name;
	np.label = label;
	np.defaultValues[0] = def ? 1.0 : 0.0;
	manager->appendToggle(np);
}

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
const char* kPlannerNames[]    = { "Auto", "Fast", "Measured" };
const char* kPlannerLabels[]   = { "Auto (instant plan, measured plan upgraded in background)", "Fast (Estimate only, never stalls)", "Measured (blocking measure once per size, wisdom cached)" };
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

} // namespace

void setup(TD::OP_ParameterManager* manager)
{
	// --- Page 1: Spectrum ---
	appendMenu (manager, "Spectrum", ChanmodeName,   ChanmodeLabel,   kChanmodeNames, kChanmodeLabels, static_cast<int>(ChanMode::MonoMix));
	appendMenu (manager, "Spectrum", ScaleName,      ScaleLabel,      kScaleNames,   kScaleLabels,   static_cast<int>(Scale::Log));
	appendFloat(manager, "Spectrum", DisplaymaxName, DisplaymaxLabel, 24000.0, 100.0, 48000.0);
	appendInt  (manager, "Spectrum", BinsName,       BinsLabel,       16384, 256, 32768);
	appendFloat(manager, "Spectrum", WarpName,       WarpLabel,       0.963, 0.0, 1.0);
	appendMenu (manager, "Spectrum", WarpinterpName, WarpinterpLabel, kWarpinterpNames, kWarpinterpLabels, static_cast<int>(WarpInterp::Linear));
	appendFloat(manager, "Spectrum", LogfloorName,   LogfloorLabel,   20.0, 1.0, 500.0);
	appendMenu (manager, "Spectrum", WinmodeName,    WinmodeLabel,    kWinmodeNames, kWinmodeLabels, static_cast<int>(WinMode::Samples));
	appendInt  (manager, "Spectrum", WinsamplesName, WinsamplesLabel, 3175, 1, 32768);
	appendFloat(manager, "Spectrum", WinmsName,      WinmsLabel,      72.0, 1.0, 1000.0);
	appendMenu (manager, "Spectrum", PadName,        PadLabel,        kPadNames,     kPadLabels,     5 /* 32768 */);
	appendMenu (manager, "Spectrum", PlannerName,    PlannerLabel,    kPlannerNames, kPlannerLabels, static_cast<int>(Planner::Auto));

	// --- Page 2: EQ (off by default: the 6 dB default boost is a prototype leftover and costs real-time budget) ---
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
	appendMenu (manager, "Window & Weighting", WindowName,    WindowLabel,    kWindowNames,    kWindowLabels,    static_cast<int>(WindowType::Kaiser));
	appendFloat(manager, "Window & Weighting", KaiserName,    KaiserLabel,    15.0, 1.0, 55.0);
	appendMenu (manager, "Window & Weighting", WeightingName, WeightingLabel, kWeightingNames, kWeightingLabels, static_cast<int>(Weighting::Off));
	appendMenu (manager, "Window & Weighting", MagnormName,   MagnormLabel,   kMagnormNames,   kMagnormLabels,   static_cast<int>(MagNorm::CoherentGain));

	// --- Page 4: Loudness & Ballistics ---
	appendMenu (manager, "Loudness & Ballistics", LoudnessName,  LoudnessLabel,  kLoudnessNames, kLoudnessLabels, static_cast<int>(Loudness::Off));
	appendMenu (manager, "Loudness & Ballistics", DbrefName,     DbrefLabel,     kDbrefNames,    kDbrefLabels,    static_cast<int>(DbRef::FramePeak));
	appendFloat(manager, "Loudness & Ballistics", DbrangeName,   DbrangeLabel,   80.0, 10.0, 160.0);
	appendToggle(manager, "Loudness & Ballistics", BallenableName, BallenableLabel, false);
	appendMenu (manager, "Loudness & Ballistics", BallmodeName,  BallmodeLabel,  kBallmodeNames, kBallmodeLabels, static_cast<int>(BallisticsMode::Coefficient));
	appendFloat(manager, "Loudness & Ballistics", AttackName,    AttackLabel,    0.0, 0.0, 0.99);
	appendFloat(manager, "Loudness & Ballistics", ReleaseName,   ReleaseLabel,   0.0, 0.0, 0.99);
	appendFloat(manager, "Loudness & Ballistics", AttackmsName,  AttackmsLabel,  50.0, 0.0, 2000.0);
	appendFloat(manager, "Loudness & Ballistics", ReleasemsName, ReleasemsLabel, 200.0, 0.0, 5000.0);
	{
		TD::OP_NumericParameter np;
		np.page = "Loudness & Ballistics";
		np.name = ResetName;
		np.label = ResetLabel;
		manager->appendPulse(np);
	}

	// --- Page 5: Performance ---
	appendToggle(manager, "Performance", AsyncName,       AsyncLabel,       true);
	appendInt   (manager, "Performance", UpdateeveryName, UpdateeveryLabel, 1, 1, 16);
	appendInt   (manager, "Performance", ParampollName,   ParampollLabel,   1, 1, 16);
	// Off by default: for a handful of channels the per-frame thread-pool wake-ups cost more
	// than the ~60 us of work per channel they distribute, and they add frame-time jitter.
	appendToggle(manager, "Performance", ParallelName,    ParallelLabel,    false);
	appendInt   (manager, "Performance", ParallelminName, ParallelminLabel, 8, 2, 64);
}

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
	v.chanMode   = menu(ChanMode::MonoMix, ChanmodeName);
	v.scale      = menu(Scale::Log, ScaleName);
	v.displayMax = getD(DisplaymaxName);
	if (!(v.displayMax > 0.0)) v.displayMax = 24000.0;
	{
		int bins = getI(BinsName);
		v.bins = (bins <= 0) ? 16384 : std::clamp(bins, kMinBins, kMaxBins);
	}
	v.warp       = std::clamp(getD(WarpName), 0.0, 1.0);
	v.warpInterp = menu(WarpInterp::Linear, WarpinterpName);
	v.logFloor   = std::max(1.0, getD(LogfloorName));
	v.winMode    = menu(WinMode::Samples, WinmodeName);
	if (v.winMode == WinMode::Milliseconds) {
		v.winMs = std::clamp(getD(WinmsName), 0.1, 5000.0);
	} else {
		int ws = getI(WinsamplesName);
		v.winSamples = (ws <= 0) ? 3175 : std::clamp(ws, 1, kMaxWinSamples);
	}
	v.padIndex   = getI(PadName);
	v.padSize    = (v.padIndex >= 0 && v.padIndex < kPadCount) ? kPadValues[v.padIndex] : kPadDefault;
	v.planner    = menu(Planner::Auto, PlannerName);

	// --- EQ (only when enabled) ---
	v.eqEnable = getI(EqenableName) != 0;
	if (v.eqEnable) {
		v.highShelf   = getI(HighshelfName) != 0;
		v.lowShelf    = getI(LowshelfName) != 0;
		v.gainDb      = v.highShelf ? getD(GaindbName) : 0.0;
		v.cutoffHz    = v.highShelf ? getD(CutoffhzName) : 1000.0;
		v.lowGainDb   = v.lowShelf ? getD(LowgaindbName) : 0.0;
		v.lowCutoffHz = v.lowShelf ? getD(LowcutoffhzName) : 200.0;
		v.q           = getD(QName);
		v.amount      = getD(AmountName);
	}

	// --- Window & weighting ---
	v.window     = menu(WindowType::Kaiser, WindowName);
	if (v.window == WindowType::Kaiser) v.kaiserBeta = std::clamp(getD(KaiserName), 0.0, 100.0);
	v.weighting  = menu(Weighting::Off, WeightingName);
	v.magNorm    = menu(MagNorm::CoherentGain, MagnormName);

	// --- Loudness (dB options only when a dB mode is active) ---
	v.loudness = menu(Loudness::Off, LoudnessName);
	if (v.loudness != Loudness::Off) {
		v.dbRef   = menu(DbRef::FramePeak, DbrefName);
		v.dbRange = std::max(1e-3, getD(DbrangeName));
	}

	// --- Ballistics (only when enabled) ---
	v.ballEnable = getI(BallenableName) != 0;
	if (v.ballEnable) {
		v.ballMode = menu(BallisticsMode::Coefficient, BallmodeName);
		if (v.ballMode == BallisticsMode::Milliseconds) {
			v.attackMs  = std::max(0.0, getD(AttackmsName));
			v.releaseMs = std::max(0.0, getD(ReleasemsName));
		} else {
			v.attack  = std::clamp(getD(AttackName), 0.0, 0.99);
			v.release = std::clamp(getD(ReleaseName), 0.0, 0.99);
		}
	}

	// --- Performance ---
	v.async       = getI(AsyncName) != 0;
	v.updateEvery = std::clamp(getI(UpdateeveryName), 1, 16);
	v.parallel = getI(ParallelName) != 0;
	if (v.parallel) v.parallelMin = std::clamp(getI(ParallelminName), 2, kMaxChannels);

	if (reads) *reads = n;
	return v;
}

int readParamPoll(const TD::OP_Inputs* inputs)
{
	return inputs ? std::clamp(inputs->getParInt(ParampollName), 1, 16) : 1;
}

ChanMode readChanMode(const TD::OP_Inputs* inputs)
{
	if (!inputs) return ChanMode::MonoMix;
	int m = inputs->getParInt(ChanmodeName);
	return (m < 0 || m >= static_cast<int>(ChanMode::COUNT)) ? ChanMode::MonoMix : static_cast<ChanMode>(m);
}

int readBins(const TD::OP_Inputs* inputs)
{
	if (!inputs) return 16384;
	int bins = inputs->getParInt(BinsName);
	return (bins <= 0) ? 16384 : std::clamp(bins, kMinBins, kMaxBins);
}

} // namespace Parameters
