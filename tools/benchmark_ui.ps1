$ErrorActionPreference = 'Stop'
$root = (Resolve-Path '.').Path
$baselineCommit = '18e2110f5d279a2e4b892c218ffce6dbd648420f'
$baselineSource = Join-Path $root 'build/ui-baseline-src'
$baselineBuild = Join-Path $root 'build/ui-baseline-build'

function Run-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE) { throw "$Program failed ($LASTEXITCODE)" }
}
function Measure-UI {
    param([string]$Executable, [string]$Report)
    $result = Join-Path $root 'nco-self-test/ui-benchmark.txt'
    Remove-Item $result -Force -ErrorAction SilentlyContinue
    $process = Start-Process $Executable -ArgumentList '--ui-smoke-test' -Wait -PassThru
    if ($process.ExitCode) { throw "UI benchmark failed ($($process.ExitCode))" }
    if (!(Test-Path $result)) { throw 'UI benchmark did not write a new result' }
    Copy-Item $result $Report -Force
    Write-Output $Report
    Get-Content $Report
}

Run-Checked -Program 'git' -Arguments @('fetch', '--depth=1', 'origin', $baselineCommit)
Run-Checked -Program 'git' -Arguments @('worktree', 'add', '--detach', $baselineSource, $baselineCommit)
Copy-Item 'assets/nco.ico' "$baselineSource/assets/nco.ico" -Force
Copy-Item 'tools/ui_benchmark.cpp' "$baselineSource/src/ui_test.cpp" -Force
Run-Checked -Program 'cmake' -Arguments @('-S', $baselineSource, '-B', $baselineBuild, '-A', 'x64',
    "-DCMAKE_TOOLCHAIN_FILE=$root/.vcpkg/scripts/buildsystems/vcpkg.cmake",
    '-DVCPKG_TARGET_TRIPLET=x64-windows-nco', "-DVCPKG_OVERLAY_TRIPLETS=$root/triplets",
    "-DVCPKG_OVERLAY_PORTS=$root/build/vcpkg-overlay", "-DVCPKG_INSTALLED_DIR=$root/vcpkg_installed")
Run-Checked -Program 'cmake' -Arguments @('--build', $baselineBuild, '--config', 'Release', '--parallel')
Measure-UI -Executable "$baselineBuild/Release/NCO.exe" -Report 'build/ui-baseline.txt'

$original = [IO.File]::ReadAllBytes("$root/src/ui_test.cpp")
try {
    Copy-Item 'tools/ui_benchmark.cpp' 'src/ui_test.cpp' -Force
    # Copy-Item preserves the source timestamp; force MSBuild to compile this replacement.
    (Get-Item 'src/ui_test.cpp').LastWriteTimeUtc = [DateTime]::UtcNow
    Run-Checked -Program 'cmake' -Arguments @('--build', 'build', '--config', 'Release', '--parallel')
    Measure-UI -Executable "$root/build/Release/NCO.exe" -Report 'build/ui-current.txt'
} finally {
    [IO.File]::WriteAllBytes("$root/src/ui_test.cpp", $original)
    # Restore the full regression suite before testing/packaging the actual app.
    Run-Checked -Program 'cmake' -Arguments @('--build', 'build', '--config', 'Release', '--parallel')
}
