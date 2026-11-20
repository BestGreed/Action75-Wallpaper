param([switch]$Test, [string]$BuildRoot)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$source=Join-Path $root 'native/windows-player'
$build=Join-Path $source 'build'
$configure=@()
if ($BuildRoot) {
    $BuildRoot=[IO.Path]::GetFullPath($BuildRoot)
    $build=Join-Path $BuildRoot 'build'
    $configure+="-DACTION75_DIST_DIR=$(Join-Path $BuildRoot 'dist')"
}
$header=Join-Path $root 'vendor-source/windows-native/nlohmann/json.hpp'
if(-not (Test-Path -LiteralPath $header)) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $header) | Out-Null
    Invoke-WebRequest 'https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp' -OutFile $header
}
if((Get-FileHash -LiteralPath $header -Algorithm SHA256).Hash -ne 'AAF127C04CB31C406E5B04A63F1AE89369FCCDE6D8FA7CDDA1ED4F32DFC5DE63') {throw 'JSON header checksum mismatch.'}
& cmake -S $source -B $build -G 'Visual Studio 17 2022' -A x64 @configure
if($LASTEXITCODE -ne 0){throw 'CMake configure failed.'}
$targets=if($Test){ @() }else{ @('--target','Action75Native') }
& cmake --build $build --config Release @targets --parallel 2
if($LASTEXITCODE -ne 0){throw 'Player build failed.'}
if($Test){
    & ctest --test-dir $build -C Release --output-on-failure --no-tests=error
    if($LASTEXITCODE -ne 0){throw 'Player tests failed.'}
}
