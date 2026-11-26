param([string]$PackageDirectory = $PSScriptRoot, [string]$ReportDirectory, [string]$PreviousPackageDirectory)
$ErrorActionPreference = 'Stop'
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path
if (!$ReportDirectory) { $ReportDirectory = Join-Path ([IO.Path]::GetTempPath()) ('FileSearcher-lifecycle-' + [guid]::NewGuid().ToString()) }
$fixture = [IO.Path]::GetFullPath($ReportDirectory)
if (Test-Path -LiteralPath $fixture) { throw 'Choose a new lifecycle report folder.' }
New-Item -ItemType Directory -Path $fixture | Out-Null
$installed = Join-Path $fixture 'installed-app'
$documents = Join-Path $fixture 'documents'
$state = Join-Path $fixture 'state'
New-Item -ItemType Directory -Path $installed,$documents | Out-Null
$original = Join-Path $documents 'keep-original.txt'
'Synthetic original outside the portable installation.' | Set-Content -LiteralPath $original -Encoding utf8
$before = (Get-FileHash -LiteralPath $original).Hash
# Copy only runtime files needed by this lifecycle test; originals and state are siblings.
foreach ($entry in Get-ChildItem -LiteralPath $package) {
    if ($entry.Name -notin @('source','licenses')) { Copy-Item -LiteralPath $entry.FullName -Destination $installed -Recurse }
}
$savedPath=$env:PATH; $savedBackend=$env:QT_QUICK_BACKEND
try {
    $env:PATH="$env:SystemRoot\System32;$env:SystemRoot"; $env:QT_QUICK_BACKEND='software'
    $arguments=@('--smoke-test','--data-dir',('"'+$state+'"'),'-platform','offscreen')
    $process=Start-Process -FilePath "$installed\filesearcher.exe" -ArgumentList $arguments -WorkingDirectory $fixture -WindowStyle Hidden -Wait -PassThru -RedirectStandardError "$fixture\install-errors.log"
    if($process.ExitCode -ne 0 -or !(Test-Path -LiteralPath "$state\release-state.json")) { throw 'Fresh portable launch failed.' }
    if($PreviousPackageDirectory) {
        $previous=(Resolve-Path -LiteralPath $PreviousPackageDirectory).Path
        $state=Join-Path $fixture 'previous-state'
        $arguments=@('--smoke-test','--data-dir',('"'+$state+'"'),'-platform','offscreen')
        $process=Start-Process -FilePath "$previous\filesearcher.exe" -ArgumentList $arguments -WorkingDirectory $fixture -WindowStyle Hidden -Wait -PassThru
        if($process.ExitCode -ne 0 -or !(Test-Path -LiteralPath "$state\metadata.sqlite")) { throw 'Previous package failed to create its state.' }
    } else {
        '{"format":1,"release":"0.1.0"}' | Set-Content -LiteralPath "$state\release-state.json" -Encoding utf8
    }
    $process=Start-Process -FilePath "$installed\filesearcher.exe" -ArgumentList $arguments -WorkingDirectory $fixture -WindowStyle Hidden -Wait -PassThru -RedirectStandardError "$fixture\upgrade-errors.log"
    if($process.ExitCode -ne 0) { throw 'Upgrade launch failed.' }
    $backup = @(Get-ChildItem -LiteralPath "$state\backups" -Directory)
    if($backup.Count -ne 1) { throw 'Expected one pre-upgrade snapshot.' }
    & "$installed\state_backup.exe" --verify $backup[0].FullName | Set-Content -LiteralPath "$fixture\backup-verification.json" -Encoding utf8
    if($LASTEXITCODE -ne 0) { throw 'Packaged backup verification failed.' }
    & "$installed\state_backup.exe" --restore $backup[0].FullName --destination "$fixture\restored-state"
    if($LASTEXITCODE -ne 0) { throw 'Packaged recovery failed.' }
    # Portable uninstall is scoped to the exact synthetic installation, never data/docs.
    $resolved = (Resolve-Path -LiteralPath $installed).Path
    if($resolved -ne (Join-Path $fixture 'installed-app') -or !$resolved.StartsWith($fixture+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected uninstall test path.' }
    if((Get-Item -LiteralPath $resolved).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Linked installation cannot be removed by this test.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
    if((Get-FileHash -LiteralPath $original).Hash -ne $before -or !(Test-Path -LiteralPath "$state\organization.sqlite") -or !(Test-Path -LiteralPath "$fixture\restored-state\metadata.sqlite")) { throw 'Uninstall/restore preservation check failed.' }
    [pscustomobject]@{fresh_portable_launch=$true;previous_binary_exercised=[bool]$PreviousPackageDirectory;upgrade_backup_verified=$true;restore_to_new_directory=$true;portable_uninstall_preserves_documents_and_state=$true} | ConvertTo-Json | Set-Content -LiteralPath "$fixture\lifecycle.json" -Encoding utf8
    Get-Content -LiteralPath "$fixture\lifecycle.json"
    Write-Output "Lifecycle checks passed. Logs: $fixture"
} finally { $env:PATH=$savedPath; $env:QT_QUICK_BACKEND=$savedBackend }
