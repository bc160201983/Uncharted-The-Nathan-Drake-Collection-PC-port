#!/usr/bin/env bash
# Builds Uncharted-x86_64.AppImage from a Linux build (gow3_pc/build.sh).
# Needs: appimagetool (APPIMAGETOOL), a relocatable Python 3.12 with Tk (PYTHON_STANDALONE,
# e.g. `uv python install 3.12`), Pillow installable with pip.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
ROOT=$PWD
APPIMAGETOOL=${APPIMAGETOOL:-appimagetool}
PYSRC=${PYTHON_STANDALONE:?set PYTHON_STANDALONE to a python-build-standalone folder}
APPDIR=$ROOT/dist/Uncharted.AppDir
rm -rf "$APPDIR"
APP=$APPDIR/usr/share/uncharted/app
mkdir -p "$APP"/{gow3_pc/out/gpu,gow3_pc/scripts,tools,launcher/assets,analysis/uncharted-user} "$APPDIR/usr/lib"

# Program files (as tools/package_windows.sh, without the Windows-only parts).
cp gow3_pc/out/gow3-probe gow3_pc/out/gow3-gpu-capabilities "$APP/gow3_pc/out/"
cp gow3_pc/out/gpu/libgow3gpu.so "$APP/gow3_pc/out/gpu/"
strip --strip-debug "$APP/gow3_pc/out/gow3-probe" "$APP/gow3_pc/out/gpu/libgow3gpu.so"
cp gow3_pc/scripts/*.py "$APP/gow3_pc/scripts/"
cp tools/probe_uncharted.py tools/hang_report.py tools/apply_update.py tools/install_update.py \
   tools/net_overlay.py tools/check_game_files.py tools/game_files_102.json.gz "$APP/tools/"
cp launcher/*.py launcher/*.pyw launcher/*.json "$APP/launcher/"
cp launcher/assets/icon.png "$APP/launcher/assets/" 2>/dev/null || true
cp -r launcher/assets/. "$APP/launcher/assets/"
cp tools/VERSION "$APP/VERSION"
cp tools/defaults/disabled-optimizations.txt "$APP/analysis/uncharted-user/"
cp gow3_pc/LICENSE "$APP/LICENSE.txt"
[[ -d gow3_pc/fsr4_shaders ]] && cp -r gow3_pc/fsr4_shaders "$APP/gow3_pc/"

# Shared libraries the runtime needs that a desktop may not have (SDL3, FFmpeg, ...).
# The Vulkan loader, GPU drivers, glibc, X11/Wayland and audio servers come from the system.
exclude='^(libgow3gpu|linux-vdso|ld-linux|libc\.|libm\.|libdl\.|libpthread|librt\.|libgcc_s|libstdc\+\+|libvulkan|libX|libxcb|libwayland|libGL|libEGL|libdrm|libgbm|libasound|libpulse|libpipewire|libdbus|libudev|libsystemd|libz\.|libexpat|libffi|libuuid|libresolv|libselinux|libcap|libmount|libblkid)'
copy_deps() {
    ldd "$1" | awk '/=> \//{print $1, $3}' | while read -r name path; do
        [[ $name =~ $exclude ]] && continue
        [[ -e $APPDIR/usr/lib/$name ]] && continue
        cp -L "$path" "$APPDIR/usr/lib/$name"
        copy_deps "$APPDIR/usr/lib/$name"
    done
}
LD_LIBRARY_PATH=$ROOT/gow3_pc/out/gpu:/usr/local/lib copy_deps "$APP/gow3_pc/out/gow3-probe"
LD_LIBRARY_PATH=/usr/local/lib copy_deps "$APP/gow3_pc/out/gpu/libgow3gpu.so"

# Python with Tk (launcher) and Pillow.
cp -a "$PYSRC" "$APPDIR/usr/python"
rm -f "$APPDIR"/usr/python/lib/python3*/EXTERNALLY-MANAGED
"$APPDIR/usr/python/bin/python3" -m pip install --quiet --no-warn-script-location Pillow
rm -rf "$APPDIR/usr/python/lib/python3"*/test "$APPDIR/usr/python/lib/python3"*/idlelib \
       "$APPDIR/usr/python/include"
find "$APPDIR/usr/python" -name __pycache__ -prune -exec rm -rf {} +

cp tools/appimage/AppRun "$APPDIR/AppRun"; chmod +x "$APPDIR/AppRun"
cat > "$APPDIR/uncharted-ndc.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Uncharted: The Nathan Drake Collection
Comment=PC port launcher (needs your own CUSA02344 game dump)
Exec=AppRun
Icon=uncharted-ndc
Categories=Game;
Terminal=false
EOF
icon=launcher/assets/icon.png
python3 - "$icon" "$APPDIR/uncharted-ndc.png" <<'EOF' || cp "$icon" "$APPDIR/uncharted-ndc.png"
import sys
from PIL import Image
Image.open(sys.argv[1]).convert("RGBA").resize((256, 256)).save(sys.argv[2])
EOF
ln -sf uncharted-ndc.png "$APPDIR/.DirIcon"

mkdir -p dist
ARCH=x86_64 "$APPIMAGETOOL" ${APPIMAGE_RUNTIME:+--runtime-file "$APPIMAGE_RUNTIME"} -n "$APPDIR" \
    "dist/Uncharted-NDC-$(cat tools/VERSION)-x86_64.AppImage"
