# Uncharted 2 HDR tone replacement

Experimental HDR scene path for the Vulkan port. Only U2 tone hash `3371ae8d` is covered.
The HDR output setting must be enabled. DLSS receives the HDR scene; other upscalers
currently retain SDR scene values inside HDR10 output.

`fs_3371ae8d_hdr.spv` is the checked-in runtime asset. `gow3_pc/build.sh` copies it
next to the Windows executable under `out/hdr/`. The Windows package carries this
folder, including source and license information. `GOW3_HDR_SHADER_DIR` overrides
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
to rebuild the shader. The translated U2 tone pass and anchored extension are
in `fs_3371ae8d_hdr.hlsl`; the entry point is `main_hdr_shadow`.

Diagnostic staging: `GOW3_HDR_STAGE=1` draws the tone shadow; `2` adds the scene
shadow; `3` (default) also sends it through HDR DLSS. `GOW3_HDR_TRACE=1` logs
composition formats. Visual approval, longer stability and resize tests remain.
