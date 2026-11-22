[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$CghSdk,
    [string]$Premake = 'premake5',
    [string]$MSBuild
)

$ErrorActionPreference = 'Stop'
$CghSdk = (Resolve-Path -LiteralPath $CghSdk).Path
if (!(Test-Path -LiteralPath (Join-Path $CghSdk 'cgh_sdk.h') -PathType Leaf)) {
    throw 'CghSdk must point to the CGH SDK directory containing cgh_sdk.h.'
}
$Premake = (Get-Command $Premake -CommandType Application).Source
if (!$MSBuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $MSBuild = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild/Current/Bin/amd64/MSBuild.exe' | Select-Object -First 1
    if (!$MSBuild) { throw 'Install the Visual Studio 2022 C++ x64 build tools or pass -MSBuild.' }
}

$binary = Join-Path $PSScriptRoot 'binary'
Push-Location $binary
try {
    & $Premake vs2022 "--cgh-sdk=$CghSdk"
    if ($LASTEXITCODE -ne 0) { throw 'Premake failed. Use Premake 5.0.0-beta2 as in the upstream workflow.' }
    & $MSBuild gmcl_gwater2.vcxproj /m:1 /nr:false '/clp:ErrorsOnly;Summary' /p:Configuration=Release /p:Platform=x64
    if ($LASTEXITCODE -ne 0) { throw 'The GWater2FCGH Win64 build failed.' }
} finally {
    Pop-Location
}

$out = Join-Path $PSScriptRoot 'out'
$staging = Join-Path $out ('.package-' + [guid]::NewGuid().ToString('N'))
$mod = Join-Path $staging 'GWater2FCGH'
$addon = Join-Path $mod 'Addons/gwater2'
$native = Join-Path $mod 'Native'
$licenses = Join-Path $mod 'licenses'
$shaders = Join-Path $addon 'shaders/fxc'
New-Item -ItemType Directory -Path $native, $licenses, $shaders -Force | Out-Null

try {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'cghm_metadata.json') -Destination $mod
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'LICENSE'), (Join-Path $PSScriptRoot 'README.md') -Destination $mod
    Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'licenses') -File | Copy-Item -Destination $licenses
    $dependencyLicenses = @{
        'FleX.txt' = 'FleX/LICENSE.txt'
        'AMD-AGS.txt' = 'FleX/external/ags_lib/LICENSE.txt'
        'BSPParser.txt' = 'BSPParser/LICENSE'
        'ThreadPool.txt' = 'ThreadPool/COPYING'
        'garrysmod_common.txt' = 'garrysmod_common/license.txt'
        'scanning.txt' = 'garrysmod_common/scanning/license.txt'
    }
    foreach ($notice in $dependencyLicenses.GetEnumerator()) {
        Copy-Item -LiteralPath (Join-Path $binary $notice.Value) -Destination (Join-Path $licenses $notice.Key)
    }
    Copy-Item -LiteralPath (Join-Path $binary 'x86_64/Release/gwater2fcgh_win64.dll') -Destination $native
    foreach ($file in 'NvFlexReleaseD3D_x64.dll', 'NvFlexExtReleaseD3D_x64.dll', 'amd_ags_x64.dll', 'GFSDK_Aftermath_Lib.x64.dll', 'nvToolsExt64_1.dll') {
        Copy-Item -LiteralPath (Join-Path $binary "FleX/bin/win64/$file") -Destination $native
    }
    foreach ($folder in 'data_static', 'lua', 'materials', 'models', 'resource', 'sound') {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $folder) -Destination $addon -Recurse
    }
    Get-ChildItem -LiteralPath (Join-Path $binary 'src/shaders/vcs') -Filter '*.vcs' -File | Copy-Item -Destination $shaders
    $package = Join-Path $out 'GWater2FCGH.zip'
    Compress-Archive -LiteralPath $mod -DestinationPath $package -Force
    Get-Item -LiteralPath $package
} finally {
    $resolvedOut = (Resolve-Path -LiteralPath $out).Path
    $resolvedStaging = (Resolve-Path -LiteralPath $staging).Path
    if ((Split-Path -Parent $resolvedStaging) -eq $resolvedOut -and (Split-Path -Leaf $resolvedStaging) -match '^\.package-[a-f0-9]{32}$') {
        Remove-Item -LiteralPath $resolvedStaging -Recurse -Force
    }
}
