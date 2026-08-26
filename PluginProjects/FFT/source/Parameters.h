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

#include "CHOP_CPlusPlusBase.h"

namespace Parameters {

// ---------------------------------------------------------------------------
// Menus (order == menu order in Parameters.cpp)
// ---------------------------------------------------------------------------
enum class Scale : int { Log = 0, Mel, ERB, Bark, Chroma, Linear, Melog, COUNT };
enum class WindowType : int { Kaiser = 0, Hann, Hamming, Blackman, BlackmanHarris, Rectangular, COUNT };
enum class Weighting : int { Off = 0, AWeighting, CWeighting, ITU468, COUNT };
enum class Loudness : int { Off = 0, Db, DbNorm, COUNT };
enum class WinMode : int { Samples = 0, Milliseconds, COUNT };
enum class Planner : int { Auto = 0, Fast, Measured, COUNT };
enum class MagNorm : int { CoherentGain = 0, FullScale, COUNT };
enum class DbRef : int { FramePeak = 0, Dbfs, Agc, COUNT };
enum class BallisticsMode : int { Coefficient = 0, Milliseconds, COUNT };

// Zero-padded FFT transform lengths, indexed by the "Pad" menu (menu order in Parameters.cpp)
inline constexpr int kPadValues[7] = { 1024, 2048, 4096, 8192, 16384, 32768, 65536 };
inline constexpr int kPadCount = 7;
inline constexpr int kPadDefault = 32768;

// Hard limits (defensive: the sliders are soft limits, typed values are not)
inline constexpr int    kMinBins        = 8;
inline constexpr int    kMaxBins        = 262144;
inline constexpr int    kMaxWinSamples  = 65536;
inline constexpr int    kMaxChannels    = 64;
inline constexpr double kMinSampleRate  = 1.0;
inline constexpr double kMaxSampleRate  = 384000.0;

// ---------------------------------------------------------------------------
// Names & labels
// ---------------------------------------------------------------------------
// Page 1: Spectrum
constexpr char ScaleName[]        = "Scale";        constexpr char ScaleLabel[]        = "Scale";
constexpr char DisplaymaxName[]   = "Displaymax";   constexpr char DisplaymaxLabel[]   = "Display Max Hz";
constexpr char BinsName[]         = "Bins";         constexpr char BinsLabel[]         = "Output Bins";
constexpr char WarpName[]         = "Warp";         constexpr char WarpLabel[]         = "Warp Blend";
constexpr char LogfloorName[]     = "Logfloor";     constexpr char LogfloorLabel[]     = "Log Floor Hz";
constexpr char WinmodeName[]      = "Winmode";      constexpr char WinmodeLabel[]      = "Window Length Mode";
constexpr char WinsamplesName[]   = "Winsamples";   constexpr char WinsamplesLabel[]   = "Window Sampling";
constexpr char WinmsName[]        = "Winms";        constexpr char WinmsLabel[]        = "Window Length ms";
constexpr char PadName[]          = "Pad";          constexpr char PadLabel[]          = "Zero-Pad Len";
constexpr char PlannerName[]      = "Planner";      constexpr char PlannerLabel[]      = "FFT Planner";

// Page 2: EQ
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
constexpr char BallmodeName[]     = "Ballmode";     constexpr char BallmodeLabel[]     = "Ballistics Mode";
constexpr char AttackName[]       = "Attack";       constexpr char AttackLabel[]       = "Attack Speed";
constexpr char ReleaseName[]      = "Release";      constexpr char ReleaseLabel[]      = "Release Speed";
constexpr char AttackmsName[]     = "Attackms";     constexpr char AttackmsLabel[]     = "Attack ms";
constexpr char ReleasemsName[]    = "Releasems";    constexpr char ReleasemsLabel[]    = "Release ms";
constexpr char ResetName[]        = "Reset";        constexpr char ResetLabel[]        = "Reset";

// Page 5: Performance
constexpr char ParallelName[]     = "Parallel";     constexpr char ParallelLabel[]     = "Parallel Channels";
constexpr char ParallelminName[]  = "Parallelmin";  constexpr char ParallelminLabel[]  = "Parallel Min Channels";

// ---------------------------------------------------------------------------
// Snapshot of every parameter for one cook
// ---------------------------------------------------------------------------
struct Values {
    // Spectrum
    Scale      scale        = Scale::Log;
    double     displayMax   = 24000.0;
    int        bins         = 16384;
    double     warp         = 0.963;
    double     logFloor     = 20.0;
    WinMode    winMode      = WinMode::Samples;
    int        winSamples   = 3175;
    double     winMs        = 72.0;
    int        padIndex     = 5;          // index into kPadValues
    int        padSize      = kPadDefault;
    Planner    planner      = Planner::Auto;
    // EQ
    double     gainDb       = 6.0;
    double     cutoffHz     = 1000.0;
    double     lowGainDb    = 0.0;
    double     lowCutoffHz  = 200.0;
    double     q            = 0.707;
    double     amount       = 1.0;
    // Window & weighting
    WindowType window       = WindowType::Kaiser;
    double     kaiserBeta   = 15.0;
    Weighting  weighting    = Weighting::Off;
    MagNorm    magNorm      = MagNorm::CoherentGain;
    // Loudness & ballistics
    Loudness   loudness     = Loudness::Off;
    DbRef      dbRef        = DbRef::FramePeak;
    double     dbRange      = 80.0;
    BallisticsMode ballMode = BallisticsMode::Coefficient;
    double     attack       = 0.0;
    double     release      = 0.0;
    double     attackMs     = 50.0;
    double     releaseMs    = 200.0;
    // Performance
    bool       parallel     = true;
    int        parallelMin  = 4;
};

// Fetch all parameters once (clamps every value into its valid range)
Values eval(const TD::OP_Inputs* inputs);

// Registers custom parameters and UI controls with TouchDesigner's parameter manager
void setup(TD::OP_ParameterManager* manager);

} // namespace Parameters

#endif // PARAMETERS_H
