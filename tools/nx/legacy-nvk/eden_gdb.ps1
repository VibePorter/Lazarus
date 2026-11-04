# Runs me2.nro in Eden with its GDB stub, dumps all thread stacks after $Seconds, then stops Eden.
# usage: powershell -File eden_gdb.ps1 [-Seconds 45]
param([int]$Seconds = 45, [string]$Mode = "stacks")
Stop-Process -Name eden -Force -ErrorAction SilentlyContinue; Start-Sleep 2
$c = "$env:APPDATA\eden\config\qt-config.ini"
$s = [IO.File]::ReadAllText($c).Replace("use_gdbstub\default=true", "use_gdbstub\default=false").Replace("use_gdbstub=false", "use_gdbstub=true")
[IO.File]::WriteAllText($c, $s)
Start-Process C:\ME2_Recomp\eden\eden.exe -ArgumentList '-g', "`"$env:APPDATA\eden\sdmc\switch\me2-nx\me2.nro`"" | Out-Null
Start-Sleep 5
wsl.exe -d Ubuntu-24.04 -u root -- bash /mnt/c/ME2_Recomp/tools/nx/gdb_${Mode}.sh $Seconds | Out-Null
Stop-Process -Name eden -Force -ErrorAction SilentlyContinue
$s = [IO.File]::ReadAllText($c).Replace("use_gdbstub=true", "use_gdbstub=false")
[IO.File]::WriteAllText($c, $s)
wsl.exe -d Ubuntu-24.04 -u root -- cp /root/gdb_stacks.log /mnt/c/ME2_Recomp/nx_out/gdb_stacks.log
