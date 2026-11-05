# Uncharted Collection HDR tone replacement

Experimental HDR scene path for the Vulkan port. All three games use tone hash
`3371ae8d` and post-tone hash `484cdbf5`. The HDR output setting must be enabled.
DLSS/DLAA, FSR 3/4, TAA and the stretch with upscaling off receive the HDR scene.
At native resolution the launcher enables the same composition path at 1:1, and the
renderer drops it again when the display offers no HDR10. `UNCHARTED_HDR_NATIVE=0` opts out.

Nothing of this is built with HDR off: the float-target pipelines, the tone pass
shadow and binding 12 exist only when HDR is on at start on an HDR10 display
(`hdr_pipelines`), so turning HDR on in the game applies to the scene after a restart.
The replacement matches the tone pass as translated for NVIDIA's driver (manual
interpolation through fragment shader barycentrics); on other GPUs, and on NVIDIA
GPUs without barycentrics (GTX 10xx), the scene stays SDR inside HDR10 output.

`fs_3371ae8d_hdr.spv` is the checked-in runtime asset. `gow3_pc/build.sh` copies it
next to the Windows executable under `out/hdr/`. The Windows package carries only that
`.spv`, and the RenoDX license under `licenses/renodx/`. `GOW3_HDR_SHADER_DIR` overrides
the runtime asset directory. Shader files are read once per process.

The replacement keeps guest descriptor bindings 0-11, push constants and stage I/O.
The current tone pipeline has 12 native descriptors (the replacement uses 0-9).
Binding 12 is a read-only byte-address buffer containing two floats: peak nits at
byte 0 and game-white nits at byte 4. The port uploads current slider values for
each tone draw, so no restart or pipeline rebuild is required for brightness.
Do not mix the old specialization-constant shader with this runtime.

Rebuild from PowerShell with an existing DirectX Shader Compiler:

```powershell
./gow3_pc/hdr/build.ps1 -Dxc C:/path/to/dxc.exe
```

The `renodx/` directory is the local quoted-include dependency closure of
`draw.hlsl`, unchanged, from the commit recorded in `renodx/SOURCE.txt`.
It carries the original MIT license. No external checkout or download is needed
to rebuild the shader. The decompiled game tone pass and anchored extension are
in `fs_3371ae8d_hdr.hlsl` (its header says what it is derived from); the entry point
is `main_hdr_shadow`.

Diagnostic staging: `GOW3_HDR_STAGE=1` draws the tone shadow; `2` adds the scene
shadow; `3` (default) also sends it through the selected upscaler or stretch.
`GOW3_HDR_TRACE=1` logs composition formats. U1/U3 gameplay, movies and display
changes still need validation; title-screen captures alone do not cover them.
