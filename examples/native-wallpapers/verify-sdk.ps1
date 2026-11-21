param(
    [Parameter(Mandatory)][string]$Archive,
    [string]$OutputDirectory = (Join-Path ([IO.Path]::GetTempPath()) ('a75-sdk-' + [guid]::NewGuid().ToString('N').Substring(0,8))),
    [string[]]$Project,
    [string]$WallpaperAssets,
    [string]$EffekseerArchive
)

$ErrorActionPreference = 'Stop'
$Archive = (Resolve-Path -LiteralPath $Archive).Path
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) {
    throw "Choose a new output directory. The verification will unpack and build there."
}

function Assert-NativeExit([string]$Operation) {
    if ($LASTEXITCODE -ne 0) {
        throw "$Operation failed with exit code $LASTEXITCODE."
    }
}

$extract = Join-Path $OutputDirectory 'source'
Expand-Archive -LiteralPath $Archive -DestinationPath $extract
$roots = @(Get-ChildItem -LiteralPath $extract -Directory)
if ($roots.Count -ne 1) { throw "Expected a source archive with one project directory." }
$root = $roots[0].FullName
$header = Join-Path $root 'vendor-source/windows-native/nlohmann/json.hpp'
$license = Join-Path $root 'Licenses/nlohmann-json-LICENSE.txt'
if (!(Test-Path -LiteralPath $header -PathType Leaf) -or !(Test-Path -LiteralPath $license -PathType Leaf)) {
    throw "The archive must include the nlohmann/json SDK header and its license."
}
if ((Get-FileHash -LiteralPath $header -Algorithm SHA256).Hash -ne 'AAF127C04CB31C406E5B04A63F1AE89369FCCDE6D8FA7CDDA1ED4F32DFC5DE63') {
    throw "The archive has an unexpected nlohmann/json SDK header."
}
Write-Output 'Verified nlohmann/json 3.12.0 header and license.'

$projects = [ordered]@{
    'key-game' = @{ Packs = @('night-platform-runner'); Tests = $true }
    'tidal-signal' = @{ Packs = @('tidal-signal'); Tests = $true }
    'dx12-probe' = @{ Packs = @('dx12-key-probe') }
    'supernova-horizon' = @{ Packs = @('supernova-horizon') }
    'black-hole' = @{ Packs = @('event-horizon-native') }
    'ferrofluid' = @{ Packs = @('meniscus-ferrofluid-v1'); Tests = $true }
    'atlas-packs' = @{
        Packs = @('afterimage', 'carrier', 'folio', 'meridian', 'paintball-native', 'tidal-glass')
        Target = 'action75_atlas_packs'
        StagingVariable = 'ACTION75_ATLAS_STAGING'
        Effekseer = 'ACTION75_EFFEKSEER_ROOT'
    }
    'field-keycap' = @{ Packs = @('silk-nebula', 'gilded-synapse', 'harmonic-textile', 'keycap-glow') }
    'surface-scenes' = @{ Packs = @('stillwater', 'glass-shatter', 'night-fireworks') }
    'night-orbit-trio' = @{
        Source = 'examples/author-wallpapers/sources/dx12/night-orbit-trio'
        Packs = @('night-orbit-trio')
        Tests = $true
        BundledSdk = $true
    }
    'soundings-echo' = @{
        Source = 'examples/author-wallpapers/sources/dx12/soundings-echo'
        Packs = @('soundings-echo')
        Tests = $true
        BundledSdk = $true
    }
    'particles' = @{
        Source = 'examples/author-wallpapers/sources/dx12/particles'
        Packs = @('lantern-almanac', 'tidemark-bearing')
        Effekseer = 'EFFEKSEER_ROOT'
    }
    'key-game-abi1' = @{
        Source = 'tools/fixtures/key-game-abi1'
        Packs = @('night-platform-runner')
        Tests = $true
        Legacy = $true
    }
}

$selected = if ($Project) { $Project } else { $projects.Keys }
$inventory = Get-Content -LiteralPath (Join-Path $root 'tools/release-assets.json') -Raw | ConvertFrom-Json
if ('atlas-packs' -in $selected) {
    if ($WallpaperAssets) {
        $WallpaperAssets = (Resolve-Path -LiteralPath $WallpaperAssets).Path
        $paintballAssets = @($inventory.externalWallpaperFiles.'paintball-native')
        $generated = @($inventory.generatedWallpaperFiles.'paintball-native' | Where-Object { $_ })
        $present = @($generated | Where-Object { Test-Path -LiteralPath (Join-Path $WallpaperAssets "paintball-native/$_") -PathType Leaf })
        if ($present.Count -gt 0 -and $present.Count -ne $generated.Count) {
            throw 'The Paintball asset bundle has an incomplete generated texture cache.'
        }
        $paintballAssets += $present
        foreach ($asset in $paintballAssets) {
            $relative = "paintball-native/$asset"
            $target = Join-Path $root "wallpapers-native/$relative"
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
            Copy-Item -LiteralPath (Join-Path $WallpaperAssets $relative) -Destination $target
        }
    } else {
        python -B (Join-Path $root 'tools/restore-wallpaper-assets.py')
        Assert-NativeExit 'restore external artwork'
    }
}
if (@($selected | Where-Object { $projects[$_].Effekseer }).Count) {
    $vendorScratch = Join-Path ([IO.Path]::GetTempPath()) ('a75-vendor-' + [guid]::NewGuid().ToString('N').Substring(0,8))
    New-Item -ItemType Directory -Path $vendorScratch | Out-Null
    if (!$EffekseerArchive) {
        $EffekseerArchive = Join-Path $vendorScratch 'EffekseerForCpp1.80.8.zip'
        Invoke-WebRequest 'https://github.com/effekseer/Effekseer/releases/download/1808/EffekseerForCpp1.80.8.zip' -OutFile $EffekseerArchive
    }
    $vendorHash = (Get-FileHash -LiteralPath $EffekseerArchive -Algorithm SHA256).Hash
    if ($vendorHash -ne '4206C2B3704FEE9F84331E18C7F80D061F7989910B1FADA82C7857B2B5EFC035') {
        throw 'Effekseer public SDK archive checksum mismatch'
    }
    Expand-Archive -LiteralPath $EffekseerArchive -DestinationPath $vendorScratch
    $effekseer = Join-Path $vendorScratch 'EffekseerForCpp1.80.8'
    "EffekseerForCpp1.80.8.zip SHA256 $vendorHash" | Set-Content -LiteralPath (Join-Path $OutputDirectory 'vendor.txt')
}

$public = Join-Path $OutputDirectory 'public'
foreach ($relative in @('examples', 'wallpapers-native', 'skills/action75-wallpaper-author/sdk',
                         'vendor-source/windows-native/nlohmann', 'tools/fixtures/key-game-abi1',
                         'LICENSE', 'Licenses/nlohmann-json-LICENSE.txt')) {
    $target = Join-Path $public $relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath (Join-Path $root $relative) -Destination $target -Recurse
}
$sdk = Join-Path $public 'skills/action75-wallpaper-author/sdk'
$vendor = Join-Path $public 'vendor-source/windows-native'
$report = @{}
foreach ($example in $selected) {
    $spec = $projects[$example]
    if (!$spec) { throw "Unknown author build: $example" }
    $relativeSource = if ($spec.Source) { $spec.Source } else { "examples/native-wallpapers/$example" }
    $source = Join-Path $public $relativeSource
    $build = Join-Path $OutputDirectory "$example-build"
    $pack = Join-Path $OutputDirectory "$example-pack"
    $configure = @()
    if (!$spec.BundledSdk) {
        $configure += "-DACTION75_SDK_INCLUDE=$sdk", "-DACTION75_SDK_VENDOR=$vendor"
    }
    if ($spec.Configure) { $configure += $spec.Configure }
    if ($spec.StagingVariable) { $configure += "-D$($spec.StagingVariable)=$pack" }
    if ($spec.Effekseer) { $configure += "-D$($spec.Effekseer)=$effekseer" }
    cmake -S $source -B $build -G 'Visual Studio 17 2022' -A x64 @configure
    Assert-NativeExit "$example configure"
    $targetArgs = if ($spec.Target) { @('--target', $spec.Target) } else { @() }
    cmake --build $build --config Release --parallel 2 @targetArgs
    Assert-NativeExit "$example build"
    if ($spec.Tests) {
        ctest --test-dir $build -C Release --output-on-failure --no-tests=error
        Assert-NativeExit "$example tests"
    }
    if (!$spec.StagingVariable) {
        cmake --install $build --config Release --prefix $pack
        Assert-NativeExit "$example package"
    }
    $recordings = @(Get-ChildItem -LiteralPath $build -Recurse -Filter 'CL.read.1.tlog' -File)
    if ($recordings.Count -eq 0) { throw "No compiler input traces found for $example" }
    foreach ($trace in $recordings) {
        $inputs = Get-Content -LiteralPath $trace.FullName -Raw
        if ($inputs -match '[\\/]native[\\/]windows-player[\\/]' -or $inputs -match '[\\/]apps[\\/]manager[\\/]') {
            throw "$example reads private application source: $($trace.FullName)"
        }
    }

    $manifests = @(Get-ChildItem -LiteralPath $pack -Recurse -Filter 'manifest.json' -File)
    $ids = @($manifests | ForEach-Object { (Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json).id })
    if ($ids.Count -ne $spec.Packs.Count -or (Compare-Object ($ids | Sort-Object) ($spec.Packs | Sort-Object))) {
        throw "Unexpected package IDs for $example. Found $ids"
    }
    $peReport = Join-Path $OutputDirectory "$example-pe.json"
    $arguments = @('--report', $peReport)
    $option = if ($spec.Legacy) { '--legacy-fixture' } else { '--pack' }
    foreach ($manifest in $manifests) { $arguments += @($option, $manifest.DirectoryName) }
    python -B (Join-Path $root 'tools/verify-native-wallpapers.py') @arguments
    Assert-NativeExit "$example ABI and imports"
    $report[$example] = @{ Packs = $ids; CompilerTraces = $recordings.Count; PE = $peReport; Tests = [bool]$spec.Tests }
    Write-Output "PASS $example public-only build, package and DLL inspection"
}
$report | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'sdk-builds.json') -Encoding utf8
Write-Output "SDK verification output in $OutputDirectory"
