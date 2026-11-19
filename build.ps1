param(
    [switch]$Clean,
    [switch]$NoDeploy,
    [string]$MsysBin = "C:\msys64\ucrt64\bin"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $MsysBin)) {
    throw "MSYS2 UCRT64 bin directory not found at '$MsysBin'. Pass -MsysBin <path> or install MSYS2 UCRT64 with mingw-w64-ucrt-x86_64-qt6-base."
}

$env:PATH = "$MsysBin;$env:PATH"
$env:PKG_CONFIG_PATH = Join-Path (Split-Path $MsysBin -Parent) "lib\pkgconfig"

$root = $PSScriptRoot
Push-Location $root
try {
    if ($Clean -and (Test-Path "build")) {
        Remove-Item -Recurse -Force "build"
    }
    if (-not (Test-Path "build")) {
        New-Item -ItemType Directory -Path "build" | Out-Null
    }

    $jobs = [Math]::Max(1, [Environment]::ProcessorCount)
    Write-Host "Building Sava with MSYS2 UCRT64 ($jobs jobs)..."
    & (Join-Path $MsysBin "mingw32-make.exe") -j $jobs CXX=g++ PKG_CONFIG=pkg-config WINDRES=windres
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    $exe = Join-Path $root "build\sava.exe"
    if (-not (Test-Path $exe)) {
        throw "Expected executable not found at $exe"
    }

    if (-not $NoDeploy) {
        Write-Host "Deploying Qt 6 plugins and runtime DLLs into build\..."
        $windeployqt = Join-Path $MsysBin "windeployqt6.exe"
        & $windeployqt --no-translations --no-compiler-runtime --no-opengl-sw --dir (Join-Path $root "build") $exe
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }

        # Copy all transitive MSYS2 UCRT64 DLL dependencies required by sava.exe and deployed Qt plugins
        # so build\sava.exe runs standalone without MSYS2 on PATH.
        $objdump = Join-Path $MsysBin "objdump.exe"
        $buildDir = Join-Path $root "build"
        $queue = [System.Collections.Generic.Queue[string]]::new()
        $visited = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)

        $queue.Enqueue($exe)
        [void]$visited.Add($exe)
        Get-ChildItem -Path $buildDir -Recurse -Include *.dll | ForEach-Object {
            if ($visited.Add($_.FullName)) {
                $queue.Enqueue($_.FullName)
            }
        }

        $copied = 0
        while ($queue.Count -gt 0) {
            $current = $queue.Dequeue()
            $imports = & $objdump -p $current 2>$null | ForEach-Object {
                if ($_ -match '^\s*DLL Name:\s+(\S+)') { $Matches[1] }
            }
            foreach ($dll in $imports) {
                $src = Join-Path $MsysBin $dll
                if (-not (Test-Path $src)) {
                    continue
                }
                $dst = Join-Path $buildDir $dll
                if (-not (Test-Path $dst)) {
                    Copy-Item -Path $src -Destination $dst -Force
                    $copied++
                }
                if ($visited.Add($dst)) {
                    $queue.Enqueue($dst)
                }
            }
        }
        Write-Host "Deployed standalone bundle to $buildDir ($copied runtime DLLs copied)."
    }

    Write-Host "Done: $exe"
} finally {
    Pop-Location
}
