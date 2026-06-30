param(
    [string]$CarburetorRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) "..\carburetor"),

    [ValidateSet("x86", "x64")]
    [string]$Arch = "x64",

    [string]$BuildDir = "",
    [string]$DockerImage = "accelerator-linux-builder:bullseye",
    [string]$StageRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) "..\accelerator-linux-stage")
)

$ErrorActionPreference = "Stop"

function Assert-Command([string]$Name) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Required command was not found in PATH: $Name"
    }
}

Assert-Command docker

if (-not (Test-Path $CarburetorRoot)) {
    throw "Carburetor path does not exist: $CarburetorRoot"
}

$carburetorRootPath = (Resolve-Path $CarburetorRoot).Path
$stageRootPath = [System.IO.Path]::GetFullPath($StageRoot)
$effectiveBuildDir = if ($BuildDir) { $BuildDir } else { "build-linux-stable-$Arch" }
$archCFlags = if ($Arch -eq "x86") { "-m32" } else { "-m64" }
$dependencyRoot = if ($Arch -eq "x86") { "build-linux-stable-x86" } else { "build-linux-stable-x64" }
$stageKey = (($effectiveBuildDir -replace "[^A-Za-z0-9_.-]", "-").Trim("-"))
if (-not $stageKey) {
    $stageKey = $Arch
}

New-Item -ItemType Directory -Path $stageRootPath -Force | Out-Null

$containerCommand = @'
set -euxo pipefail
work_root=/stage/carburetor-__ARCH__-__STAGE_KEY__
rm -rf "$work_root"
git clone /src/carburetor "$work_root"
(cd /src/carburetor && tar \
  --exclude=.git \
  --exclude=build \
  --exclude=build-linux-stable-x86 \
  --exclude=build-linux-stable-x64 \
  -cf - .) | tar -xf - -C "$work_root"
cd "$work_root"
if [ ! -f './__DEPENDENCY_ROOT__/distorm-prefix/src/distorm/distorm3.a' ]; then
  make -C './__DEPENDENCY_ROOT__/distorm-prefix/src/distorm/make/linux' clean clib CFLAGS='__ARCH_CFLAGS__ -fPIC -O2 -Wall -DSUPPORT_64BIT_OFFSET -DDISTORM_STATIC -std=c99'
fi
rm -rf './__BUILD_DIR__'
cmake -S . -B './__BUILD_DIR__' \
  -DCMAKE_C_COMPILER=gcc \
  -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_C_FLAGS='__ARCH_CFLAGS__' \
  -DCMAKE_CXX_FLAGS='__ARCH_CFLAGS__' \
  -DCMAKE_EXE_LINKER_FLAGS='__ARCH_CFLAGS__' \
  -DCARBURETOR_BREAKPAD_SOURCE_DIR='./__DEPENDENCY_ROOT__/breakpad-prefix/src/breakpad' \
  -DCARBURETOR_JSON_SOURCE_DIR='./__DEPENDENCY_ROOT__/json-prefix/src/json' \
  -DCARBURETOR_CODEC_SOURCE_DIR='./__DEPENDENCY_ROOT__/codec-prefix/src/codec' \
  -DCARBURETOR_DISTORM_SOURCE_DIR='./__DEPENDENCY_ROOT__/distorm-prefix/src/distorm'
cmake --build './__BUILD_DIR__' -- -j"$(nproc)"
'@

$containerCommand = $containerCommand.
    Replace("__ARCH__", $Arch).
    Replace("__STAGE_KEY__", $stageKey).
    Replace("__DEPENDENCY_ROOT__", $dependencyRoot).
    Replace("__ARCH_CFLAGS__", $archCFlags).
    Replace("__BUILD_DIR__", $effectiveBuildDir)

& docker run --rm `
    -v "${carburetorRootPath}:/src/carburetor:ro" `
    -v "${stageRootPath}:/stage" `
    -w /stage `
    $DockerImage `
    bash -lc $containerCommand

if ($LASTEXITCODE -ne 0) {
    throw "Linux carburetor build failed with exit code $LASTEXITCODE"
}
