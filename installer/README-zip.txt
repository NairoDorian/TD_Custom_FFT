TD_Custom_FFT @@VERSION@@ - FFT Custom CHOP for TouchDesigner
https://github.com/NairoDorian/TD_Custom_FFT

Requirements: Windows 10/11 64-bit, a CPU with AVX2 + FMA, TouchDesigner 2025.30000 or later,
the Microsoft Visual C++ Redistributable (x64), current version (the installers check it).

INSTALL - pick one
  1. Double-click Install.cmd. It copies FFT\ into Documents\Derivative\Plugins\FFT, the examples into
     Documents\TD_Custom_FFT\Examples, and asks whether to add the optional Intel oneMKL backend.
  2. By hand: close TouchDesigner, copy the FFT folder into Documents\Derivative\Plugins\
     (so that Documents\Derivative\Plugins\FFT\FFT.dll exists). Keep every file of the folder together:
     FFT.dll loads libfftw3f-3.3.11-avx2.dll from its own folder.

Then start TouchDesigner, press Tab and create CHOP > FFT Custom. If TouchDesigner asks whether to load
the plugin, allow it. Open Examples\Circular_Audio_Spectrum.toe to see it working.

OPTIONAL: Intel oneMKL backend
  The node's Performance page has an "FFT Backend" toggle: off = FFTW3 (included), on = Intel oneMKL, a
  faster FFT on the analysis thread. oneMKL is not included (456 MB); "Install.cmd -OneMKL" downloads the
  tested version from nuget.org and checks its SHA-256. Without it the toggle falls back to FFTW3 and the
  node shows a warning saying so.

UPDATE: run Install.cmd from the newer zip. REMOVE: "Install.cmd -Uninstall".

License: GPL-3.0-or-later with a TouchDesigner linking permission (FFT\FFT-LICENSE.txt, FFT\FFT-NOTICE.md).
FFTW 3.3.11: GPL-2.0-or-later (FFT\FFTW3-COPYING.txt); its source is attached to the GitHub release.
Third-party notices: FFT\THIRD_PARTY_NOTICES.md.
