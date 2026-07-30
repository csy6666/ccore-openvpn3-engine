#requires -Version 5.1
# SPDX-License-Identifier: MPL-2.0

[CmdletBinding()]
param(
    [string]$Output = 'dist\compliance\ccore-openvpn3-mpl-compliance.zip',
    [string]$BuildDirectory = 'out\android-arm64-v8a-api23-ndk28',
    [string]$EngineLibrary = 'dist\android\arm64-v8a\libccore_openvpn3.so',
    [string]$AndroidNdk = "$env:LOCALAPPDATA\Android\Sdk\ndk\28.1.13356709"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$utf8NoBom = New-Object Text.UTF8Encoding($false)
$pinnedCommit = '1512c16622288f3c01da09d3278ac61a86dca26d'

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Resolve-RepoPath([string]$Path, [string]$Root) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $Root $Path))
}

function Write-Json([string]$Path, $Value) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 8) + [Environment]::NewLine, $utf8NoBom)
}

function New-DeterministicZip([string]$SourceDirectory, [string]$DestinationPath, [DateTimeOffset]$Timestamp) {
    $sourceRoot = [IO.Path]::GetFullPath($SourceDirectory).TrimEnd('\')
    $destination = [IO.Path]::GetFullPath($DestinationPath)
    if (Test-Path -LiteralPath $destination) { throw "Archive already exists: $destination" }
    $stream = [IO.File]::Open($destination, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try {
        $archive = New-Object IO.Compression.ZipArchive($stream, [IO.Compression.ZipArchiveMode]::Create, $false)
        try {
            foreach ($file in @(Get-ChildItem -LiteralPath $sourceRoot -Recurse -File | Sort-Object FullName)) {
                $relative = $file.FullName.Substring($sourceRoot.Length + 1).Replace('\', '/')
                $entry = $archive.CreateEntry($relative, [IO.Compression.CompressionLevel]::Optimal)
                $entry.LastWriteTime = $Timestamp
                $input = [IO.File]::OpenRead($file.FullName)
                $output = $entry.Open()
                try { $input.CopyTo($output) } finally { $output.Dispose(); $input.Dispose() }
            }
        } finally {
            $archive.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

$repositoryRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$outputPath = Resolve-RepoPath $Output $repositoryRoot
$buildRoot = Resolve-RepoPath $BuildDirectory $repositoryRoot
$enginePath = Resolve-RepoPath $EngineLibrary $repositoryRoot
$ndkRoot = [IO.Path]::GetFullPath($AndroidNdk)

if (Test-Path -LiteralPath $outputPath) { throw "Compliance bundle already exists: $outputPath" }
foreach ($required in @($buildRoot, $enginePath, $ndkRoot)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required compliance input is missing: $required" }
}

$head = (& git -C $repositoryRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Unable to resolve the fork commit.' }
& git -C $repositoryRoot merge-base --is-ancestor $pinnedCommit $head
if ($LASTEXITCODE -ne 0) { throw "OpenVPN 3 base commit $pinnedCommit is not an ancestor of $head" }
$sourceDate = [DateTimeOffset]::Parse((& git -C $repositoryRoot show -s --format=%cI $head).Trim()).ToUniversalTime()

$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$tempPrefix = $tempRoot.TrimEnd('\') + '\'
$workRoot = [IO.Path]::GetFullPath((Join-Path $tempRoot ('ccore-openvpn3-compliance-' + [Guid]::NewGuid().ToString('N'))))
if (-not ($workRoot + '\').StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Temporary path escaped the system temp directory: $workRoot"
}

try {
    $bundleRoot = Join-Path $workRoot 'bundle'
    $sourceRoot = Join-Path $workRoot 'overlay'
    $licenseRoot = Join-Path $bundleRoot 'third_party_licenses'
    New-Item -ItemType Directory -Path $bundleRoot, $sourceRoot, $licenseRoot -Force | Out-Null

    $upstreamArchive = Join-Path $bundleRoot "openvpn3-$pinnedCommit-upstream.zip"
    & git -C $repositoryRoot archive --format=zip --output=$upstreamArchive $pinnedCommit
    if ($LASTEXITCODE -ne 0) { throw 'git archive failed for the pinned OpenVPN 3 source.' }

    $committedChanges = @(& git -C $repositoryRoot diff --name-only --diff-filter=ACMRTUXB "$pinnedCommit..$head")
    if ($LASTEXITCODE -ne 0) { throw 'git diff failed while collecting the committed fork overlay.' }
    $workingChanges = @(& git -C $repositoryRoot diff --name-only --diff-filter=ACMRTUXB)
    if ($LASTEXITCODE -ne 0) { throw 'git diff failed while collecting the working fork overlay.' }
    $allowedUntracked = @(& git -C $repositoryRoot ls-files --others --exclude-standard | Where-Object {
        $_ -eq 'CCORE-MPL-BOUNDARY.md' -or
        $_ -eq 'ENGINE-THIRD-PARTY-NOTICES.md' -or
        $_ -like 'ccore/*' -or
        $_ -like 'deps/triplets/*' -or
        $_ -like 'deps/vcpkg_manifests/android/*' -or
        $_ -like 'scripts/*'
    })
    if ($LASTEXITCODE -ne 0) { throw 'git ls-files failed while collecting the fork overlay.' }
    $overlayFiles = @($committedChanges + $workingChanges + $allowedUntracked | Sort-Object -Unique)
    if ($overlayFiles.Count -eq 0) { throw 'The fork overlay is unexpectedly empty.' }

    $overlayManifestRows = @()
    foreach ($relative in $overlayFiles) {
        $source = Join-Path $repositoryRoot ($relative.Replace('/', '\'))
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Overlay source is missing: $relative" }
        $destination = Join-Path $sourceRoot ($relative.Replace('/', '\'))
        $parent = Split-Path -Parent $destination
        if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
        Copy-Item -LiteralPath $source -Destination $destination
        $item = Get-Item -LiteralPath $source
        $overlayManifestRows += [ordered]@{
            path = $relative.Replace('\', '/')
            bytes = $item.Length
            sha256 = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToUpperInvariant()
        }
    }
    Write-Json (Join-Path $sourceRoot 'CCORE-FORK-MANIFEST.json') ([ordered]@{
        schemaVersion = 1
        upstreamCommit = $pinnedCommit
        forkCommit = $head
        selectedLicense = 'MPL-2.0'
        files = $overlayManifestRows
    })
    [IO.File]::WriteAllText((Join-Path $sourceRoot 'SOURCE-ASSEMBLY.md'), @"
# Source assembly

1. Expand `openvpn3-$pinnedCommit-upstream.zip`.
2. Expand `ccore-openvpn3-fork-overlay.zip` over that directory, replacing files.
3. Follow `scripts/build-android-arm64.ps1` from the assembled tree.

`CCORE-FORK-MANIFEST.json` records the exact fork file hashes.
"@, $utf8NoBom)

    $overlayArchive = Join-Path $bundleRoot 'ccore-openvpn3-fork-overlay.zip'
    New-DeterministicZip $sourceRoot $overlayArchive $sourceDate

    foreach ($file in @('LICENSE.md', 'CCORE-MPL-BOUNDARY.md', 'ENGINE-THIRD-PARTY-NOTICES.md')) {
        Copy-Item -LiteralPath (Join-Path $repositoryRoot $file) -Destination (Join-Path $bundleRoot $file)
    }
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'LICENSES\MPL-2.0.txt') -Destination (Join-Path $bundleRoot 'MPL-2.0.txt')

    $shareRoot = Join-Path $buildRoot 'vcpkg_installed\arm64-android23\share'
    $dependencyRows = @()
    foreach ($dependency in @(
        @{ Name = 'asio'; Version = '1.36.0' },
        @{ Name = 'fmt'; Version = '12.1.0' },
        @{ Name = 'lz4'; Version = '1.10.0' },
        @{ Name = 'openssl'; Version = '3.6.2' }
    )) {
        $packageRoot = Join-Path $shareRoot $dependency.Name
        $destinationRoot = Join-Path $licenseRoot $dependency.Name
        New-Item -ItemType Directory -Path $destinationRoot -Force | Out-Null
        foreach ($name in @('copyright', 'vcpkg.spdx.json')) {
            $source = Join-Path $packageRoot $name
            if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing vcpkg compliance file: $source" }
            Copy-Item -LiteralPath $source -Destination (Join-Path $destinationRoot $name)
        }
        $dependencyRows += [ordered]@{ name = $dependency.Name; version = $dependency.Version }
    }

    $ndkLicenseRoot = Join-Path $licenseRoot 'android-ndk'
    New-Item -ItemType Directory -Path $ndkLicenseRoot -Force | Out-Null
    foreach ($notice in @(
        @{ Source = 'toolchains\llvm\prebuilt\windows-x86_64\NOTICE'; Name = 'LLVM-NOTICE' },
        @{ Source = 'toolchains\llvm\prebuilt\windows-x86_64\sysroot\NOTICE'; Name = 'SYSROOT-NOTICE' }
    )) {
        $source = Join-Path $ndkRoot $notice.Source
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing Android NDK notice: $source" }
        Copy-Item -LiteralPath $source -Destination (Join-Path $ndkLicenseRoot $notice.Name)
    }

    $engine = Get-Item -LiteralPath $enginePath
    Write-Json (Join-Path $bundleRoot 'ENGINE-BINARY-MANIFEST.json') ([ordered]@{
        schemaVersion = 1
        file = 'libccore_openvpn3.so'
        bytes = $engine.Length
        sha256 = (Get-FileHash -LiteralPath $enginePath -Algorithm SHA256).Hash.ToUpperInvariant()
        abi = 'arm64-v8a'
        androidApi = 23
        upstreamCommit = $pinnedCommit
        forkCommit = $head
        selectedLicense = 'MPL-2.0'
        upstreamSourceSha256 = (Get-FileHash -LiteralPath $upstreamArchive -Algorithm SHA256).Hash.ToUpperInvariant()
        forkOverlaySha256 = (Get-FileHash -LiteralPath $overlayArchive -Algorithm SHA256).Hash.ToUpperInvariant()
        androidNdk = '28.1.13356709'
        dependencies = $dependencyRows
    })

    $manifestRows = @(Get-ChildItem -LiteralPath $bundleRoot -Recurse -File | ForEach-Object {
        [ordered]@{
            path = $_.FullName.Substring($bundleRoot.Length + 1).Replace('\', '/')
            bytes = $_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToUpperInvariant()
        }
    } | Sort-Object path)
    Write-Json (Join-Path $bundleRoot 'MANIFEST.json') ([ordered]@{ schemaVersion = 1; files = $manifestRows })

    $candidate = Join-Path $workRoot 'compliance.zip'
    New-DeterministicZip $bundleRoot $candidate $sourceDate
    $outputDirectory = Split-Path -Parent $outputPath
    if (-not (Test-Path -LiteralPath $outputDirectory)) { New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null }
    Move-Item -LiteralPath $candidate -Destination $outputPath

    [PSCustomObject]@{
        Output = $outputPath
        Bytes = (Get-Item -LiteralPath $outputPath).Length
        Sha256 = (Get-FileHash -LiteralPath $outputPath -Algorithm SHA256).Hash.ToUpperInvariant()
        UpstreamCommit = $pinnedCommit
        ForkCommit = $head
        OverlayFileCount = $overlayFiles.Count
        EngineSha256 = (Get-FileHash -LiteralPath $enginePath -Algorithm SHA256).Hash.ToUpperInvariant()
    }
} finally {
    if (Test-Path -LiteralPath $workRoot) {
        $resolvedWorkRoot = [IO.Path]::GetFullPath($workRoot)
        if (-not ($resolvedWorkRoot + '\').StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to remove temporary path outside the system temp directory: $resolvedWorkRoot"
        }
        Remove-Item -LiteralPath $resolvedWorkRoot -Recurse -Force
    }
}
