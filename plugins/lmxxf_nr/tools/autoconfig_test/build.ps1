# Builds and runs the offline autoconfig tests (VS 2022 x64 tools). Scratch files go under the repo's data\.
param([string]$Scratch = (Join-Path $PSScriptRoot '..\..\..\..\data\autoconfig_test'), [string]$Modules = '', [string[]]$Sweep = @())
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$env:PATH = (Split-Path $vswhere) + ";" + $env:PATH
Push-Location $PSScriptRoot
try {
    cmd /c "`"$vcvars`" >nul && cl /nologo /std:c++20 /EHsc /O2 /W3 /MP main.cpp ..\..\src\Autoconfig.cpp ..\..\src\Runtime.cpp /Fe:autoconfig_test.exe /link /nologo"
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    Remove-Item *.obj -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $Scratch | Out-Null
    $a = @($Scratch); if ($Modules) { $a += '--modules', $Modules }; foreach ($s in $Sweep) { $a += '--sweep', $s }
    & .\autoconfig_test.exe @a
    exit $LASTEXITCODE
} finally { Pop-Location }
