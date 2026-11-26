param([string]$PackageDirectory = $PSScriptRoot, [switch]$Scale, [switch]$ObserveTcp)
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
    $process = Start-Process -FilePath "$package\package_smoke.exe" -WorkingDirectory $logs -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput "$logs\workflow.json" -RedirectStandardError "$logs\workflow-errors.log"
    $samples=0; $connections=0; $observed=@{}
    if($ObserveTcp) {
        while(!$process.HasExited) {
            $ids=@{}
            foreach($worker in @(Get-Process -Name package_smoke,embedding_worker,document_worker -ErrorAction SilentlyContinue)) {
                if($worker.Path -and [IO.Path]::GetDirectoryName($worker.Path) -eq $package) { $ids[[string]$worker.Id]=$true; $observed[$worker.ProcessName]=$true }
            }
            $rows=& "$env:SystemRoot\System32\netstat.exe" -ano -p tcp
            if($LASTEXITCODE -ne 0) { throw 'TCP sampling failed.' }
            foreach($line in $rows) { $fields=$line.Trim() -split '\s+'; if($fields.Length -eq 5 -and $fields[0] -eq 'TCP' -and $ids.ContainsKey($fields[4]) -and $fields[3] -ne 'LISTENING') { $connections++ } }
            $samples++; Start-Sleep -Milliseconds 100
        }
        [pscustomobject]@{samples=$samples;observed_processes=@($observed.Keys);ipv4_tcp_connection_observations=$connections;scope='Sampled local synthetic workflow. Short-lived traffic, IPv6 and UDP are outside this measurement.'} | ConvertTo-Json | Set-Content -LiteralPath "$logs\tcp.json" -Encoding utf8
    }
    $process.WaitForExit()
    Get-Content -LiteralPath "$logs\workflow.json"
    if ($process.ExitCode -ne 0) { throw "Packaged workflow failed ($($process.ExitCode)). See $logs" }
    $process = Start-Process -FilePath "$package\filesearcher.exe" -WorkingDirectory $logs -ArgumentList '--smoke-test -platform offscreen' -WindowStyle Hidden -PassThru -Wait -RedirectStandardError "$logs\ui-errors.log"
    if ($process.ExitCode -ne 0) { throw "Packaged UI failed ($($process.ExitCode)). See $logs" }
    Write-Output "Package checks passed. Logs: $logs"
} finally { foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name,$previous[$name],'Process') } }
