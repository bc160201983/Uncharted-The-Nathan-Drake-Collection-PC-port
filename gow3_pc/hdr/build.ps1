# Rebuild the checked-in replacement with an existing DXC installation; no downloads.
param([Parameter(Mandatory)][string]$Dxc)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    & $Dxc -spirv '-fspv-target-env=vulkan1.3' -enable-16bit-types -T ps_6_6 `
        -E main_hdr_shadow '-fspv-entrypoint-name=main' -I renodx `
        fs_3371ae8d_hdr.hlsl -Fo fs_3371ae8d_hdr.spv
    if ($LASTEXITCODE -ne 0) { throw 'HDR shader compilation failed' }
} finally { Pop-Location }
