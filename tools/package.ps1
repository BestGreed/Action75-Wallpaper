param([string]$Compiler, [string]$BuildReceipt, [string]$OutputDirectory, [string]$Wallpapers)

$ErrorActionPreference = 'Stop'
$arguments = @()
if ($Compiler) { $arguments += @('--compiler', $Compiler) }
if ($BuildReceipt) { $arguments += @('--receipt', $BuildReceipt) }
if ($Wallpapers) { $arguments += @('--wallpapers', $Wallpapers) }
if ($OutputDirectory) { $arguments += @('--output-dir', $OutputDirectory) }

& python (Join-Path $PSScriptRoot 'release_payload.py') @arguments
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed.' }
