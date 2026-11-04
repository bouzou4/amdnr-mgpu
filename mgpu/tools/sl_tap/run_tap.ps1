# Starts a game with sl_tap loaded (early APC), drops capture triggers, then kills it. Sends no input.
# Restores the game's settings file and per-app GPU preference if they changed.
param(
  [Parameter(Mandatory)][string]$Exe,
  [Parameter(Mandatory)][string]$Tag,
  [string]$AppId = '',
  [string]$SettingsFile = '',
  [int]$RunSec = 60,
  [int]$CaptureAfter = 600,
  [string]$GpuPref = '',
  [int[]]$Triggers = @(),
  [int]$ExpectCaps = 1
)

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$tools = Resolve-Path (Join-Path $PSScriptRoot '..')
$b = "$root\data\sl_tap\bin"; $out = "$root\data\sl_tap\$Tag"
$name = [IO.Path]::GetFileNameWithoutExtension($Exe)
$k = 'HKCU:\Software\Microsoft\DirectX\UserGpuPreferences'
New-Item -ItemType Directory -Force $b, $out | Out-Null
Copy-Item "$tools\build\sl_tap\sl_tap.dll", "$tools\build\sl_tap\sl_tap_launch.exe" $b -Force
Set-Content "$b\sl_tap.ini" "out=$out`ncapture_after=$CaptureAfter"
if ($SettingsFile) { Copy-Item $SettingsFile "$out\settings.before" }
$orig = (Get-ItemProperty $k -Name $Exe -ErrorAction SilentlyContinue).$Exe
if ($GpuPref) { Set-ItemProperty $k -Name $Exe -Value $GpuPref }
$env_args = if ($AppId) { @('--env', "SteamAppId=$AppId", '--env', "SteamGameId=$AppId") } else { @() }
try {
  & "$b\sl_tap_launch.exe" "$b\sl_tap.dll" $Exe --fallback-sec 20 @env_args | Tee-Object "$out\launch.txt"
  $t0 = Get-Date
  $sent = @{}
  while (((Get-Date) - $t0).TotalSeconds -lt $RunSec) {
    Start-Sleep 1
    $el = ((Get-Date) - $t0).TotalSeconds
    foreach ($tt in $Triggers) { if ($el -ge $tt -and -not $sent["t$tt"]) { $sent["t$tt"] = 1; New-Item "$out\capture.trigger" -Force | Out-Null; "trigger at $([int]$el) s" } }
    if (@(Get-ChildItem $out -Recurse -Filter capture.json -ErrorAction SilentlyContinue).Count -ge $ExpectCaps) { Start-Sleep 2; break }
    if (-not (Get-Process $name -ErrorAction SilentlyContinue)) { 'game exited'; break }
  }
  $p = Get-Process $name -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($p) {
    $c = Get-Counter "\GPU Engine(pid_$($p.Id)_*)\Utilization Percentage" -SampleInterval 1 -MaxSamples 2 -ErrorAction SilentlyContinue
    $c.CounterSamples | Where-Object CookedValue -gt 0.5 | Group-Object { ($_.InstanceName -replace '^pid_\d+_luid_(0x[0-9a-f]+_0x[0-9a-f]+)_phys_\d+_eng_\d+_engtype_(.*)$', '$1 $2') } |
      ForEach-Object { "{0}  avg {1:N1}%" -f $_.Name, (($_.Group | Measure-Object CookedValue -Average).Average) } | Tee-Object -Append "$out\gpu.txt"
  }
} finally {
  Get-Process $name -ErrorAction SilentlyContinue | ForEach-Object { taskkill /T /F /PID $_.Id | Out-Null }
  Start-Sleep 3
  if ($GpuPref) { if ($null -ne $orig) { Set-ItemProperty $k -Name $Exe -Value $orig } else { Remove-ItemProperty $k -Name $Exe } }
  if ($SettingsFile -and (Get-FileHash $SettingsFile).Hash -ne (Get-FileHash "$out\settings.before").Hash) {
    Copy-Item $SettingsFile "$out\settings.after"; Copy-Item "$out\settings.before" $SettingsFile -Force; 'settings file changed by the game; restored'
  }
}
