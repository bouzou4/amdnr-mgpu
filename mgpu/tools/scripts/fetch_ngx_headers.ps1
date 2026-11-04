# Fetches NVIDIA's public NGX headers (NVIDIA/DLSS include/) into third_party/ngx (git-ignored).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$p = Join-Path $root 'third_party\ngx'
if (Test-Path $p) { Write-Host 'ngx already present'; exit 0 }
$ref = if ($args.Count -gt 0) { $args[0] } else { 'main' }
git clone --depth 1 --branch $ref --filter=blob:none --sparse 'https://github.com/NVIDIA/DLSS.git' $p
git -C $p sparse-checkout set include /LICENSE.txt
