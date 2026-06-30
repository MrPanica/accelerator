param(
    [Parameter(Mandatory = $true)]
    [string]$SourceModPath,

    [string]$BuildDir = "build/linux-accelerator",
    [string]$Targets = "x86,x86_64",
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

if (-not (Test-Path $SourceModPath)) {
    throw "SourceMod path does not exist: $SourceModPath"
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$sourceModRoot = (Resolve-Path $SourceModPath).Path
$stageRootPath = [System.IO.Path]::GetFullPath($StageRoot)
$linuxBuildDir = $BuildDir.Replace("\", "/")
$stageKey = (($BuildDir -replace "[^A-Za-z0-9_.-]", "-").Trim("-"))
if (-not $stageKey) {
    $stageKey = "default"
}

New-Item -ItemType Directory -Path $stageRootPath -Force | Out-Null

$containerCommand = @'
set -euxo pipefail
work_root=/stage/accelerator-work-__STAGE_KEY__
rm -rf "$work_root"
git clone --recurse-submodules /src/accelerator "$work_root"
(cd /src/accelerator && tar \
  --exclude=.git \
  --exclude=build \
  --exclude=dist \
  --exclude=_am_temp \
  -cf - .) | tar -xf - -C "$work_root"
cd "$work_root"
rm -rf '__BUILD_DIR__'
mkdir -p '__BUILD_DIR__'
cd '__BUILD_DIR__'
CC=clang CXX=clang++ python3 "$work_root/configure.py" --sm-path /src/sourcemod --targets '__TARGETS__'
ambuild
'@

$containerCommand = $containerCommand.
    Replace("__STAGE_KEY__", $stageKey).
    Replace("__BUILD_DIR__", $linuxBuildDir).
    Replace("__TARGETS__", $Targets)

& docker run --rm `
    -v "${repoRoot}:/src/accelerator:ro" `
    -v "${sourceModRoot}:/src/sourcemod:ro" `
    -v "${stageRootPath}:/stage" `
    -w /stage `
    $DockerImage `
    bash -lc $containerCommand

if ($LASTEXITCODE -ne 0) {
    throw "Linux accelerator build failed with exit code $LASTEXITCODE"
}
