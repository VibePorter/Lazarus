# Runs me2.nro in Eden (no debugger) for $Seconds, screenshots the window, stops Eden, prints the log tail.
param([int]$Seconds = 60, [string]$Shot = "C:\ME2_Recomp\nx_out\eden_shot.png")
Stop-Process -Name eden -Force -ErrorAction SilentlyContinue; Start-Sleep 2
$c = "$env:APPDATA\eden\config\qt-config.ini"
[IO.File]::WriteAllText($c, [IO.File]::ReadAllText($c).Replace("use_gdbstub=true", "use_gdbstub=false"))
$p = Start-Process C:\ME2_Recomp\eden\eden.exe -ArgumentList '-g', "`"$env:APPDATA\eden\sdmc\switch\me2-nx\me2.nro`"" -PassThru
Start-Sleep $Seconds
& C:\ME2_Recomp\me2recomp\tools\screenshot.ps1 -ProcessId $p.Id -Out $Shot
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
$f = Get-ChildItem "$env:APPDATA\eden\sdmc\switch\me2-nx\logs" -Filter *.log | Sort-Object LastWriteTime -Descending | Select-Object -First 1
"log $($f.Name): $((Get-Content $f.FullName).Count) lines"
Get-Content $f.FullName -Tail 12 | ForEach-Object { $_.Substring(0, [Math]::Min(200, $_.Length)) }
