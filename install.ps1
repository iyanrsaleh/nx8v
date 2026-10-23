# install.ps1 — pasang nx8v secara global agar bisa dipanggil dari mana saja.
# Dari rilis GitHub:
#   powershell -ExecutionPolicy Bypass -Command "irm https://github.com/iyanrsaleh/nx8v/releases/latest/download/install.ps1 | iex"
# Dari pohon sumber yang sudah dibangun: .\install.ps1 [-Remove]

param(
  [switch]$Remove
)

$ErrorActionPreference = "Stop"

$destDir = Join-Path $env:LOCALAPPDATA "nx8v\bin"
$dest    = Join-Path $destDir "nx8v.exe"
$release = "https://github.com/iyanrsaleh/nx8v/releases/latest/download"

if ($Remove) {
  if (Test-Path $destDir) { Remove-Item $destDir -Recurse -Force }
  $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
  $cleaned  = ($userPath -split ";" | Where-Object { $_ -and $_ -ne $destDir }) -join ";"
  [Environment]::SetEnvironmentVariable("Path", $cleaned, "User")
  Write-Host "nx8v dihapus dari sistem ($destDir)."
  Write-Host "Buka terminal baru agar PATH diperbarui."
  exit 0
}

New-Item -ItemType Directory -Force -Path $destDir | Out-Null

$local = $null
if ($PSScriptRoot) {
  $candidate = Join-Path $PSScriptRoot "build\Release\nx8v.exe"
  if (Test-Path $candidate) { $local = $candidate }
}

function Copy-Nx8vLoader($fromDir) {
  $loader = Join-Path $fromDir "WebView2Loader.dll"
  if (Test-Path $loader) {
    Copy-Item $loader (Join-Path $destDir "WebView2Loader.dll") -Force
  }
}

if ($local) {
  Copy-Item $local $dest -Force
  Copy-Nx8vLoader (Split-Path $local -Parent)
  $linuxSrc = Join-Path $PSScriptRoot "build\linux\nx8v"
  if (Test-Path $linuxSrc) {
    Copy-Item $linuxSrc (Join-Path $destDir "nx8v-linux") -Force
  }
} else {
  $zip = Join-Path $env:TEMP "nx8v-win-x64.zip"
  $unpack = Join-Path $env:TEMP "nx8v-win-x64"
  Invoke-WebRequest "$release/nx8v-win-x64.zip" -OutFile $zip
  if (Test-Path $unpack) { Remove-Item $unpack -Recurse -Force }
  Expand-Archive $zip -DestinationPath $unpack -Force
  $exe = Get-ChildItem $unpack -Filter nx8v.exe -Recurse | Select-Object -First 1
  if (-not $exe) { Write-Error "nx8v.exe tidak ada di arsip rilis." }
  Copy-Item $exe.FullName $dest -Force
  Copy-Nx8vLoader $exe.DirectoryName
}

# tambahkan ke PATH user bila belum ada
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
$entries  = $userPath -split ";"
if ($entries -notcontains $destDir) {
  [Environment]::SetEnvironmentVariable("Path", ($userPath.TrimEnd(";") + ";" + $destDir), "User")
  Write-Host "PATH user diperbarui."
}

Write-Host ""
Write-Host "nx8v terpasang: $dest"
Write-Host "Buka terminal BARU lalu coba:"
Write-Host "  nx8v --version"
Write-Host "  nx8v init"
