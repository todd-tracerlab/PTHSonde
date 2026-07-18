# Build the PTHSonde desktop app AND bundle the SHARPpy venv so the download is
# fully self-contained (the real SPC panel works with no separate install).
#
#   Run from processor\:   powershell -ExecutionPolicy Bypass -File build_app.ps1
#
# Steps:
#   1. PyInstaller builds dist\PTHSonde\PTHSonde.exe (onedir) from PTHSonde.spec.
#   2. venv310 (Python 3.10 + SHARPpy/PySide2) is copied next to the exe as
#      dist\PTHSonde\venv310, where the app's venv search finds it (APP_DIR\venv310).
#
# Note: the app is a subprocess architecture - the 3.13 pywebview exe shells out to
# venv310's python.exe to render the genuine SHARPpy panel. That is why the venv must
# ship alongside the exe rather than being frozen into it.
$ErrorActionPreference = "Stop"
$proc = $PSScriptRoot
Set-Location $proc

# Close a running instance so PyInstaller can overwrite the locked DLLs.
Get-Process PTHSonde -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800

Write-Host "==> PyInstaller build..."
py -3.13 -m PyInstaller PTHSonde.spec --noconfirm
if ($LASTEXITCODE -ne 0) { throw "PyInstaller failed ($LASTEXITCODE)" }

$dest = Join-Path $proc "dist\PTHSonde\venv310"
$src  = Join-Path $proc "venv310"
Write-Host "==> Bundling SHARPpy venv -> $dest"
if (-not (Test-Path $src)) { throw "venv310 not found at $src" }
# robocopy /MIR mirrors the venv; exit codes 0-7 are success for robocopy.
robocopy $src $dest /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }

$py = Join-Path $dest "Scripts\python.exe"
if (Test-Path $py) { Write-Host "==> OK - bundled python: $py" }
else { throw "bundled venv python missing at $py" }
Write-Host "==> Done. dist\PTHSonde is self-contained."
