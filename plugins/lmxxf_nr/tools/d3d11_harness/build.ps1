# Builds d3d11_harness.exe (VS 2022 x64 tools) into x64\d3d11_harness\ at the repository root.
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$out = Join-Path $PSScriptRoot '..\..\..\..\x64\d3d11_harness'
New-Item -ItemType Directory -Force $out | Out-Null
$src = Join-Path $PSScriptRoot 'main.cpp'
$ngx = Join-Path $PSScriptRoot '..\..\..\..\src\external\nvngx_dlss_sdk'
& cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && cl /nologo /O2 /EHsc /std:c++20 /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /I`"$ngx`" `"$src`" /Fo`"$out\main.obj`" /Fe`"$out\d3d11_harness.exe`""
exit $LASTEXITCODE
