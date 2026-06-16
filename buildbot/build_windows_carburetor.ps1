param(
    [string]$CarburetorRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) "..\carburetor"),
    [ValidateSet("x86", "x64")]
    [string]$Arch = "x64",
    [string]$BuildDir = "build\win-carburetor",
    [string]$Generator = "NMake Makefiles",
    [switch]$ApplyBreakpadPatches
)

$ErrorActionPreference = "Stop"

function Assert-Command([string]$Name) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Required command was not found in PATH: $Name"
    }
}

function Resolve-FirstPath([string[]]$Candidates) {
    foreach ($candidate in $Candidates) {
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }
    return $null
}

function Resolve-CarburetorDependency([string]$Root, [string]$Leaf) {
    $match = Get-ChildItem $Root -Directory -Filter "build*" -ErrorAction SilentlyContinue |
        Sort-Object Name |
        ForEach-Object {
            $candidate = Join-Path $_.FullName $Leaf
            if (Test-Path $candidate) {
                return (Resolve-Path $candidate).Path
            }
        } |
        Select-Object -First 1

    if (-not $match) {
        throw "Failed to locate carburetor dependency path: $Leaf"
    }

    return $match
}

Assert-Command cmake
Assert-Command git

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw "cl.exe was not found. Run this script from a Visual Studio Developer PowerShell."
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$carburetorRoot = (Resolve-Path $CarburetorRoot).Path
$buildRoot = Join-Path $repoRoot $BuildDir
$stageRoot = Join-Path $buildRoot ("stage-" + $Arch)
$cmakeBuildRoot = Join-Path $buildRoot ("cmake-" + $Arch)

Remove-Item -LiteralPath $stageRoot -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $cmakeBuildRoot -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $stageRoot -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $stageRoot "deps") -Force | Out-Null

$sourceFiles = @(
    "main.cpp",
    "compressed_symbol_supplier.cpp",
    "compressed_symbol_supplier.h",
    "repo_source_line_resolver.cc",
    "repo_source_line_resolver.h",
    "repo_source_line_resolver_types.h"
)

foreach ($sourceFile in $sourceFiles) {
    Copy-Item -LiteralPath (Join-Path $carburetorRoot $sourceFile) -Destination (Join-Path $stageRoot $sourceFile) -Force
}

$compressedSymbolSupplierPath = Join-Path $stageRoot "compressed_symbol_supplier.cpp"
$compressedSymbolSupplier = Get-Content $compressedSymbolSupplierPath -Raw
$compressedSymbolSupplier = $compressedSymbolSupplier.Replace("struct stat sb;`r`n  return stat(file_name.c_str(), &sb) == 0;", "struct _stat64i32 sb;`r`n  return _stat64i32(file_name.c_str(), &sb) == 0;")
$compressedSymbolSupplier = $compressedSymbolSupplier.Replace("open(symbol_file->c_str(), O_RDONLY)", "_open(symbol_file->c_str(), O_RDONLY)")
$compressedSymbolSupplier = $compressedSymbolSupplier.Replace("lseek(symbol_fd, -4, SEEK_END)", "_lseek(symbol_fd, -4, SEEK_END)")
$compressedSymbolSupplier = $compressedSymbolSupplier.Replace("read(symbol_fd, &uncompressed_length, sizeof(uncompressed_length))", "_read(symbol_fd, &uncompressed_length, sizeof(uncompressed_length))")
$compressedSymbolSupplier = $compressedSymbolSupplier.Replace("lseek(symbol_fd, 0, SEEK_SET)", "_lseek(symbol_fd, 0, SEEK_SET)")
Set-Content -LiteralPath $compressedSymbolSupplierPath -Value $compressedSymbolSupplier -NoNewline

Copy-Item -LiteralPath (Join-Path $repoRoot "buildbot\carburetor.windows.CMakeLists.txt") -Destination (Join-Path $stageRoot "CMakeLists.txt") -Force

$breakpadRoot = Resolve-CarburetorDependency $carburetorRoot "breakpad-prefix\src\breakpad"
$distormRoot = Resolve-CarburetorDependency $carburetorRoot "distorm-prefix\src\distorm"
$jsonRoot = Resolve-CarburetorDependency $carburetorRoot "json-prefix\src\json\include"
$cppcodecRoot = Resolve-CarburetorDependency $carburetorRoot "codec-prefix\src\codec"
$zlibRoot = Join-Path $repoRoot "third_party\zlib"

Copy-Item -LiteralPath $breakpadRoot -Destination (Join-Path $stageRoot "deps\breakpad") -Recurse -Force
Copy-Item -LiteralPath $distormRoot -Destination (Join-Path $stageRoot "deps\distorm") -Recurse -Force
Copy-Item -LiteralPath $jsonRoot -Destination (Join-Path $stageRoot "deps\json") -Recurse -Force
Copy-Item -LiteralPath $cppcodecRoot -Destination (Join-Path $stageRoot "deps\cppcodec") -Recurse -Force
Copy-Item -LiteralPath $zlibRoot -Destination (Join-Path $stageRoot "deps\zlib") -Recurse -Force

$patchRoot = Join-Path $repoRoot "patches"
if ($ApplyBreakpadPatches -and (Test-Path (Join-Path $stageRoot "deps\breakpad\.git"))) {
    Push-Location (Join-Path $stageRoot "deps\breakpad")
    try {
        & git clean -fdx
        & git reset --hard
        Get-ChildItem $patchRoot -Filter "*.patch" | Sort-Object Name | ForEach-Object {
            & git apply --reject $_.FullName
            if ($LASTEXITCODE -ne 0) {
                Write-Warning "Failed to apply patch $($_.Name); continuing with upstream breakpad sources."
            }
        }
    } finally {
        Pop-Location
    }
}

$platform = if ($Arch -eq "x86") { "Win32" } else { "x64" }
New-Item -ItemType Directory -Path $cmakeBuildRoot -Force | Out-Null

$cmakeArgs = @(
    "-S", $stageRoot,
    "-B", $cmakeBuildRoot,
    "-G", $Generator
)

if ($Generator -like "Visual Studio*") {
    $cmakeArgs += @("-A", $platform)
}

& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) {
    throw "cmake configure failed with exit code $LASTEXITCODE"
}

& cmake --build $cmakeBuildRoot --config Release
if ($LASTEXITCODE -ne 0) {
    throw "cmake build failed with exit code $LASTEXITCODE"
}
