# Installs the built OptiScaler.dll into a game folder as dxgi.dll, beside the lmxxf pair.
# Needs a game_backup.ps1 backup first. Keeps the folder's OptiScaler.ini and its OptiScaler\ libraries.
param(
    [Parameter(Mandatory)] [string]$GameDir,
    [Parameter(Mandatory)] [string]$BackupDir,
    [string]$Dll = (Join-Path $PSScriptRoot '..\..\src\x64\Release\a\OptiScaler.dll'),
    [string]$LmxxfDir
)
$ErrorActionPreference = 'Stop'
$GameDir = (Resolve-Path $GameDir).Path.TrimEnd('\')
if (-not (Test-Path (Join-Path $BackupDir 'manifest.csv'))) { throw 'no backup: run game_backup.ps1 first' }
$exe = Get-Process -ErrorAction SilentlyContinue | ? { $_.Path -and $_.Path.StartsWith($GameDir, 'OrdinalIgnoreCase') }
if ($exe) { throw "the game is running: $($exe.Name -join ', ')" }

Copy-Item -Force $Dll (Join-Path $GameDir 'dxgi.dll')
"dxgi.dll <- $((Resolve-Path $Dll).Path) ($((Get-FileHash -Algorithm SHA256 $Dll).Hash))"
foreach ($n in 'LmxxfNrRuntime.dll', 'LmxxfNrRuntime.pak') {
    $dst = Join-Path $GameDir $n
    if ($LmxxfDir) { Copy-Item -Force (Join-Path $LmxxfDir $n) $dst }
    if (-not (Test-Path $dst)) { throw "missing $n beside dxgi.dll" }
    "$n $((Get-FileHash -Algorithm SHA256 $dst).Hash)"
}
