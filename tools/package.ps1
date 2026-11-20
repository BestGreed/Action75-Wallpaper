param([string]$Compiler, [string]$Manager, [string]$Player, [string]$OutputDirectory)

$ErrorActionPreference = 'Stop'
$arguments = @()
if ($Compiler) { $arguments += @('--compiler', $Compiler) }
if ($Manager) { $arguments += @('--manager', $Manager) }
if ($Player) { $arguments += @('--player', $Player) }
if ($OutputDirectory) { $arguments += @('--output-dir', $OutputDirectory) }

& python (Join-Path $PSScriptRoot 'release_payload.py') @arguments
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed.' }
