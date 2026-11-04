# Saves targets of a RenderDoc capture as PNG: rdc_save.ps1 -Capture <rdc> -Dir <folder> -Eids 1,2,3 [-Depth]
param([Parameter(Mandatory)] [string] $Capture, [Parameter(Mandatory)] [string] $Dir, [Parameter(Mandatory)] [int[]] $Eids,
      [switch] $Depth)
New-Item -ItemType Directory -Force $Dir | Out-Null
$env:RDC_IN = $Capture; $env:RDC_DIR = $Dir; $env:RDC_SAVE = ($Eids -join ';'); $env:RDC_DEPTH = if ($Depth) { '1' } else { '' }
$p = Start-Process 'C:\Program Files\RenderDoc\qrenderdoc.exe' -ArgumentList '--python', "$PSScriptRoot\rdc_save.py" `
    -PassThru -WindowStyle Minimized
if (-not $p.WaitForExit(900000)) { Stop-Process -Id $p.Id -Force; 'timeout' }
Get-Content (Join-Path $Dir 'save.txt')
