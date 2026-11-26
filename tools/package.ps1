param([string]$OutputDirectory, [switch]$Zip)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'build\windows-release'
$qt = Join-Path $repo '.tools\qt\6.8.3\msvc2022_64'
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repo ('build\FileSearcher-validation-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$destination = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $destination) { throw 'Choose a new package folder; existing folders are never overwritten.' }
$sourceDestination = "$destination-dependency-sources"
if (Test-Path -LiteralPath $sourceDestination) { throw 'Choose a new source companion folder; existing folders are never overwritten.' }
$inventoryPath = "$repo\.tools\release-notices\source-inventory.json"
if (!(Test-Path -LiteralPath $inventoryPath)) { throw 'Run tools/prepare_release_sources.py before creating a Phase 10 package.' }
$sourceInventory = @(Get-Content -LiteralPath $inventoryPath -Raw | ConvertFrom-Json)
foreach ($entry in $sourceInventory) {
    if (!$entry.sdk_revision_matches) { throw 'Dependency source revision has not been verified.' }
    if ((Get-FileHash -LiteralPath "$repo\.tools\release-sources\$($entry.archive)").Hash -ne $entry.sha256) { throw 'Dependency source checksum mismatch.' }
}
foreach ($name in @('filesearcher.exe','document_worker.exe','embedding_worker.exe','package_smoke.exe','state_backup.exe','onnxruntime.dll','zip.dll','z.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $build $name))) { throw "Missing $name. Build windows-release with -ModelTests first." }
}
New-Item -ItemType Directory -Path $destination | Out-Null
foreach ($name in @('filesearcher.exe','document_worker.exe','embedding_worker.exe','package_smoke.exe','state_backup.exe','onnxruntime.dll','zip.dll','z.dll')) {
    Copy-Item -LiteralPath (Join-Path $build $name) -Destination $destination
}
$savedPath = $env:PATH
try {
    $env:PATH = "$qt\bin;$env:PATH"
    & "$qt\bin\windeployqt.exe" --verbose 0 --release --no-compiler-runtime --no-translations --qmldir "$repo\src\app" --dir $destination "$destination\filesearcher.exe" "$destination\document_worker.exe" "$destination\package_smoke.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Qt runtime deployment failed.' }
} finally { $env:PATH = $savedPath }
# Document workers deliberately use the offscreen platform; windeployqt's default
# Windows plugin selection does not include it.
Copy-Item -LiteralPath "$qt\plugins\platforms\qoffscreen.dll" -Destination "$destination\platforms"
@"
[Paths]
Prefix=.
Plugins=.
QmlImports=qml
"@ | Set-Content -LiteralPath "$destination\qt.conf" -Encoding utf8
Copy-Item -LiteralPath "$build\models" -Destination $destination -Recurse
$licenses = New-Item -ItemType Directory -Path "$destination\licenses"
Copy-Item -LiteralPath "$repo\docs\licenses\BGE-MIT.txt" -Destination $licenses
Copy-Item -LiteralPath "$repo\docs\PACKAGE_VALIDATION.md" -Destination "$destination\READ-ME-FIRST.md"
Copy-Item -LiteralPath "$qt\sbom" -Destination "$licenses\qt-sbom" -Recurse
New-Item -ItemType Directory -Path "$licenses\qt-source-notices" | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
foreach ($entry in $sourceInventory) {
    [IO.Compression.ZipFile]::CreateFromDirectory("$repo\.tools\release-notices\$($entry.notice_directory)","$licenses\qt-source-notices\$($entry.module).zip")
}
Copy-Item -LiteralPath $inventoryPath -Destination "$licenses\source-inventory.json"
foreach ($name in @('LGPL-3.0-only.txt','GPL-3.0-only.txt')) {
    Copy-Item -LiteralPath "$repo\.tools\release-notices\qtbase\LICENSES\$name" -Destination $licenses
}
Copy-Item -LiteralPath "$repo\LICENSE","$repo\THIRD_PARTY_NOTICES.md" -Destination $destination
foreach ($name in @('hnswlib','libzip','zlib')) {
    Copy-Item -LiteralPath "$build\vcpkg_installed\x64-windows\share\$name\copyright" -Destination "$licenses\$name.txt"
}
$ort = "$repo\.tools\onnxruntime\onnxruntime-win-x64-1.23.2"
Copy-Item -LiteralPath "$ort\LICENSE" -Destination "$licenses\ONNX-Runtime-LICENSE.txt"
Copy-Item -LiteralPath "$ort\ThirdPartyNotices.txt" -Destination "$licenses\ONNX-Runtime-ThirdPartyNotices.txt"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$redist = Get-ChildItem -LiteralPath "$vs\VC\Redist\MSVC" -Directory | Where-Object Name -Match '^14\.' | Sort-Object Name -Descending | Select-Object -First 1
Copy-Item -LiteralPath "$($redist.FullName)\vc_redist.x64.exe" -Destination $destination
$fixtures = New-Item -ItemType Directory -Path "$destination\smoke-fixtures"
Copy-Item -LiteralPath "$repo\build\fixtures\text.pdf" -Destination "$fixtures\reference.pdf"
Copy-Item -LiteralPath "$repo\build\fixtures\notes.docx" -Destination "$fixtures\reference.docx"
Copy-Item -LiteralPath "$PSScriptRoot\verify-package.ps1" -Destination $destination
Copy-Item -LiteralPath "$PSScriptRoot\verify-release-lifecycle.ps1","$PSScriptRoot\verify-clean-machine.ps1" -Destination $destination
$source = New-Item -ItemType Directory -Path "$destination\source"
foreach ($name in @('src','tests','tools','docs','CMakeLists.txt','CMakePresets.json','vcpkg.json','README.md','IMPLEMENTATION_PLAN.md','LICENSE','THIRD_PARTY_NOTICES.md')) {
    Copy-Item -LiteralPath (Join-Path $repo $name) -Destination $source -Recurse
}
$commit = & git -C $repo rev-parse HEAD
$dirty = [bool](& git -C $repo status --porcelain)
[pscustomobject]@{ version='0.2.0'; channel='local-validation'; source_base_commit=$commit; source_has_uncommitted_changes=$dirty; qt='6.8.3'; onnxruntime='1.23.2'; source_companion=(Split-Path -Leaf $sourceDestination); independent_machine_test='pending'; built_with_model_tests=$true } | ConvertTo-Json | Set-Content -LiteralPath "$destination\release-metadata.json" -Encoding utf8
New-Item -ItemType Directory -Path $sourceDestination | Out-Null
foreach ($entry in $sourceInventory) { Copy-Item -LiteralPath "$repo\.tools\release-sources\$($entry.archive)" -Destination $sourceDestination }
Copy-Item -LiteralPath $inventoryPath -Destination "$sourceDestination\source-inventory.json"
Copy-Item -LiteralPath "$repo\THIRD_PARTY_NOTICES.md" -Destination "$sourceDestination\READ-ME.md"
Get-ChildItem -LiteralPath $destination -File -Recurse | Sort-Object FullName | ForEach-Object {
    [pscustomobject]@{ path=$_.FullName.Substring($destination.Length+1).Replace('\','/'); bytes=$_.Length; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
} | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath "$destination\manifest.json" -Encoding utf8
if ($Zip) {
    [IO.Compression.ZipFile]::CreateFromDirectory($destination,"$destination.zip",[IO.Compression.CompressionLevel]::Optimal,$true)
    [IO.Compression.ZipFile]::CreateFromDirectory($sourceDestination,"$sourceDestination.zip",[IO.Compression.CompressionLevel]::NoCompression,$true)
}
Write-Output "Validation package: $destination"
