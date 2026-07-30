#requires -Version 5.1
# SPDX-License-Identifier: MPL-2.0

[CmdletBinding()]
param(
    [string]$AndroidSdk = "$env:LOCALAPPDATA\Android\Sdk",
    [string]$NdkVersion = '28.1.13356709',
    [string]$VcpkgRoot = 'C:\Users\c2320\Documents\ccore-openvpn3-vcpkg',
    [string]$BuildDirectory = 'out\android-arm64-v8a-api23-ndk28',
    [string]$OutputDirectory = 'dist\android\arm64-v8a',
    [string]$ReleaseCommit = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$ndkRoot = Join-Path $AndroidSdk "ndk\$NdkVersion"
$androidToolchain = Join-Path $ndkRoot 'build\cmake\android.toolchain.cmake'
$toolchain = Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
$cmake = Join-Path $AndroidSdk 'cmake\3.31.6\bin\cmake.exe'
$ninja = Join-Path $AndroidSdk 'cmake\3.31.6\bin\ninja.exe'
$strip = Join-Path $ndkRoot 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe'
$nm = Join-Path $ndkRoot 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-nm.exe'
$readelf = Join-Path $ndkRoot 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe'
$buildPath = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    [IO.Path]::GetFullPath($BuildDirectory)
} else {
    [IO.Path]::GetFullPath((Join-Path $repositoryRoot $BuildDirectory))
}
$triplets = Join-Path $repositoryRoot 'deps\triplets'
$outputPath = if ([IO.Path]::IsPathRooted($OutputDirectory)) {
    [IO.Path]::GetFullPath($OutputDirectory)
} else {
    [IO.Path]::GetFullPath((Join-Path $repositoryRoot $OutputDirectory))
}

foreach ($required in @($ndkRoot, $androidToolchain, $toolchain, $cmake, $ninja, $strip, $nm, $readelf)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required build dependency is missing: $required"
    }
}

$env:ANDROID_NDK_HOME = $ndkRoot
$env:ANDROID_NDK_ROOT = $ndkRoot

& $cmake -S $repositoryRoot -B $buildPath -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
    "-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=$androidToolchain" `
    '-DVCPKG_TARGET_TRIPLET=arm64-android23' `
    '-DVCPKG_HOST_TRIPLET=x64-mingw-dynamic' `
    "-DVCPKG_OVERLAY_TRIPLETS=$triplets" `
    "-DVCPKG_MANIFEST_DIR=$(Join-Path $repositoryRoot 'deps\vcpkg_manifests\android')" `
    "-DVCPKG_OVERLAY_PORTS=$(Join-Path $repositoryRoot 'deps\vcpkg-ports')" `
    '-DVCPKG_FEATURE_FLAGS=manifests' `
    '-DANDROID_ABI=arm64-v8a' `
    '-DANDROID_PLATFORM=android-23' `
    '-DBUILD_CCORE_ENGINE=ON' `
    '-DBUILD_TESTING=OFF' `
    '-DCMAKE_BUILD_TYPE=Release'
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE" }

& $cmake --build $buildPath --target ccore_openvpn3
if ($LASTEXITCODE -ne 0) { throw "CMake build failed with exit code $LASTEXITCODE" }

$library = Get-ChildItem -LiteralPath $buildPath -Recurse -Filter 'libccore_openvpn3.so' | Select-Object -First 1
if ($null -eq $library) { throw 'libccore_openvpn3.so was not produced' }

New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
$outputLibrary = Join-Path $outputPath $library.Name
Copy-Item -LiteralPath $library.FullName -Destination $outputLibrary -Force
& $strip --strip-unneeded $outputLibrary
if ($LASTEXITCODE -ne 0) { throw "llvm-strip failed with exit code $LASTEXITCODE" }

$elfHeader = @(& $readelf -h $outputLibrary)
if ($LASTEXITCODE -ne 0) { throw "llvm-readelf failed with exit code $LASTEXITCODE" }
foreach ($requiredHeader in @('Class:\s+ELF64', 'Type:\s+DYN', 'Machine:\s+AArch64')) {
    if (-not ($elfHeader -match $requiredHeader)) { throw "Unexpected Android engine ELF header: $requiredHeader" }
}

$expectedExports = @(
    'ccore_ovpn3_abi_version',
    'ccore_ovpn3_client_create',
    'ccore_ovpn3_client_destroy',
    'ccore_ovpn3_client_last_error',
    'ccore_ovpn3_client_read_packet',
    'ccore_ovpn3_client_ready',
    'ccore_ovpn3_client_reconnect',
    'ccore_ovpn3_client_start',
    'ccore_ovpn3_client_stop',
    'ccore_ovpn3_client_write_packet',
    'ccore_ovpn3_eval_profile',
    'ccore_ovpn3_license',
    'ccore_ovpn3_version'
)
$exportRows = @(& $nm -D --defined-only --format=posix $outputLibrary)
if ($LASTEXITCODE -ne 0) { throw "llvm-nm failed with exit code $LASTEXITCODE" }
$actualExports = @($exportRows | ForEach-Object {
    if ($_ -match '^(ccore_ovpn3_[^@\s]+)@@CCORE_OPENVPN3_2\s') { $Matches[1] }
} | Sort-Object -Unique)
$exportDifference = @(Compare-Object $expectedExports $actualExports)
if ($exportDifference.Count -ne 0) {
    throw "Unexpected C ABI exports after stripping: $($exportDifference | Out-String)"
}

$header = Get-Content -Raw -LiteralPath (Join-Path $repositoryRoot 'ccore\include\ccore_openvpn3.h')
if ($header -notmatch '(?m)^#define CCORE_OVPN3_ABI_VERSION 2$') {
    throw 'The engine header does not declare CCORE_OVPN3_ABI_VERSION 2.'
}
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'ccore\include\ccore_openvpn3.h') -Destination (Join-Path $outputPath 'ccore_openvpn3.h') -Force

$outputItem = Get-Item -LiteralPath $outputLibrary
$sourceCommit = (& git -C $repositoryRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Unable to resolve the source commit.' }
$sourceTree = (& git -C $repositoryRoot rev-parse 'HEAD^{tree}').Trim()
if ($LASTEXITCODE -ne 0) { throw 'Unable to resolve the source tree.' }
$forkCommit = if ([string]::IsNullOrWhiteSpace($ReleaseCommit)) {
    $sourceCommit
} else {
    $ReleaseCommit.Trim().ToLowerInvariant()
}
if ($forkCommit -notmatch '^[0-9a-f]{40}$') { throw "Invalid release commit: $forkCommit" }
[PSCustomObject]@{
    Library = $outputLibrary
    Bytes = $outputItem.Length
    UnstrippedBytes = $library.Length
    Sha256 = (Get-FileHash -LiteralPath $outputLibrary -Algorithm SHA256).Hash
    ABI = 'arm64-v8a'
    CABI = 2
    ExportCount = $actualExports.Count
    AndroidAPI = 23
    OpenVPN3UpstreamCommit = '1512c16622288f3c01da09d3278ac61a86dca26d'
    ForkCommit = $forkCommit
    SourceCommit = $sourceCommit
    SourceTree = $sourceTree
    License = 'MPL-2.0'
}
