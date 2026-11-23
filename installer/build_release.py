# SPDX-License-Identifier: GPL-3.0-or-later
"""Package a built FFT plugin into the release assets.

    python installer/build_release.py [--build-dir DIR] [--tag vX.Y.Z] [--release] [--no-installer]
                                      [--fftw-source]

Reads a finished build (PluginProjects/FFT/build by default - configure and build it first, see README.md,
"Building") and writes into installer/dist/:

    TD_Custom_FFT-Setup.exe      the Inno Setup installer (TD_Custom_FFT.iss), unless --no-installer
    TD_Custom_FFT-Windows.zip    the same files for a manual or scripted install:
                                   FFT/          the plugin folder (copy into Documents\\Derivative\\Plugins\\)
                                   Examples/     the example project(s)
                                   install.ps1, Install.cmd, README.txt, manifest.json
    install.ps1                  the one-line installer, stamped with this release's tag and the zip's SHA-256
    fftw-3.3.11.tar.gz           FFTW's corresponding source (GPL), with --fftw-source (CI does this on a tag)
    SHA256SUMS.txt               of every file above

Single sources of truth, so nothing here is typed twice: the version is PluginProjects/FFT/plugin.json, the
plugin's files are FFT.dll plus the build's own runtime manifest (FFT_runtime_dlls.txt, written by
td_plugin_add_runtime_dll), the oneMKL pin is installer/onemkl.json, the Visual C++ runtime floor is the
compiler version CMake recorded for this build, the FFTW DLL and tarball are checked against
3rdParty/fftw3/VERSION.

--release (what CI passes on a tag) turns the consistency checks into errors: the tag must equal 'v' + the
plugin.json version, and FFT.dll must have been built from the current commit with a clean tree (the build
embeds `git rev-parse --short HEAD`, with "+dirty" appended for local changes - CMakeLists.txt, FFT_GIT_HASH).

Standard library only (it runs on a bare GitHub runner).
"""
from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
PROJECT = REPO / "PluginProjects" / "FFT"
FFTW_DIR = PROJECT / "3rdParty" / "fftw3"
EXAMPLES = REPO / "examples"
STAGE = HERE / "stage"
DIST = HERE / "dist"

ZIP_NAME = "TD_Custom_FFT-Windows.zip"
SETUP_NAME = "TD_Custom_FFT-Setup.exe"
# The example the installer's finish page offers to open.
EXAMPLE_MAIN = "Circular_Audio_Spectrum.toe"


def fail(msg: str) -> None:
    print(f"build_release: ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def warn(msg: str) -> None:
    print(f"build_release: warning: {msg}", file=sys.stderr)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git(*args: str) -> str:
    try:
        return subprocess.run(["git", *args], cwd=REPO, check=True, capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def read_version_record() -> dict[str, str]:
    """3rdParty/fftw3/VERSION: KEY=VALUE lines (indented lines are commentary)."""
    rec: dict[str, str] = {}
    for line in (FFTW_DIR / "VERSION").read_text(encoding="utf-8").splitlines():
        m = re.match(r"^([A-Z0-9_]+)=(.*)$", line)
        if m:
            rec[m.group(1)] = m.group(2).strip()
    return rec


def msvc_runtime_floor(build_dir: Path) -> tuple[int, int]:
    """The VC++ runtime FFT.dll needs: 14.<minor> for compiler 19.<minor> (MSVC's numbering)."""
    for f in glob.glob(str(build_dir / "CMakeFiles" / "*" / "CMakeCXXCompiler.cmake")):
        m = re.search(r'set\(CMAKE_CXX_COMPILER_VERSION "19\.(\d+)', Path(f).read_text(encoding="utf-8"))
        if m:
            return 14, int(m.group(1))
    fail(f"no MSVC compiler version under {build_dir}/CMakeFiles - is this an MSVC build tree?")
    raise AssertionError


def find_iscc() -> str | None:
    for cand in (shutil.which("iscc"), shutil.which("ISCC"),
                 os.path.expandvars(r"%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"),
                 r"C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
                 r"C:\Program Files\Inno Setup 6\ISCC.exe"):
        if cand and Path(cand).is_file():
            return cand
    return None


def iss_string(s: str) -> str:
    """An ISPP string literal (ISPP doubles a quote inside a string)."""
    return '"' + s.replace('"', '""') + '"'


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", type=Path, default=PROJECT / "build",
                    help="the configured + built CMake tree (default: PluginProjects/FFT/build)")
    ap.add_argument("--tag", default="", help="release tag (default: v<plugin.json version>)")
    ap.add_argument("--release", action="store_true",
                    help="CI mode: tag must match plugin.json, FFT.dll must come from this clean commit")
    ap.add_argument("--no-installer", action="store_true", help="skip ISCC (zip + install.ps1 only)")
    ap.add_argument("--fftw-source", action="store_true",
                    help="download FFTW's source tarball (GPL corresponding source) and add it to dist/")
    args = ap.parse_args()

    manifest = json.loads((PROJECT / "plugin.json").read_text(encoding="utf-8"))
    version = manifest["version"]
    tag = args.tag or f"v{version}"
    if tag != f"v{version}":
        (fail if args.release else warn)(f"tag {tag} does not match plugin.json version {version}")

    bin_dir = args.build_dir / "bin" / "Release"
    fft_dll = bin_dir / "FFT.dll"
    runtime_list = args.build_dir / "FFT_runtime_dlls.txt"
    if not fft_dll.is_file():
        fail(f"{fft_dll} not found - build the plugin first (Release)")
    if not runtime_list.is_file():
        fail(f"{runtime_list} not found - it is written by td_plugin_add_runtime_dll at configure time")

    # Build identity: the hash CMake embedded must be this commit's, without "+dirty" (CMakeLists.txt).
    commit = git("rev-parse", "--short", "HEAD")
    dll_bytes = fft_dll.read_bytes()
    if commit:
        built_here = commit.encode() in dll_bytes
        dirty = (commit + "+dirty").encode() in dll_bytes
        if not built_here or dirty:
            what = "from uncommitted changes (+dirty)" if dirty else f"from a commit other than {commit}"
            (fail if args.release else warn)(f"FFT.dll was built {what}; reconfigure and rebuild before releasing")
    elif args.release:
        fail("git is not available, so the build identity cannot be checked")

    vc_major, vc_minor = msvc_runtime_floor(args.build_dir)
    onemkl = json.loads((HERE / "onemkl.json").read_text(encoding="utf-8"))
    onemkl.pop("_comment", None)
    record = read_version_record()

    # ---- stage ------------------------------------------------------------------------------------------
    if STAGE.exists():
        shutil.rmtree(STAGE)
    (STAGE / "FFT").mkdir(parents=True)
    (STAGE / "Examples").mkdir()

    plugin_files = ["FFT.dll"] + [ln.strip() for ln in runtime_list.read_text(encoding="utf-8").splitlines() if ln.strip()]
    for name in plugin_files:
        src = bin_dir / name
        if not src.is_file():
            fail(f"{src} is in the runtime manifest but not in the build output")
        shutil.copy2(src, STAGE / "FFT" / name)
    fftw_dll = STAGE / "FFT" / record["FFTW_VENDORED_DLL"]
    if not fftw_dll.is_file():
        fail(f"the runtime manifest does not name {fftw_dll.name}")
    if sha256(fftw_dll) != record["FFTW_VENDORED_DLL_SHA256"]:
        fail(f"{fftw_dll.name} does not match FFTW_VENDORED_DLL_SHA256 in 3rdParty/fftw3/VERSION")

    examples = sorted(p.name for p in EXAMPLES.glob("*.toe"))
    if EXAMPLE_MAIN not in examples:
        fail(f"examples/{EXAMPLE_MAIN} is missing")
    for name in examples:
        shutil.copy2(EXAMPLES / name, STAGE / "Examples" / name)

    # release.iss: everything TD_Custom_FFT.iss needs from this build, as ISPP defines.
    dlls, lic = onemkl["dlls"], onemkl["licenses"]
    lines = [
        "; Generated by installer/build_release.py - do not edit (and not committed).",
        f"#define AppVersion {iss_string(version)}",
        f"#define AppTag {iss_string(tag)}",
        f"#define MinVcMajor {vc_major}",
        f"#define MinVcMinor {vc_minor}",
        f"#define ExampleMain {iss_string(EXAMPLE_MAIN)}",
        f"#define OneMklVersion {iss_string(onemkl['package_version'])}",
        f"#define OneMklUrl {iss_string(onemkl['url'])}",
        f"#define OneMklSha256 {iss_string(onemkl['sha256'])}",
        f"#define OneMklFileVersion {iss_string(onemkl['file_version'])}",
        f"#define OneMklInstalledSize {int(onemkl['installed_size'])}",
        f"#define OneMklDllDir {iss_string(onemkl['dll_dir'].replace('/', chr(92)))}",
        f"#define OneMklMarkerFile {iss_string(onemkl['marker'])}",
        f"#define OneMklMarker {iss_string(onemkl['package'] + ' ' + onemkl['package_version'])}",
        f"#define OneMklDllCsv {iss_string(','.join(dlls))}",
        f"#dim OneMklDlls[{len(dlls)}]",
    ]
    lines += [f"#define OneMklDlls[{i}] {iss_string(d)}" for i, d in enumerate(dlls)]
    lines += [f"#dim OneMklLicSrc[{len(lic)}]", f"#dim OneMklLicDst[{len(lic)}]"]
    for i, (src, dst) in enumerate(lic.items()):
        lines += [f"#define OneMklLicSrc[{i}] {iss_string(src.replace('/', chr(92)))}",
                  f"#define OneMklLicDst[{i}] {iss_string(dst)}"]
    (STAGE / "release.iss").write_text("\n".join(lines) + "\n", encoding="utf-8")

    # ---- dist -------------------------------------------------------------------------------------------
    if DIST.exists():
        shutil.rmtree(DIST)
    DIST.mkdir()

    zip_manifest = {
        "name": "TD_Custom_FFT",
        "version": version,
        "tag": tag,
        "commit": commit,
        "plugin_folder": "FFT",
        "plugin_files": plugin_files,
        "examples": examples,
        "min_vc_runtime": f"{vc_major}.{vc_minor}",
        "onemkl": onemkl,
    }
    script = (HERE / "install.ps1").read_text(encoding="utf-8")
    if "@@RELEASE_TAG@@" not in script or "@@ZIP_SHA256@@" not in script:
        fail("install.ps1 lost its @@RELEASE_TAG@@ / @@ZIP_SHA256@@ placeholders")
    # ASCII, no BOM: Windows PowerShell 5.1 reads a BOM-less script as the ANSI code page (so non-ASCII would be
    # mangled), and `irm | iex` can choke on a BOM.
    bad = sorted({c for c in script if ord(c) > 127})
    if bad:
        fail(f"install.ps1 must be pure ASCII; found {''.join(bad)!r}")
    in_zip_script = script.replace("@@RELEASE_TAG@@", tag)   # runs from the extracted zip: no hash needed

    zip_path = DIST / ZIP_NAME
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name in plugin_files:
            z.write(STAGE / "FFT" / name, f"FFT/{name}")
        for name in examples:
            z.write(STAGE / "Examples" / name, f"Examples/{name}")
        # Windows line endings for the files a user opens in Notepad or runs with cmd.exe.
        z.writestr("install.ps1", in_zip_script.replace("\r\n", "\n").replace("\n", "\r\n").encode("ascii"))
        z.writestr("Install.cmd", (HERE / "Install.cmd").read_text(encoding="utf-8").replace("\r\n", "\n").replace("\n", "\r\n"))
        z.writestr("README.txt", (HERE / "README-zip.txt").read_text(encoding="utf-8")
                   .replace("@@VERSION@@", version).replace("\r\n", "\n").replace("\n", "\r\n"))
        z.writestr("manifest.json", json.dumps(zip_manifest, indent=2) + "\n")
    zip_sha = sha256(zip_path)

    # The release asset: what `irm .../releases/latest/download/install.ps1 | iex` runs. It carries the zip's
    # hash, so the zip it downloads is the one built here.
    stamped = in_zip_script.replace("@@ZIP_SHA256@@", zip_sha)
    (DIST / "install.ps1").write_bytes(stamped.replace("\r\n", "\n").replace("\n", "\r\n").encode("ascii"))

    if not args.no_installer:
        iscc = find_iscc()
        if not iscc:
            fail("ISCC.exe (Inno Setup 6.5+) not found - install it (winget install JRSoftware.InnoSetup) "
                 "or pass --no-installer")
        cmd = [iscc, "/Q", f"/DStageDir={STAGE}", f"/DOutputDir={DIST}", f"/DRepoDir={REPO}",
               str(HERE / "TD_Custom_FFT.iss")]
        print("build_release:", " ".join(cmd))
        r = subprocess.run(cmd)
        if r.returncode != 0:
            fail(f"ISCC failed ({r.returncode})")

    if args.fftw_source:
        url, md5 = record["FFTW_VENDORED_SOURCE"], record["FFTW_VENDORED_SOURCE_MD5"]
        out = DIST / url.rsplit("/", 1)[-1]
        print(f"build_release: downloading {url}")
        with urllib.request.urlopen(url, timeout=120) as resp, out.open("wb") as f:
            shutil.copyfileobj(resp, f)
        if hashlib.md5(out.read_bytes()).hexdigest() != md5:
            fail(f"{out.name}: MD5 does not match FFTW_VENDORED_SOURCE_MD5 ({md5})")
        shutil.copy2(FFTW_DIR / "fftw-3.3.11-cmake-version.patch", DIST / "fftw-3.3.11-cmake-version.patch")

    assets = sorted(p for p in DIST.iterdir() if p.is_file() and p.name != "SHA256SUMS.txt")
    (DIST / "SHA256SUMS.txt").write_text("".join(f"{sha256(p)}  {p.name}\n" for p in assets), encoding="utf-8")

    print(f"build_release: {tag} (commit {commit or '?'}), VC++ runtime >= {vc_major}.{vc_minor}, "
          f"oneMKL pin {onemkl['package_version']}")
    for p in assets + [DIST / "SHA256SUMS.txt"]:
        print(f"  {p.stat().st_size:>12,}  {p.name}")


if __name__ == "__main__":
    main()
