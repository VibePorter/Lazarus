# Starts the smoke run and, after -At seconds, takes -Count non-invasive stack samples of every thread of sa2.exe with
# cdb (WinDbg's console debugger), -Gap seconds apart. Output: out\validation\<Name>\stacks_<n>.txt.
param([string]$Name = 'stacks', [int]$At = 45, [int]$Count = 3, [int]$Gap = 2, [int]$Seconds = 70,
      [string[]]$ExtraArguments = @())
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$report = Join-Path $root "out\validation\$Name"
$smoke = Join-Path $PSScriptRoot 'smoke_me2_pc.ps1'
$argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $smoke, '-Name', $Name, '-Seconds', $Seconds, '-VideoAt', ($At + 5))
if ($ExtraArguments) { $argList += '-ExtraArguments'; $argList += ($ExtraArguments -join ',') }
$runner = Start-Process powershell -ArgumentList $argList -PassThru -WindowStyle Hidden
Start-Sleep -Seconds $At
$proc = Get-Process sa2 -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) { throw 'sa2.exe is not running' }
$cdb = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps\cdbX64.exe'
$sym = Join-Path $root 'out\pc'
for ($i = 0; $i -lt $Count; $i++) {
    $out = Join-Path $report "stacks_$i.txt"
    & $cdb -pv -p $proc.Id -y $sym -c '.reload /f sa2.exe; ~*k 25; qd' *> $out
    Start-Sleep -Seconds $Gap
}
$runner.WaitForExit()
"done: $report"
