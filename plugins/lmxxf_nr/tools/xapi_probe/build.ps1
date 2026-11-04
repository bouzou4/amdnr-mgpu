# Builds xapi_probe.exe (VS 2022 x64 tools) into x64\xapi_probe\ at the repository root.
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$root = Join-Path $PSScriptRoot '..\..\..\..'
$out = Join-Path $root 'x64\xapi_probe'
New-Item -ItemType Directory -Force $out | Out-Null
# Vulkan headers: the vulkan submodule, or the main checkout's when this is a worktree without submodules.
$vk = Join-Path $root 'src\external\vulkan\include'
if (-not (Test-Path (Join-Path $vk 'vulkan\vulkan.h'))) {
  $common = (git -C $root rev-parse --git-common-dir)
  $vk = Join-Path (Split-Path $common -Parent) 'src\external\vulkan\include'
}
$srcs = @('main.cpp', 'common.cpp', 'probe_d3d.cpp', 'probe_vk.cpp') | ForEach-Object { "`"$(Join-Path $PSScriptRoot $_)`"" }
Push-Location $out
& cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && cl /nologo /O2 /EHsc /std:c++20 /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /I`"$vk`" $($srcs -join ' ') /Fe`"$out\xapi_probe.exe`" /link gdi32.lib user32.lib"
$rc = $LASTEXITCODE
Pop-Location
exit $rc
