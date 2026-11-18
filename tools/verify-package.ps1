param([string]$PackageDirectory = $PSScriptRoot, [switch]$Scale)
$ErrorActionPreference = 'Stop'
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path
$manifest = Get-Content -LiteralPath "$package\manifest.json" -Raw | ConvertFrom-Json
foreach ($file in $manifest) {
    $path = [IO.Path]::GetFullPath((Join-Path $package $file.path))
    if (!$path.StartsWith($package + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid manifest path.' }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.sha256) { throw "Package checksum mismatch: $($file.path)" }
}
$names = @('PATH','QT_PLUGIN_PATH','QML2_IMPORT_PATH','QML_IMPORT_PATH','QT_ROOT_DIR','QT_QPA_PLATFORM_PLUGIN_PATH','QT_QPA_FONTDIR','QT_QUICK_BACKEND','QT_FORCE_STDERR_LOGGING')
$previous = @{}
foreach ($name in $names) { $previous[$name] = [Environment]::GetEnvironmentVariable($name,'Process'); [Environment]::SetEnvironmentVariable($name,$null,'Process') }
$logs = Join-Path ([IO.Path]::GetTempPath()) ('FileSearcher-check-' + [guid]::NewGuid().ToString())
New-Item -ItemType Directory -Path $logs | Out-Null
try {
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    $env:QT_QPA_FONTDIR = "$env:SystemRoot\Fonts"
    $env:QT_QUICK_BACKEND = 'software'; $env:QT_FORCE_STDERR_LOGGING = '1'
    $arguments = if ($Scale) { '--scale' } else { '--check' }
    $process = Start-Process -FilePath "$package\package_smoke.exe" -WorkingDirectory $logs -ArgumentList $arguments -WindowStyle Hidden -PassThru -Wait -RedirectStandardOutput "$logs\workflow.json" -RedirectStandardError "$logs\workflow-errors.log"
    Get-Content -LiteralPath "$logs\workflow.json"
    if ($process.ExitCode -ne 0) { throw "Packaged workflow failed ($($process.ExitCode)). See $logs" }
    $process = Start-Process -FilePath "$package\filesearcher.exe" -WorkingDirectory $logs -ArgumentList '--smoke-test -platform offscreen' -WindowStyle Hidden -PassThru -Wait -RedirectStandardError "$logs\ui-errors.log"
    if ($process.ExitCode -ne 0) { throw "Packaged UI failed ($($process.ExitCode)). See $logs" }
    Write-Output "Package checks passed. Logs: $logs"
} finally { foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name,$previous[$name],'Process') } }
