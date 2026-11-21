param(
    [string]$BuildDir,
    [string]$Output,
    [Alias("Sdk")][string]$SdkInclude,
    [string]$SdkVendor
)
$ErrorActionPreference = 'Stop'
$source = Split-Path $PSScriptRoot -Parent
if (!$BuildDir) {
    $BuildDir = Join-Path $env:TEMP "Action75-Ferrofluid-$([guid]::NewGuid())"
}
if (!$Output) {
    $Output = Join-Path $BuildDir 'pack'
}

$configure = @('-S', $source, '-B', $BuildDir, '-G', 'Visual Studio 17 2022', '-A', 'x64')
if ($SdkInclude) {
    $publicInCheckout = Join-Path $SdkInclude 'skills/action75-wallpaper-author/sdk'
    if (Test-Path -LiteralPath (Join-Path $publicInCheckout 'Action75Wallpaper12.h')) {
        if (!$SdkVendor) {
            $SdkVendor = Join-Path $SdkInclude 'vendor-source/windows-native'
        }
        $SdkInclude = $publicInCheckout
    }
    $configure += "-DACTION75_SDK_INCLUDE=$((Resolve-Path -LiteralPath $SdkInclude).Path)"
}
if ($SdkVendor) {
    $configure += "-DACTION75_SDK_VENDOR=$((Resolve-Path -LiteralPath $SdkVendor).Path)"
}
cmake @configure
if ($LASTEXITCODE) { throw 'Ferrofluid configuration failed' }
cmake --build $BuildDir --config Release --parallel
if ($LASTEXITCODE) { throw 'Ferrofluid build failed' }
ctest --test-dir $BuildDir -C Release --output-on-failure
if ($LASTEXITCODE) { throw 'Ferrofluid tests failed' }
cmake --install $BuildDir --config Release --prefix $Output
if ($LASTEXITCODE) { throw 'Ferrofluid staging failed' }
