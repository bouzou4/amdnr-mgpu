# Fetches AMD's MIT-licensed HIP headers into third_party/ (git-ignored), for reference and layout checks.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dst = Join-Path $root 'third_party'
New-Item -ItemType Directory -Force $dst | Out-Null
$tag = if ($args.Count -gt 0) { $args[0] } else { 'rocm-7.0.0' }
foreach ($repo in @('hip', 'clr')) {
  $p = Join-Path $dst $repo
  if (Test-Path $p) { Write-Host "$repo already present"; continue }
  git clone --depth 1 --branch $tag --filter=blob:none --sparse --no-checkout "https://github.com/ROCm/$repo.git" $p
  if ($repo -eq 'hip') { git -C $p sparse-checkout set --no-cone /include/ /LICENSE.md /LICENSE.txt; git -C $p checkout }
  else { git -C $p sparse-checkout set --no-cone /hipamd/include/ /LICENSE.md /LICENSE.txt; git -C $p checkout }
}
