# Builds (and optionally flashes) the CrossMux firmware for the reTerminal Sticky on Windows.
#
# Why this is more than `pio run`:
#  * ESP-IDF/SCons break on source paths past 260 chars, so the tree is
#    mirrored to a short path under %USERPROFILE%\.platformio and built there.
#  * Builds use the pioarduino fork, not stock PlatformIO, installed into a
#    private venv next to the mirror. CI pins 6.2.0, but on Windows that version
#    fails before compiling ("No module named 'SCons.Tool.FortranCommon'"), so
#    this script stays on 6.1.19, which builds the same platform.
#  * The PlatformIO workspace is the mirror's own .pio: scripts/patch_sdfat.py
#    refuses an SdFat outside <project>/.pio/libdeps.
#  * Upstream's pinned NimBLE now keeps the BleKeyboardHost passkey callback, and
#    scripts/patch_ble_keyboard_host.py fails if it is missing, so the mirror's
#    SDK copy is no longer rewritten here.
#
# Usage (from anywhere):
#   powershell -ExecutionPolicy Bypass -File tools\sticky\build-windows.ps1 [-Env sticky-gh_release] [-Upload] [-Port COM5]
param(
  [string]$Env = "sticky-gh_release",
  [switch]$Upload,
  [string]$Port = ""
)
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "..\..")).Path
$ws = Join-Path $env:USERPROFILE ".platformio\ws-crossmux"
$mirror = Join-Path $ws "m"
$venv = Join-Path $ws "venv"

if (-not (Test-Path (Join-Path $repo "freeink-sdk\libs"))) {
  git -C $repo -c core.longpaths=true submodule update --init freeink-sdk
  if ($LASTEXITCODE -ne 0) { throw "could not fetch the freeink-sdk submodule" }
}

if (-not (Test-Path "$venv\Scripts\python.exe")) {
  # Must be Python 3.11: pioarduino's builder imports packages (e.g. littlefs)
  # from its own 3.11 penv into this interpreter, so a 3.12+ venv fails with
  # "cannot import name 'lfs' from partially initialized module 'littlefs'".
  # Windows PowerShell turns any stderr line of a native command into a
  # terminating error under ErrorActionPreference=Stop (and uv/py print normal
  # progress there), so run them with errors non-fatal and check the result.
  $ErrorActionPreference = "Continue"
  if (Get-Command py -ErrorAction SilentlyContinue) { & py -3.11 -m venv $venv 2>&1 | Out-Null }
  if (-not (Test-Path "$venv\Scripts\python.exe") -and (Get-Command uv -ErrorAction SilentlyContinue)) {
    & uv venv --python 3.11 $venv 2>&1 | Out-Null  # uv fetches 3.11 if needed
    if (Test-Path "$venv\Scripts\python.exe") { & uv pip install --python "$venv\Scripts\python.exe" pip 2>&1 | Out-Null }
  }
  $ErrorActionPreference = "Stop"
  if (-not (Test-Path "$venv\Scripts\python.exe")) {
    throw "need Python 3.11 for the build venv: install it (py -3.11) or install uv"
  }
}
# Outside the creation branch so an existing venv gets this pin too.
$ErrorActionPreference = "Continue"
& "$venv\Scripts\python.exe" -m pip install -q "pioarduino==6.1.19" 2>&1 | Out-Null
$ErrorActionPreference = "Stop"
if ($LASTEXITCODE -ne 0) { throw "could not install pioarduino 6.1.19 into the build venv" }

robocopy $repo $mirror /MIR /XD .git .pio build /NFL /NDL /NJH /NJS /NP | Out-Null
# scripts/git_branch.py reads the commit with git from the mirror, and release
# builds refuse to build without it. A worktree's .git is a pointer file that
# robocopy already copied; for a normal checkout, point the mirror at it.
if (Test-Path (Join-Path $repo ".git") -PathType Container) {
  Set-Content -Path (Join-Path $mirror ".git") -Value ("gitdir: " + (Join-Path $repo ".git")) -Encoding ascii -NoNewline
}

$env:PLATFORMIO_WORKSPACE_DIR = Join-Path $mirror ".pio"
Push-Location $mirror
try {
  $pioArgs = @("run", "-e", $Env)
  if ($Upload) { $pioArgs += @("-t", "upload"); if ($Port) { $pioArgs += @("--upload-port", $Port) } }
  # python -m (not pio.exe): the exe launcher hard-codes the path it was installed from.
  & "$venv\Scripts\python.exe" -m platformio @pioArgs
  if ($LASTEXITCODE -ne 0) { throw "pio exited with $LASTEXITCODE" }
} finally { Pop-Location }
Write-Host "Firmware: $mirror\.pio\build\$Env\firmware.bin"
