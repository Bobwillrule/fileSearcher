$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repoRoot
try {
    if (!(Test-Path .tools/python/Scripts/python.exe)) {
        python -m venv .tools/python
        if ($LASTEXITCODE -ne 0) { throw 'Python environment creation failed' }
    }
    & ./.tools/python/Scripts/python.exe -m pip install -r tools/requirements-phase1.txt aqtinstall==3.3.0
    if ($LASTEXITCODE -ne 0) { throw 'Reference tool installation failed' }
    & ./.tools/python/Scripts/python.exe -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O .tools/qt --modules qtpdf --archives qtpdf
    if ($LASTEXITCODE -ne 0) { throw 'Qt PDF installation failed' }
    $archive = Join-Path $repoRoot '.tools/onnxruntime.zip'
    if (!(Test-Path -LiteralPath $archive)) {
        Invoke-WebRequest 'https://github.com/microsoft/onnxruntime/releases/download/v1.23.2/onnxruntime-win-x64-1.23.2.zip' -OutFile $archive
    }
    if ((Get-FileHash -LiteralPath $archive).Hash -ne '0B38DF9AF21834E41E73D602D90DB5CB06DBD1CA618948B8F1D66D607AC9F3CD') { throw 'ONNX Runtime checksum mismatch' }
    Expand-Archive -LiteralPath $archive -DestinationPath .tools/onnxruntime -Force
    & ./.tools/python/Scripts/python.exe tools/prepare_model.py
    if ($LASTEXITCODE -ne 0) { throw 'Model preparation failed' }
} finally { Pop-Location }
