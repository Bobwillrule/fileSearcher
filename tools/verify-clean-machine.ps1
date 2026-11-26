param([string]$PackageDirectory=$PSScriptRoot, [switch]$RequireOffline)
$ErrorActionPreference='Stop'
$package=(Resolve-Path -LiteralPath $PackageDirectory).Path
$active=@([Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces() | Where-Object { $_.OperationalStatus -eq 'Up' -and $_.NetworkInterfaceType -ne 'Loopback' })
if($RequireOffline -and $active.Count) { throw 'Active network interfaces detected. Disconnect networking in the test VM/PC, then rerun; this script does not alter host networking.' }
foreach($command in @('cl.exe','qmake.exe','python.exe')) {
    if(Get-Command $command -ErrorAction SilentlyContinue) { Write-Warning "$command is on PATH; record whether this is actually a clean test machine." }
}
& "$package\verify-package.ps1" -PackageDirectory $package
& "$package\verify-release-lifecycle.ps1" -PackageDirectory $package
$report=Join-Path ([IO.Path]::GetTempPath()) ('FileSearcher-clean-machine-'+[guid]::NewGuid().ToString()+'.json')
[pscustomobject]@{windows_version=[Environment]::OSVersion.VersionString;os_64bit=[Environment]::Is64BitOperatingSystem;active_nonloopback_interfaces=$active.Count;offline_requested=$RequireOffline.IsPresent;automated_package_checks_passed=$true;manual_dpi_fonts_keyboard_upgrade_checks='pending operator review';independent_environment='operator must attest'} | ConvertTo-Json | Set-Content -LiteralPath $report -Encoding utf8
Write-Output "Automated checks passed. Complete the manual checklist and retain: $report"
