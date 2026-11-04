# Fetches AMD's public FidelityFX API headers (MIT, GPUOpen FidelityFX-SDK Kits/FidelityFX) into third_party/ffx (git-ignored).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$p = Join-Path $root 'third_party\ffx'
if (Test-Path $p) { Write-Host 'ffx already present'; exit 0 }
$ref = if ($args.Count -gt 0) { $args[0] } else { 'main' }
git clone --depth 1 --branch $ref --filter=blob:none --sparse 'https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git' $p
git -C $p sparse-checkout set Kits/FidelityFX/api/include Kits/FidelityFX/framegeneration/include Kits/FidelityFX/upscalers/include /LICENSE.txt /license.txt
