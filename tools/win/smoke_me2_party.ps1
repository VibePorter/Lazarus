# ME2 PC smoke run at Kasumi's party (the console test spot), from a save placed there (2026-11-06).
#
# Prepares out\validation\<Name>\userdata with the save in out\party_save (a copy of a user data folder: profile and
# careers) and the DLC packages linked from ME2_DLC_DIR (a junction,
# nothing copied), then runs tools\win\smoke_me2_pc.ps1 with the menu path the user gave:
#   START, then DOWN x3, A, DOWN, DOWN, A (2 s apart; the main menu needs 3 DOWNs to Load, user 11-06 22:2x); the level loads in ~15-20 s. The presses count from marks the
#   game sets (mnk_input_driver.cpp): START 4 s after the Press Start screen (mark 1, the start-up movies done) and
#   again every 6 s until the main menu opened (the screen can still be loading DLC at 4 s: PC 11-06 21:07), the
#   menu presses from 6 s after the main menu opening (mark 2: the movie loads before the menu takes input; at 3 s a
#   press was lost twice, PC 11-06 21:28 / 21:59), so a slow or fast startup does not shift them. The save: the career's
#   newest save, at the party.
# -Spin: -SpinAt seconds after the main menu opened, turn the camera right for -SpinSeconds (the console runs' "spinning").
# Everything else (seconds, height, extra arguments) passes through.
param([string]$Name = 'party', [int]$Seconds = 150, [int]$Height = 720, [switch]$Spin, [int]$SpinAt = 60,
      [int]$SpinSeconds = 40, [int]$VideoAt = 100, [string[]]$ExtraArguments = @())
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$save = Join-Path $projectRoot 'out\party_save'
# The DLC packages (the 0000000000000000 folder of a user data root): ME2_DLC_DIR, default out\dlc.
$dlc = if ($env:ME2_DLC_DIR) { $env:ME2_DLC_DIR } else { Join-Path $projectRoot 'out\dlc' }
if (-not (Test-Path -LiteralPath $save)) { throw "no save at $save" }
if (-not (Test-Path -LiteralPath $dlc)) { throw "no DLC at $dlc" }
$report = Join-Path $projectRoot "out\validation\$Name"
if (Test-Path -LiteralPath $report) {
    # A junction is removed as a link, not followed (Remove-Item -Recurse would delete the DLC through it).
    $link = Join-Path $report 'userdata\0000000000000000'
    if (Test-Path -LiteralPath $link) { (Get-Item -LiteralPath $link).Delete() }
    Remove-Item -LiteralPath $report -Recurse -Force
}
$userdata = Join-Path $report 'userdata'
New-Item -ItemType Directory -Force -Path $userdata | Out-Null
Copy-Item -Path (Join-Path $save '*') -Destination $userdata -Recurse -Force
New-Item -ItemType Junction -Path (Join-Path $userdata '0000000000000000') -Target $dlc | Out-Null
$script = 'M1+4~2:START;M1+10~2:START;M1+16~2:START;M1+22~2:START;M1+28~2:START;M2+6:DOWN;M2+8:DOWN;M2+10:DOWN;M2+12:A;M2+14:DOWN;M2+16:DOWN;M2+18:A'
if ($Spin) { $script += ";M2+${SpinAt}:RX+:$SpinSeconds" }
# Only the script drives the game: no SDL backend, so a controller attached to this PC (all controllers are player 1,
# me2_shared_input) cannot move the menu cursor during the scripted presses (PC 11-06 22:09: a menu path went wrong).
$ExtraArguments = @('--input_backend=none') + $ExtraArguments
& (Join-Path $PSScriptRoot 'smoke_me2_pc.ps1') -Name $Name -Seconds $Seconds -Height $Height -InputScript $script -VideoAt $VideoAt `
    -ExtraArguments $ExtraArguments
# The junction goes before anyone cleans the run folder recursively.
$link = Join-Path $userdata '0000000000000000'
if (Test-Path -LiteralPath $link) { (Get-Item -LiteralPath $link).Delete() }
