# Builds upsample_bench.exe (VS 2022 x64 tools) into x64\upsample_bench\ at the repository root.
# -FfxInclude: ffx-api\include of a FidelityFX-SDK checkout (default: this repository's submodule).
param([string]$FfxInclude = (Join-Path $PSScriptRoot '..\..\..\..\src\external\FidelityFX-SDK\ffx-api\include'))
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$out = Join-Path $PSScriptRoot '..\..\..\..\x64\upsample_bench'
New-Item -ItemType Directory -Force $out | Out-Null
$src = Join-Path $PSScriptRoot 'main.cpp'
& cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && cl /nologo /O2 /EHsc /std:c++20 /W3 /utf-8 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I`"$FfxInclude`" `"$src`" /Fo`"$out\\`" /Fe`"$out\upsample_bench.exe`""
exit $LASTEXITCODE
