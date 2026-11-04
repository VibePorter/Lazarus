# Runs me2.nro in Ryujinx for $Seconds (or until it exits), screenshots the window, stops it, prints both logs' tails.
param([int]$Seconds = 60, [string]$Shot = "C:\ME2_Recomp\nx_out\ryu_shot.png")
Stop-Process -Name Ryujinx -Force -ErrorAction SilentlyContinue; Start-Sleep 2
$p = Start-Process C:\ME2_Recomp\Ryujinx\Ryujinx.exe -ArgumentList "`"$env:APPDATA\Ryujinx\sdcard\switch\me2-nx\me2.nro`"" -PassThru
for ($i = 0; $i -lt $Seconds -and -not $p.HasExited; $i++) { Start-Sleep 1 }
if (-not $p.HasExited) {
  & C:\ME2_Recomp\me2recomp\tools\screenshot.ps1 -ProcessId $p.Id -Out $Shot
  Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
} else { "Ryujinx exited after $i s" }
$r = Get-ChildItem C:\ME2_Recomp\Ryujinx\Logs | Sort-Object LastWriteTime -Descending | Select-Object -First 1
"== Ryujinx $($r.Name)"
Get-Content $r.FullName | Select-String '\|E\||Unhandled|InvalidAccess|PrintGuestStackTrace' | Select-Object -Last 8 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(220, $_.Line.Length)) }
$f = Get-ChildItem "$env:APPDATA\Ryujinx\sdcard\switch\me2-nx\logs" -Filter *.log | Sort-Object LastWriteTime -Descending | Select-Object -First 1
"== game $($f.Name): $((Get-Content $f.FullName).Count) lines"
Get-Content $f.FullName -Tail 8 | ForEach-Object { $_.Substring(0, [Math]::Min(200, $_.Length)) }
