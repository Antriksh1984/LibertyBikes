# Builds the installer exe and zips a release into releases\LibertyBikes-<version>.zip.
# Run Build.ps1 first. Needs Python 3 with PyInstaller (set $env:PYTHON or use ..\_deps\pyenv).
param([string]$Version = '1.0.0')
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$python = if ($env:PYTHON) { $env:PYTHON } else { Join-Path $root '..\_deps\pyenv\Scripts\python.exe' }
if (-not (Test-Path "$root\bin\LibertyBikes.asi")) { throw 'bin\LibertyBikes.asi is missing; run Build.ps1 first' }

$ErrorActionPreference = 'Continue'   # PyInstaller logs to stderr; Windows PowerShell would treat that as an error
& $python -m PyInstaller --noconfirm --onefile --console --noupx --name LibertyBikesInstaller `
    --distpath "$root\bin" --workpath "$root\build\pyinstaller" --specpath "$root\build\pyinstaller" `
    --exclude-module tkinter --exclude-module numpy "$root\tools\installer.py" 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Installer build failed' }
$ErrorActionPreference = 'Stop'

$stage = Join-Path $root 'build\package'
$zip = Join-Path $root "releases\LibertyBikes-$Version.zip"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force "$stage\scripts", (Split-Path $zip) | Out-Null
Copy-Item "$root\bin\LibertyBikes.asi", "$root\LibertyBikes.ini" "$stage\scripts"
Copy-Item "$root\bin\LibertyBikesInstaller.exe", "$root\README.md", "$root\LICENSE", "$root\LICENSE-reVC.txt", "$root\Plugin-SDK-LICENSE.txt" $stage
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Write-Output "Packaged $zip"
