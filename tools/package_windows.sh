#!/usr/bin/env bash
# Builds dist/Uncharted-windows/ and dist/Uncharted-windows.zip: a folder that runs on any Windows
# 10/11 x64 PC with a Vulkan GPU, without Python or MSYS2 installed.
#   Uncharted Launcher.exe   starter (tools/windows_starter) -> python\pythonw.exe launcher\...
#   python\                  Windows Python with Tkinter and Pillow (tools/copy_python.py)
#   launcher\ tools\         the launcher and the start script (tools/probe_uncharted.py)
#   gow3_pc\scripts\         the preparation scripts; gow3_pc\out\ the runtime and its DLLs
# No game files ship: each player selects their own copy of the game in the launcher.
# Run after building the runtime (gow3_pc/build.sh), from Git Bash or an MSYS2 CLANG64 shell.
# PYTHON_HOME is a Windows Python 3.10+ with Pillow (default: the `python` on PATH).
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
# The Windows Python (looked up before the CLANG64 tools go first on PATH: MSYS2 has its own).
python=${PYTHON:-python}
python_home=${PYTHON_HOME:-$("$python" -c 'import sys, os; print(os.path.dirname(sys.executable))')}
msys=${MSYS2_ROOT:-/c/msys64}
# Inside the MSYS2 shell (GitHub Actions) its root is "/": without the slash, "$msys/usr" stays a
# local path ("//usr/..." is a network share, each try timed out after ~11 s).
msys=${msys%/}
[[ -d $msys/clang64/bin ]] && export PATH="$msys/clang64/bin:$PATH"
[[ -f gow3_pc/out/gow3-probe.exe ]] || { echo 'Build the runtime first: gow3_pc/build.sh' >&2; exit 1; }

stage=dist/stage/Uncharted-windows
rm -rf -- dist/stage
mkdir -p "$stage/gow3_pc/out" "$stage/gow3_pc/scripts" "$stage/tools" "$stage/launcher/assets" \
    "$stage/analysis/uncharted-user"

# The runtime without debug information, and every DLL it loads from the CLANG64 tree.
llvm-strip --strip-debug -o "$stage/gow3_pc/out/gow3-probe.exe" gow3_pc/out/gow3-probe.exe
ldd gow3_pc/out/gow3-probe.exe | awk '/clang64/ {print $3}' | sort -u | while read -r dll; do
    cp "$dll" "$stage/gow3_pc/out/"
done

# HDR tone replacement, its source, exact include snapshot and MIT license.
# The checked-in SPIR-V avoids making DXC a dependency of the runtime build.
cp -R gow3_pc/hdr "$stage/gow3_pc/out/hdr"
cp gow3_pc/scripts/*.py "$stage/gow3_pc/scripts/"
cp tools/probe_uncharted.py tools/hang_report.py tools/apply_update.py tools/install_update.py "$stage/tools/"
# PkgTool (LibOrbisPkg, LGPL-3.0): Advanced -> Install game update unpacks the player's own update PKG.
mkdir -p "$stage/tools/pkgtool"
cp tools/pkgtool/PkgTool.exe tools/pkgtool/LibOrbisPkg.dll "$stage/tools/pkgtool/"
cp launcher/uncharted_launcher.pyw launcher/updater.py "$stage/launcher/"
# The package's version (tools/VERSION; the launcher's updates compare it with the releases).
cp tools/VERSION "$stage/VERSION"
cp launcher/assets/uncharted.ico launcher/assets/icon.png "$stage/launcher/assets/"
# The runtime's optimizations known to break Uncharted (GOW3_TOGGLE_FILE).
cp tools/defaults/disabled-optimizations.txt "$stage/analysis/uncharted-user/"
cp tools/README-Windows.txt "$stage/README.txt"
cp gow3_pc/LICENSE "$stage/LICENSE.txt"
cp tools/github/CHANGELOG.md "$stage/CHANGELOG.md"

# Licenses of the sources built into the runtime, and of every MSYS2 package whose DLL ships: its
# license files and, in THIRD-PARTY.txt, its version, license and source (some packages, FFmpeg
# among them, carry no license file).
licenses=$stage/licenses
mkdir -p "$licenses"
mkdir -p "$licenses/renodx"
cp gow3_pc/hdr/renodx/LICENSE gow3_pc/hdr/renodx/SOURCE.txt "$licenses/renodx/"
for dir in gow3_pc/gpu/third_party/* gow3_pc/third_party/*; do
    for file in "$dir"/LICENSE* "$dir"/COPYING* "$dir"/COPYRIGHT*; do
        [[ -f $file ]] || continue
        mkdir -p "$licenses/$(basename -- "$dir")"
        cp "$file" "$licenses/$(basename -- "$dir")/"
    done
done
head -n 24 gow3_pc/gpu/third_party/gcn/include/gcn/si_ci_vi_merged_offset.h > "$licenses/gcn-headers.txt"
mkdir -p "$licenses/liborbispkg"
cp tools/pkgtool/LICENSE.txt "$licenses/liborbispkg/LICENSE.txt"
echo "PkgTool and LibOrbisPkg 0.2.231 by Maxton, unchanged; source: https://github.com/maxton/LibOrbisPkg" \
    > "$licenses/liborbispkg/SOURCE.txt"
pacman=$(command -v pacman || echo "$msys/usr/bin/pacman")
# Three pacman calls in all (one per DLL took minutes on the CI runner).
dlls=()
for dll in "$stage"/gow3_pc/out/*.dll; do
    dlls+=("$(cygpath -m "$msys/clang64/bin/$(basename -- "$dll")")")
done
mapfile -t packages < <({ "$pacman" -Qqo "${dlls[@]}" 2>/dev/null || true; } | sort -u)
{ "$pacman" -Qlq "${packages[@]}" | grep '/share/licenses/.*[^/]$' || true; } | while read -r file; do
    name=${file#*/share/licenses/}
    mkdir -p "$licenses/msys2/$(dirname -- "$name")"
    cp "$msys$file" "$licenses/msys2/$name"
done
{
    echo "Third-party packages whose DLLs ship in gow3_pc\\out (MSYS2 CLANG64 builds)."
    echo "Their sources: https://repo.msys2.org/mingw/sources/ and https://packages.msys2.org/"
    "$pacman" -Qi "${packages[@]}" | awk '/^Name /{print ""} /^(Name|Version|Licenses|URL) /{print}'
} > "$licenses/THIRD-PARTY.txt"

# DLAA and FSR 4 Native AA (tools/fetch_aa_files.sh), when fetched: NVIDIA's DLSS DLL and the
# bridge under their own licenses, and the FSR 4 native model. Without them the menus leave the
# two out.
if [[ -f gow3_pc/out/gow3_dlss.dll && -f gow3_pc/out/nvngx_dlss.dll ]]; then
    cp gow3_pc/out/gow3_dlss.dll gow3_pc/out/nvngx_dlss.dll "$stage/gow3_pc/out/"
    mkdir -p "$licenses/nvidia-dlss" "$licenses/gow3-dlss-bridge"
    dlss_license=${NVIDIA_DLSS_LICENSE:-}
    if [[ -z $dlss_license ]]; then
        for file in downloads/dlss-sdk-*/LICENSE.txt; do
            [[ -f $file ]] && { dlss_license=$file; break; }
        done
    fi
    [[ -f $dlss_license ]] || { echo 'Need the DLSS license (NVIDIA_DLSS_LICENSE).' >&2; exit 1; }
    cp "$dlss_license" "$licenses/nvidia-dlss/LICENSE.txt"
    cp gow3_pc/gpu/dlss_bridge/LICENSE.txt "$licenses/gow3-dlss-bridge/LICENSE.txt"
fi
if [[ -f gow3_pc/fsr4_shaders/fsr4_model_v07_i8_native_1080_pre.spv ]]; then
    mkdir -p "$stage/gow3_pc/fsr4_shaders" "$licenses/fsr4"
    cp gow3_pc/fsr4_shaders/LICENSE-FSR4-v07.txt gow3_pc/fsr4_shaders/*.spv gow3_pc/fsr4_shaders/*.bin \
        gow3_pc/fsr4_shaders/*.json "$stage/gow3_pc/fsr4_shaders/"
    cp gow3_pc/fsr4_shaders/LICENSE-FSR4-v07.txt "$licenses/fsr4/"
fi

"$python_home/python.exe" tools/copy_python.py "$python_home" "$stage/python"
# Python's own license is python\LICENSE.txt; Pillow's goes with the others.
for file in "$python_home"/Lib/site-packages/pillow-*.dist-info/licenses/* \
    "$python_home"/Lib/site-packages/pillow-*.dist-info/LICENSE*; do
    [[ -f $file ]] && { mkdir -p "$licenses/pillow"; cp "$file" "$licenses/pillow/"; }
done

windres tools/windows_starter/starter.rc -O coff -o dist/stage/starter.res.o
clang -O2 -municode -mwindows tools/windows_starter/starter.c dist/stage/starter.res.o \
    -o "$stage/Uncharted Launcher.exe"

find "$stage" -name __pycache__ -prune -exec rm -rf {} +
# Nothing from the game or a player (game executables, saves, shader caches, logs) may ship.
forbidden=$(find "$stage" \( -iname eboot.bin -o -iname '*.elf' -o -iname sce_sys -o -iname savedata \
    -o -iname cache -o -iname '*.log' -o -iname 'probe*.ini' -o -iname '*.pkg' -o -iname '*.psarc' \
    -o -iname '*.pak' -o -iname 'boot*.bin' \) -print)
if [[ -n $forbidden ]]; then
    echo "Package holds files that must not ship:" >&2
    echo "$forbidden" >&2
    exit 1
fi

rm -f dist/Uncharted-windows.zip
"$python_home/python.exe" -m zipfile -c dist/Uncharted-windows.zip "$stage"
rm -rf dist/Uncharted-windows
mv "$stage" dist/Uncharted-windows
du -sh dist/Uncharted-windows dist/Uncharted-windows.zip
