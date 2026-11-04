# Fetches NVIDIA's public Streamline SDK headers (MIT, NVIDIA-RTX/Streamline include/) into third_party/streamline (git-ignored).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$p = Join-Path $root 'third_party\streamline'
if (Test-Path $p) { Write-Host 'streamline already present'; exit 0 }
$ref = if ($args.Count -gt 0) { $args[0] } else { 'v2.12.0' }
git clone --depth 1 --branch $ref --filter=blob:none --sparse 'https://github.com/NVIDIA-RTX/Streamline.git' $p
git -C $p sparse-checkout set include /license.txt /LICENSE.txt
