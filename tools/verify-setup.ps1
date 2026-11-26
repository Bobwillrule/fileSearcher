param([Parameter(Mandatory)][string]$SetupExe)
$ErrorActionPreference = 'Stop'
$setup = (Resolve-Path -LiteralPath $SetupExe).Path
$key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{F3BD5762-DAD8-4581-9DF6-091C8B79DEF2}_is1'
if (Test-Path -LiteralPath $key) { throw 'An installed copy exists. Use a separate test account.' }
$id = [guid]::NewGuid().ToString()
$fixture = Join-Path ([IO.Path]::GetTempPath()) "FileSearcher setup test $id"
$app = Join-Path $fixture 'application'
$state = Join-Path $fixture 'state'
$group = "FileSearcher test $id"
$shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) "$group\FileSearcher.lnk"
$options = @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/DIR="'+$app+'"'),('/GROUP="'+$group+'"'))
function RunCheck($exe,$options) {
    $proc = Start-Process -FilePath $exe -ArgumentList $options -WorkingDirectory $fixture -WindowStyle Hidden -Wait -PassThru
    if ($proc.ExitCode -ne 0) { throw "Exit code $($proc.ExitCode) running $exe" }
}
New-Item -ItemType Directory -Path $fixture | Out-Null
$doc = Join-Path $fixture 'original.txt'
'Synthetic original document' | Set-Content -LiteralPath $doc
$before = (Get-FileHash -LiteralPath $doc).Hash
try {
    RunCheck $setup $options
    if (!(Test-Path -LiteralPath $key) -or !(Test-Path -LiteralPath $shortcut)) { throw 'Setup registration or shortcut missing.' }
    $target = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcut).TargetPath
    if ($target -ne "$app\filesearcher.exe") { throw 'Shortcut target mismatch.' }
    RunCheck $target @('--smoke-test','--data-dir',('"'+$state+'"'),'-platform','offscreen')
    if (!(Test-Path -LiteralPath "$state\release-state.json")) { throw 'First launch failed to initialize state.' }
    'Foreign file' | Set-Content -LiteralPath "$app\unowned.txt"
    RunCheck $setup $options
    & "$app\verify-package.ps1" -PackageDirectory $app
} finally {
    if (Test-Path -LiteralPath "$app\unins000.exe") {
        RunCheck "$app\unins000.exe" @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART')
    }
}
if ((Test-Path -LiteralPath $key) -or (Test-Path -LiteralPath $shortcut) -or (Test-Path -LiteralPath "$app\filesearcher.exe")) { throw 'Uninstall did not remove application registration or binaries.' }
if (!(Test-Path -LiteralPath "$app\unowned.txt") -or !(Test-Path -LiteralPath "$state\metadata.sqlite") -or (Get-FileHash -LiteralPath $doc).Hash -ne $before) { throw 'Uninstall preservation failed.' }
Write-Output "Install, Start menu launch target, reinstall, packaged workflow, uninstall and preservation checks passed."
