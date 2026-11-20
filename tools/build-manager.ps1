param([switch]$Test)
$ErrorActionPreference = 'Stop'
$managerRoot = Join-Path $PSScriptRoot '../apps/manager'
$cargoCommand = Get-Command cargo -ErrorAction SilentlyContinue
if ($cargoCommand) {
    $cargoExe = $cargoCommand.Source
} else {
    $cargoRoot = if ($env:CARGO_HOME) { $env:CARGO_HOME } else { Join-Path $env:USERPROFILE '.cargo' }
    $cargoExe = Join-Path $cargoRoot 'bin/cargo.exe'
    if (-not (Test-Path -LiteralPath $cargoExe)) {
        throw 'Cargo was not found. Install Rust or set CARGO_HOME to your Cargo directory.'
    }
}
Push-Location $managerRoot
try {
    if ($Test) {
        & $cargoExe test --release --locked
        if ($LASTEXITCODE -ne 0) { throw 'Manager tests failed.' }
    }
    & $cargoExe build --release --locked
    if ($LASTEXITCODE -ne 0) { throw 'Manager build failed.' }
} finally {
    Pop-Location
}
