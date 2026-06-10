# Windows driver for the genamba testbench
$env:Path  = "C:\msys64\mingw64\bin;C:\msys64\usr\bin;" + $env:Path
$env:LC_ALL = "C"

Push-Location $PSScriptRoot
try {
    & make genamba PYTHON3=python3
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $exe = Join-Path $PSScriptRoot "obj_genamba\Vtb_genamba.exe"
    if (-not (Test-Path $exe)) {
        Write-Error "Build did not produce $exe"
        exit 1
    }

    # T1: no scenario plusarg yet (cmodel_init not called); T3 adds it.
    & $exe
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
