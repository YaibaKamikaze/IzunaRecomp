# Izuna Mod Studio as a Windows app: Electron's official Windows build with the
# studio inside, in out\Izuna Mod Studio\ (run "Izuna Mod Studio.exe" there).
#
#   powershell -ExecutionPolicy Bypass -File tools\studio\electron\build-windows.ps1
#
# Downloads Electron from its GitHub releases and checks it against the
# release's own SHA-256 list. Nothing else is downloaded or installed.
$ErrorActionPreference = "Stop"
$version = "33.2.1"
$studio = Split-Path -Parent $PSScriptRoot
$out = Join-Path $studio "out"
$app = Join-Path $out "Izuna Mod Studio"
$zip = Join-Path $out "electron-v$version-win32-x64.zip"
New-Item -ItemType Directory -Force -Path $out | Out-Null

$base = "https://github.com/electron/electron/releases/download/v$version"
if (-not (Test-Path $zip)) {
  Write-Host "Downloading Electron $version..."
  Invoke-WebRequest "$base/electron-v$version-win32-x64.zip" -OutFile $zip
}
$sums = (Invoke-WebRequest "$base/SHASUMS256.txt").Content
$want = ($sums -split "`n" | Where-Object { $_ -match "electron-v$version-win32-x64\.zip" }) -replace "\s.*", ""
$have = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
if ($have -ne $want.Trim().ToLower()) { Remove-Item $zip; throw "The Electron download does not match its checksum: try again." }

if (Test-Path $app) { Remove-Item -Recurse -Force $app }
Expand-Archive $zip -DestinationPath $app
Rename-Item (Join-Path $app "electron.exe") "Izuna Mod Studio.exe"

# The studio itself, as the app's resources.
$res = Join-Path $app "resources\app"
New-Item -ItemType Directory -Force -Path $res | Out-Null
Copy-Item (Join-Path $studio "index.html"), (Join-Path $studio "studio.css") $res
Copy-Item -Recurse (Join-Path $studio "js") $res
New-Item -ItemType Directory -Force -Path (Join-Path $res "electron") | Out-Null
Copy-Item (Join-Path $PSScriptRoot "main.js") (Join-Path $res "electron")
Copy-Item (Join-Path $PSScriptRoot "package.json") $res
Remove-Item -Force (Join-Path $app "resources\default_app.asar") -ErrorAction SilentlyContinue
Write-Host "Done: $app\Izuna Mod Studio.exe"
