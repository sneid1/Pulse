$ErrorActionPreference = "Stop"

# Configure + build the Pulse engine (MSVC + Ninja, C++20).
# NOTE: will not link until M0 lands (engine core + PulseGame submission rewrite +
# main.cpp rewrite). See SEED_NOTES.md.

$root = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }

Push-Location $root
try {
    cmake -S . -B build -G Ninja
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
    cmake --build build
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}
finally {
    Pop-Location
}
