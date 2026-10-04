param([string]$Compiler = "clang++")
$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$buildRoot = Join-Path $repoRoot "build"
New-Item -ItemType Directory -Force $buildRoot | Out-Null
foreach ($target in @("test_core", "test_decoder", "bench_ring")) {
    & $Compiler -std=c++20 -O3 -Wall -Wextra -I (Join-Path $repoRoot "feed_handler/include") (Join-Path $repoRoot "feed_handler/tests/$target.cpp") -o (Join-Path $buildRoot "$target.exe")
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $target" }
}
foreach ($target in @("test_core", "test_decoder")) {
    & (Join-Path $buildRoot "$target.exe")
    if ($LASTEXITCODE -ne 0) { throw "Test failed: $target" }
}
Write-Host "Core checks passed. Run build/bench_ring.exe handoff or saturated."
