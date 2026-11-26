param(
    [Parameter(Mandatory)][string]$PortableZip,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$Compiler = (Join-Path $PSScriptRoot '..\.tools\inno\ISCC.exe')
)
$ErrorActionPreference = 'Stop'
$zip = (Resolve-Path -LiteralPath $PortableZip).Path
$compilerPath = (Resolve-Path -LiteralPath $Compiler).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Choose a new output directory.' }
$stage = Join-Path ([IO.Path]::GetTempPath()) ('FileSearcher-pack-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $stage | Out-Null
try {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, $stage)
    $roots = @(Get-ChildItem -LiteralPath $stage -Directory)
    if ($roots.Count -ne 1) { throw 'Expected a single portable package folder.' }
    $package = $roots[0].FullName
    $manifest = Get-Content -Raw -LiteralPath "$package\manifest.json" | ConvertFrom-Json
    $expected = @{'manifest.json' = $true}
    foreach ($row in $manifest) {
        $path = [IO.Path]::GetFullPath((Join-Path $package $row.path))
        if (!$path.StartsWith($package + '\',[StringComparison]::OrdinalIgnoreCase) -or $expected.ContainsKey($row.path)) { throw 'Invalid package member.' }
        if ((Get-FileHash -LiteralPath $path).Hash -ne $row.sha256) { throw "Invalid checksum for $($row.path)" }
        $expected[$row.path]=$true
    }
    foreach ($file in Get-ChildItem -LiteralPath $package -File -Recurse -Force) {
        if (!$expected.ContainsKey($file.FullName.Substring($package.Length + 1).Replace('\','/'))) { throw "Unexpected package member: $($file.Name)" }
    }
    $version = (Get-Content -Raw -LiteralPath "$package\release-metadata.json" | ConvertFrom-Json).version
    if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid release version.' }
    $runtime = (Get-Item -LiteralPath "$package\vc_redist.x64.exe").VersionInfo.ProductVersion
    if ($runtime -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw 'Invalid runtime version.' }
    New-Item -ItemType Directory -Path $output | Out-Null
    & $compilerPath /Qp "/DPackageDir=$package" "/DAppVersion=$version" "/DRuntimeVersion=$runtime" "/O$output" "$PSScriptRoot\FileSearcher.iss"
    if ($LASTEXITCODE -ne 0) { throw 'Setup compilation failed.' }
    $portable = "FileSearcher-$version-Portable-x64.zip"
    Copy-Item -LiteralPath $zip -Destination (Join-Path $output $portable)
    $checksums = foreach ($name in 'FileSearcher-Setup.exe',$portable) {
        $digest = (Get-FileHash -LiteralPath (Join-Path $output $name)).Hash.ToLowerInvariant()
        "$digest  $name"
    }
    $checksums | Out-File -LiteralPath (Join-Path $output 'SHA256SUMS.txt') -Encoding ascii
} finally {
    $resolved = (Resolve-Path -LiteralPath $stage).Path
    if ($resolved -eq $stage -and (Split-Path $resolved -Leaf) -like 'FileSearcher-pack-*') {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
