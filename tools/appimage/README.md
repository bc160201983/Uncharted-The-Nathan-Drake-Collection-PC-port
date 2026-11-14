# Linux AppImage

`build_appimage.sh` packs a native Linux build (no Wine/Proton) into one
`Uncharted-NDC-<version>-x86_64.AppImage`: the runtime, the launcher with its own Python 3.12 + Tk +
Pillow, and the few libraries a desktop may lack (SDL3, fmt, Zydis, xxHash). The Vulkan loader,
GPU drivers and glibc come from the system.

Tested on Bazzite (Fedora-based) with a Ryzen 7800X3D and an RTX 5070 Ti: Among Thieves plays at
the right speed; Drake's Deception boots and reaches the menus. Linux has had less testing than
Windows; reports welcome.

## What the AppImage does at start

The AppImage is read-only, but the launcher and runtime write next to themselves (`analysis/`).
`AppRun` copies the program files once per version to `~/.local/share/uncharted-ndc/app`
(`UNCHARTED_HOME` changes the folder) and starts from there; saves, settings, logs and the shader
cache under `analysis/` are never overwritten. The launcher's self-update is off in the AppImage
(the releases hold the Windows package): a new AppImage is the update.

- `./Uncharted-NDC-…AppImage` opens the launcher.
- `./Uncharted-NDC-…AppImage --boot <game folder>` starts a game without it.

## Building (Ubuntu 24.04 was used)

1. The Linux branch of `gow3_pc/build.sh` needs, beyond its package list: GCC 14 (`deducing this`),
   current Vulkan-Headers and VulkanMemoryAllocator (newer than Ubuntu's), and SDL3 built from
   source.
2. FFmpeg is best linked statically with only the decoders the games use, so the AppImage does
   not drag in a distribution's whole FFmpeg dependency tree:
   ```
   ./configure --prefix=/opt/ffmin --enable-pic --disable-shared --enable-static --disable-programs \
     --disable-doc --disable-everything --enable-decoder=h264,hevc,mpeg4,aac,mp3 \
     --enable-parser=h264,hevc,aac --enable-demuxer=h264,mov --enable-protocol=file \
     --enable-swscale --enable-swresample --disable-avdevice --disable-avfilter \
     --disable-network --disable-autodetect
   ```
   and link `libgow3gpu.so` and `gow3-probe` against those `.a` files (`-Wl,--exclude-libs,ALL`
   for the shared library).
3. Then:
   ```
   APPIMAGETOOL=appimagetool \
   PYTHON_STANDALONE=~/.local/share/uv/python/cpython-3.12.*-linux-x86_64-gnu \
   tools/appimage/build_appimage.sh
   ```
   (`uv python install 3.12` provides a relocatable Python with Tk.)
