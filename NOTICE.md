# Plugin_FFT — copyright and license

TouchDesigner Custom FFT CHOP (`Plugin_FFT`, `TD_Custom_FFT`)
Copyright (C) 2026 NairoDorian

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License along with this program (the file
[`LICENSE`](LICENSE)). If not, see <https://www.gnu.org/licenses/>.

SPDX: `GPL-3.0-or-later` with the additional permission below.

## Additional permission under GNU GPL version 3 section 7

If you modify this Program, or any covered work, by linking or combining it with the TouchDesigner C++ SDK
(or a modified version of that library) — the CPlusPlus operator headers and sample code published by
Derivative Inc. — or with TouchDesigner itself, containing parts covered by the terms of Derivative's Shared
Use License or of the TouchDesigner license, the licensors of this Program grant you additional permission
to convey the resulting work. Corresponding Source for a non-source form of such a combination shall include
the source code for the parts of the TouchDesigner C++ SDK used as well as that of the covered work.

**Why this permission exists.** A TouchDesigner operator plugin is compiled against Derivative's SDK
headers, and `source/FFT.cpp` and `source/Parameters.cpp` started from Derivative's CPlusPlus CHOP sample,
whose license requires its notice to be kept on the file and limits use to TouchDesigner. Without this
permission the GPL would forbid distributing the plugin at all. The permission covers this project's own
code; it does not change the license of any third-party component.

## Which terms apply to which file

| Files | Terms |
|---|---|
| Everything in this repository not listed below | GPL-3.0-or-later with the additional permission above (`SPDX-License-Identifier` line at the top of each source file) |
| `PluginProjects/FFT/source/FFT.cpp`, `PluginProjects/FFT/source/Parameters.cpp` | Mixed: this project's code is GPL-3.0-or-later with the additional permission above; the portions derived from Derivative's CPlusPlus CHOP sample remain under Derivative's Shared Use License, whose notice is kept in each file as that license requires |
| The TouchDesigner SDK headers the plugin is compiled against (they live in `../PluginBuilder_V2/include/`, not here) | Derivative's Shared Use License (notice in each header) |
| `PluginProjects/FFT/3rdParty/fftw3/` (FFTW 3.3.11: DLL, import library, headers) | GPL-2.0-or-later — see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) and `3rdParty/fftw3/SOURCE.md` |
| Intel oneMKL (optional backend; not in this repository) | Intel Simplified Software License — see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) |

Third-party components and how to obtain their source: [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
