# E4 builder for Windows: turns your E4 Xbox Live Arcade zip into a playable E4 folder.
# Started by "Build E4.bat". Steps: extract the game, make sure a C++ compiler is installed,
# recompile the game's code on this PC, build E4 and put everything in the E4 folder.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # downloads are far faster without the progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Kit   = $PSScriptRoot
$Here  = Split-Path $Kit -Parent
$Game  = Join-Path $Kit 'game'
$Proj  = Join-Path $Kit 'e4recomp'
$Sdk   = Join-Path $Kit 'sdk'
$Tools = Join-Path $Kit 'tools'
$Out   = Join-Path $Here 'E4'
$ExpectedXex = 'C4157358ACE8664504EDC95348E20B4588C294A8FE32821EF4B2F2A9461A3FD9'

function Step($text) { Write-Host ''; Write-Host "=== $text ===" -ForegroundColor Cyan }
function Fail($text) { Write-Host ''; Write-Host $text -ForegroundColor Red; exit 1 }
function Check($what) { if ($LASTEXITCODE -ne 0) { Fail "$what failed (exit code $LASTEXITCODE)." } }

function Get-LatestAsset($repo, $pattern) {
  $rel = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest" `
    -Headers @{ 'User-Agent' = 'E4-builder' }
  $asset = $rel.assets | Where-Object { $_.name -match $pattern } | Select-Object -First 1
  if (-not $asset) { Fail "Couldn't find a download for $repo." }
  return $asset.browser_download_url
}
function Download($url, $dest) {
  Write-Host "  downloading $(Split-Path $url -Leaf) ..."
  Invoke-WebRequest $url -OutFile $dest -UseBasicParsing
}
function Run-Elevated($exe, $arguments, $what) {
  Write-Host "  installing $what - Windows will ask for permission (this can take a while) ..."
  $p = Start-Process $exe -ArgumentList $arguments -Verb RunAs -Wait -PassThru
  if ($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010) { Fail "Installing $what failed (exit code $($p.ExitCode))." }
}
function Is-Package($file) {
  if ($file.Length -lt 1MB) { return $false }
  $fs = [IO.File]::OpenRead($file.FullName)
  try { $b = New-Object byte[] 4; [void]$fs.Read($b, 0, 4) } finally { $fs.Close() }
  $magic = [Text.Encoding]::ASCII.GetString($b)
  return @('LIVE', 'CON ', 'PIRS') -contains $magic
}

Write-Host 'E4: Every Extend Extra Extreme - builder' -ForegroundColor White
# Files from a downloaded zip are marked as "from the internet"; unblock our own files.
Get-ChildItem $Kit -Recurse -File | Unblock-File -ErrorAction SilentlyContinue

# --- 1. Game files -----------------------------------------------------------
Step '1/5  Game files'
if (-not (Test-Path (Join-Path $Game 'default.xex'))) {
  $tmp = Join-Path $Kit 'extract_tmp'
  Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
  $candidates = @()
  $zip = Get-ChildItem $Here -Filter *.zip -File | Where-Object { $_.Name -notlike 'E4-Builder*' } |
    Select-Object -First 1
  if ($zip) {
    Write-Host "  unzipping $($zip.Name) ..."
    Expand-Archive $zip.FullName $tmp -Force
    $candidates += Get-ChildItem $tmp -Recurse -File
  }
  $candidates += Get-ChildItem $Here -File   # or a loose package file next to the builder
  $pkg = $candidates | Where-Object { Is-Package $_ } | Select-Object -First 1
  if (-not $pkg) {
    Fail ("No game found. Put your E4 Xbox Live Arcade zip (or its package file) in`n  $Here`n" +
          "next to 'Build E4.bat', then run it again.")
  }
  Write-Host "  extracting $($pkg.Name) ..."
  & (Join-Path $Tools 'e4_extract.exe') $pkg.FullName $Game
  Check 'Extracting the game'
  Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
if (-not (Test-Path (Join-Path $Game 'default.xex'))) { Fail 'That package has no default.xex - is it E4?' }
$hash = (Get-FileHash (Join-Path $Game 'default.xex') -Algorithm SHA256).Hash
if ($hash -ne $ExpectedXex) {
  Write-Host "  WARNING: this default.xex is a different version than E4 supports ($hash)." -ForegroundColor Yellow
  if ((Read-Host '  Continue anyway? (y/n)') -ne 'y') { exit 1 }
} else {
  Write-Host '  game files OK'
}

# --- 2. Compiler ---------------------------------------------------------------
Step '2/5  Compiler (installed once)'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
function Has-MsvcTools {
  if (-not (Test-Path $vswhere)) { return $false }
  $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  $sdkLib = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Lib\*\um\x64\kernel32.lib" -ErrorAction SilentlyContinue
  return [bool]$vs -and [bool]$sdkLib
}
if (-not (Has-MsvcTools)) {
  $bt = Join-Path $env:TEMP 'vs_BuildTools.exe'
  Download 'https://aka.ms/vs/17/release/vs_BuildTools.exe' $bt
  Run-Elevated $bt '--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended' 'Visual Studio Build Tools (C++ libraries)'
  if (-not (Has-MsvcTools)) { Fail 'The Visual Studio Build Tools did not install correctly. Restart the PC and run the builder again.' }
}
$llvm = Join-Path $env:ProgramFiles 'LLVM\bin'
if (-not (Test-Path (Join-Path $llvm 'clang++.exe'))) {
  $inst = Join-Path $env:TEMP 'llvm-setup.exe'
  Download (Get-LatestAsset 'llvm/llvm-project' '^LLVM-[0-9.]+-win64\.exe$') $inst
  Run-Elevated $inst '/S' 'LLVM (clang compiler)'
  if (-not (Test-Path (Join-Path $llvm 'clang++.exe'))) { Fail 'LLVM did not install correctly.' }
}
$env:Path = "$llvm;$env:Path"
Write-Host '  compiler OK'

# CMake and Ninja: portable copies inside the kit (nothing installed).
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
  $cm = Get-ChildItem $Tools -Directory -Filter 'cmake-*' -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $cm) {
    $z = Join-Path $env:TEMP 'cmake.zip'
    Download (Get-LatestAsset 'Kitware/CMake' '^cmake-[0-9.]+-windows-x86_64\.zip$') $z
    Expand-Archive $z $Tools -Force
    $cm = Get-ChildItem $Tools -Directory -Filter 'cmake-*' | Select-Object -First 1
  }
  $env:Path = "$(Join-Path $cm.FullName 'bin');$env:Path"
}
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
  if (-not (Test-Path (Join-Path $Tools 'ninja.exe'))) {
    $z = Join-Path $env:TEMP 'ninja.zip'
    Download (Get-LatestAsset 'ninja-build/ninja' '^ninja-win\.zip$') $z
    Expand-Archive $z $Tools -Force
  }
  $env:Path = "$Tools;$env:Path"
}

# --- 3. Recompile ----------------------------------------------------------------
Step '3/5  Recompiling the game code from your default.xex (a few minutes)'
if (-not (Test-Path (Join-Path $Proj 'generated\default\e4_init.h'))) {
  Push-Location $Proj
  & (Join-Path $Sdk 'bin\rexglue.exe') codegen e4_manifest.toml
  $code = $LASTEXITCODE
  Pop-Location
  if ($code -ne 0) { Fail "Recompiling failed (exit code $code)." }
} else {
  Write-Host '  already done'
}

# --- 4. Build --------------------------------------------------------------------
Step '4/5  Building E4 (10-20 minutes the first time)'
Push-Location $Proj
cmake --preset win-amd64-release "-Drexglue_DIR=$Sdk\lib\cmake\rexglue"
$code = $LASTEXITCODE
if ($code -eq 0) { cmake --build --preset win-amd64-release; $code = $LASTEXITCODE }
Pop-Location
if ($code -ne 0) { Fail "Building failed (exit code $code). Run the builder again, or report the error." }

# --- 5. Put it together -------------------------------------------------------------
Step '5/5  Putting E4 together'
$build = Join-Path $Proj 'out\build\win-amd64-release'
New-Item -ItemType Directory -Force $Out | Out-Null
Copy-Item (Join-Path $build 'e4.exe'), (Join-Path $build 'e4_launcher.exe') $Out -Force
Copy-Item (Join-Path $build '*.dll') $Out -Force
Copy-Item (Join-Path $Sdk 'bin\*.dll') $Out -Force -ErrorAction SilentlyContinue
if (-not (Test-Path (Join-Path $Out 'game\default.xex'))) {
  Copy-Item $Game (Join-Path $Out 'game') -Recurse -Force
}

Write-Host ''
Write-Host 'Done!' -ForegroundColor Green
Write-Host "E4 is in: $Out"
Write-Host 'Start it with e4_launcher.exe in that folder (enter a name, then PLAY).'
Start-Process explorer.exe $Out
