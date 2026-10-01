# Builds (and optionally flashes) the CrossMux firmware for the reTerminal Sticky on Windows.
#
# Why this is more than `pio run`:
#  * ESP-IDF/SCons break on source paths past 260 chars, so the tree is
#    mirrored to a short path under %USERPROFILE%\.platformio and built there.
#  * CI uses the pioarduino fork (6.1.19), not stock PlatformIO; it is installed
#    into a private venv next to the mirror.
#  * pioarduino on Windows defers middleware Object() calls, so the NimBLE compat
#    rewrite in scripts/patch_ble_keyboard_host.py cannot swap the source; the
#    same rewrite is applied to the mirror's (build-only) SDK copy instead.
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
  & "$venv\Scripts\python.exe" -m pip install -q "pioarduino==6.1.19"
}

robocopy $repo $mirror /MIR /XD .git .pio build /NFL /NDL /NJH /NJS /NP | Out-Null

$patch = @'
import re, sys, pathlib
root = pathlib.Path(sys.argv[1])
src = (root / "scripts/patch_ble_keyboard_host.py").read_text(encoding="utf-8")
method = re.search(r'METHOD = """(.*?)"""', src, re.S).group(1).encode().decode("unicode_escape")
p = root / "freeink-sdk/libs/network/BleKeyboardHost/src/BleKeyboardHost.cpp"
s = p.read_text(encoding="utf-8")
if s.count(method) == 1:
    p.write_text(s.replace(method, "", 1), encoding="utf-8")
'@
$patch | & "$venv\Scripts\python.exe" - $mirror

$env:PLATFORMIO_WORKSPACE_DIR = $ws
Push-Location $mirror
try {
  $pioArgs = @("run", "-e", $Env)
  if ($Upload) { $pioArgs += @("-t", "upload"); if ($Port) { $pioArgs += @("--upload-port", $Port) } }
  # python -m (not pio.exe): the exe launcher hard-codes the path it was installed from.
  & "$venv\Scripts\python.exe" -m platformio @pioArgs
  if ($LASTEXITCODE -ne 0) { throw "pio exited with $LASTEXITCODE" }
} finally { Pop-Location }
Write-Host "Firmware: $ws\build\$Env\firmware.bin"
