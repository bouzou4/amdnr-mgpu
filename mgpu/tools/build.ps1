# Configures (once) and builds with -j = logical threads - 2.
param([string]$Config = 'RelWithDebInfo', [string]$BuildDir = 'build')
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
if (-not $env:VCINSTALLDIR) {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
  cmd /c "`"$vcvars`" >nul && set" | ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] } }
}
$jobs = [Math]::Max(1, [Environment]::ProcessorCount - 2)
if (-not (Test-Path (Join-Path $BuildDir 'build.ninja'))) {
  cmake -S . -B $BuildDir -G Ninja "-DCMAKE_BUILD_TYPE=$Config" -DCMAKE_CXX_COMPILER=cl
  if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
cmake --build $BuildDir -j $jobs
exit $LASTEXITCODE
