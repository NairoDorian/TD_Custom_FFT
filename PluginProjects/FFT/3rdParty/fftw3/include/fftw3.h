/* ---------------------------------------------------------------------------
 * Stable include name for the vendored FFTW 3.3.11 AVX2 build.
 *
 * Every consumer writes the ordinary `#include <fftw3.h>`, so that name has to
 * keep existing no matter which FFTW version is vendored. The version-tagged
 * header next to this file is the real, byte-for-byte upstream one:
 *
 *     fftw3-3.3.11-avx2.h    sha256 c82bf45065551f8659748f9c77d8c9dee6783ea524d579e951757ac78a926d05
 *
 * Keeping the upstream file unmodified means its hash can be checked against
 * https://fftw.org/pub/fftw/ at any time. When the vendored version changes,
 * drop the new fftw3-<version>.h in beside this one and repoint the include
 * below; nothing else in the project needs to change.
 *
 * This project's pin (expected version string, DLL name, runtime check) lives
 * in the kFftw3Backend descriptor in source/FftBackend.h - its expectedVersion
 * field is the "3.3.11" that the runtime check compares against - not here.
 * This file stays a thin forwarder.
 * --------------------------------------------------------------------------- */

#include "fftw3-3.3.11-avx2.h"
