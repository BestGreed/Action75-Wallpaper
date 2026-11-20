param([Parameter(Mandatory)][string]$Sdk, [string]$BuildDir = (Join-Path $env:TEMP "Action75-Ferrofluid-build"))
$ErrorActionPreference='Stop'
$src=$PSScriptRoot
$pack=(Resolve-Path "$src/../Ferrofluid").Path
$Sdk=(Resolve-Path $Sdk).Path
$fxc=(Get-ChildItem "${env:ProgramFiles(x86)}/Windows Kits/10/bin/*/x64/fxc.exe" | Sort-Object FullName -Descending | Select-Object -First 1).FullName
foreach($entry in 'floorVS','fluidVS','floorPS','fluidPS'){
    $profile=($entry.Substring($entry.Length-2).ToLower())+'_5_0'
    & $fxc /nologo /O3 /T $profile /E $entry /Fo "$pack/$entry.cso" "$src/Bath.hlsl"
    if($LASTEXITCODE){throw "Shader $entry failed"}
}
$vs=& "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$bat=Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
$build=$BuildDir
New-Item -Force -Type Directory $build | Out-Null
Push-Location $build
try{
    $line='call "{0}" >nul && cl /nologo /O2 /MT /EHsc /utf-8 /std:c++17 /LD /I"{1}/native/windows-player/include" /I"{1}/vendor-source/windows-native" "{2}/Studio.cpp" /link /OUT:"{3}/wallpaper.dll" /IMPLIB:wallpaper.lib' -f $bat,$Sdk,$src,$pack
    & cmd /d /c $line
    if($LASTEXITCODE){throw 'C++ build failed'}
}finally{Pop-Location}

