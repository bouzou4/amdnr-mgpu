# Downloads the official PresentMon console release into third_party/presentmon (git-ignored).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dst = Join-Path $root 'third_party\presentmon'
$tag = if ($args.Count -gt 0) { $args[0] } else { 'v2.6.0' }
New-Item -ItemType Directory -Force $dst | Out-Null
gh release download $tag -R GameTechDev/PresentMon -p "PresentMon-$($tag.TrimStart('v'))-x64.exe" -D $dst --skip-existing
