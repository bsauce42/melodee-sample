#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Build the stem separator Melodee runs beside itself (src/melodee/shell/stems.h):

  melodee-stems     SpleeterRT's offline program (GPL-3.0), unchanged, with Deezer's
                    Spleeter model inside it (MIT), entered through
                    tools/stems/stems_main.c and linked with a static OpenBLAS
                    (BSD-3-Clause) built for the MPC's Cortex-A55 cores
  licenses/         their licence texts and where their sources are (the
                    release zip carries them)

Run by the Makefile (`make stems`) inside Melodee's build image:
    python3 tools/stems/build.py build/stems
The sources come from build/deps/spleeterrt and build/deps/openblas, which
src/melodee/dependencies.py fetches at pinned commits; model.c is unpacked
there from SpleeterRT's model.7z.

The program is a file of its own in the release (autorun/melodee/stems/), not
part of Melodee's binary: 40 MB that would sit in RAM the whole session.
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SPLEETER = ROOT / "build/deps/spleeterrt"
OPENBLAS = ROOT / "build/deps/openblas"
SOURCES = ["main.c", "spleeter.c", "gemm.c", "im2col_dilated.c", "codelet.c", "stftFix.c", "cpthread.c",
           "libsamplerate/samplerate.c", "libsamplerate/src_sinc.c"]
# One core's kernels, C BLAS alone (no LAPACK, so no Fortran), a static library. Threaded as the program expects:
# it asks for one OpenBLAS thread and calls sgemm from its own.
OPENBLAS_FLAGS = ["TARGET=CORTEXA55", "ONLY_CBLAS=1", "NO_LAPACK=1", "NO_FORTRAN=1", "NO_SHARED=1", "NUM_THREADS=4"]
CFLAGS = ["-O2", "-mcpu=cortex-a55", "-w"] # -O3 measured no faster on the MPC

MIT = """Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
"""


def revision(path):
    """The commit src/melodee/dependencies.py fetched and checked (git itself refuses another user's checkout)."""
    marker = path / "MELODEE_REVISION"
    if not marker.is_file():
        raise SystemExit(f"no {marker.relative_to(ROOT)}: run python3 tools/dependencies.py")
    return marker.read_text().strip()


def openblas(out):
    """The static library and its headers, into out/openblas."""
    prefix = out / "openblas"
    if (prefix / "lib/libopenblas.a").is_file() and (prefix / "revision").is_file() \
            and (prefix / "revision").read_text().strip() == revision(OPENBLAS):
        return prefix
    jobs = f"-j{os.cpu_count() or 2}"
    subprocess.run(["make", "-C", str(OPENBLAS), jobs, *OPENBLAS_FLAGS, "libs"], check=True)
    if prefix.exists():
        shutil.rmtree(prefix)
    subprocess.run(["make", "-C", str(OPENBLAS), *OPENBLAS_FLAGS, f"PREFIX={prefix}", "install"], check=True)
    (prefix / "revision").write_text(revision(OPENBLAS) + "\n")
    return prefix


def program(out, blas):
    source = SPLEETER / "Executable"
    if not (source / "model.c").is_file():
        raise SystemExit("no build/deps/spleeterrt/Executable/model.c: run python3 tools/dependencies.py")
    objects = out / "obj"
    objects.mkdir(parents=True, exist_ok=True)
    # gemm.c includes Intel's <mkl.h> on anything but Android and macOS; this one hands it OpenBLAS's cblas instead.
    shim = out / "include"
    shim.mkdir(parents=True, exist_ok=True)
    (shim / "mkl.h").write_text("#include <cblas.h>\n")
    include = [f"-I{shim}", f"-I{blas / 'include'}"]
    made = []
    for name in SOURCES:
        target = objects / (name.replace("/", "_")[:-2] + ".o")
        # SpleeterRT's main becomes spleeter_main: tools/stems/stems_main.c is the entry.
        rename = ["-Dmain=spleeter_main"] if name == "main.c" else []
        # Each time: an object kept from an earlier run may have been made with other flags.
        subprocess.run(["gcc", *CFLAGS, *rename, *include, "-c", str(source / name), "-o", str(target)], check=True)
        made.append(str(target))
    entry = objects / "stems_main.o"
    subprocess.run(["gcc", "-O2", "-Wall", "-Wextra", "-Werror", "-c", str(ROOT / "tools/stems/stems_main.c"), "-o", str(entry)], check=True)
    binary = out / "melodee-stems"
    subprocess.run(["gcc", "-o", str(binary), str(entry), *made, str(blas / "lib/libopenblas.a"), "-lm", "-lpthread"], check=True)
    subprocess.run(["strip", str(binary)], check=True)
    print(f"{binary}: {binary.stat().st_size // 1024} KiB")


def licenses(out):
    folder = out / "licenses"
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    shutil.copyfile(SPLEETER / "LICENSE", folder / "SpleeterRT-GPL-3.0.txt")
    shutil.copyfile(OPENBLAS / "LICENSE", folder / "OpenBLAS-LICENSE.txt")
    (folder / "Spleeter-model-MIT.txt").write_text("MIT License\n\nCopyright (c) 2019-present, Deezer SA.\n\n" + MIT)
    (folder / "README.txt").write_text(
        "melodee-stems, the program Melodee's STEMS runs, is made of:\n\n"
        "SpleeterRT's offline program, by James Fung, under the GPL 3.0 (SpleeterRT-GPL-3.0.txt), unchanged:\n"
        f"  https://github.com/james34602/SpleeterRT commit {revision(SPLEETER)}\n"
        "  It carries dr_wav, dr_flac and dr_mp3 (David Reid, public domain or MIT No Attribution) and\n"
        "  libsamplerate (Copyright (c) 2002-2016 Erik de Castro Lopo, BSD 2-clause).\n"
        "The Spleeter model it holds, trained by Deezer, under the MIT licence (Spleeter-model-MIT.txt):\n"
        "  https://github.com/deezer/spleeter\n"
        "OpenBLAS, under the BSD 3-clause licence (OpenBLAS-LICENSE.txt), linked in:\n"
        f"  https://github.com/OpenMathLib/OpenBLAS commit {revision(OPENBLAS)}\n"
        "Its entry, tools/stems/stems_main.c, and the script that builds it, tools/stems/build.py, are in\n"
        "Melodee's source under the AGPL 3.0. The source of all of it is at those addresses, and is also\n"
        "available on request from the Melodee project for three years from each release.\n")


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "build/stems").resolve()
    for path in (SPLEETER, OPENBLAS):
        revision(path)
    out.mkdir(parents=True, exist_ok=True)
    program(out, openblas(out))
    licenses(out)


if __name__ == "__main__":
    main()
