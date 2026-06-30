param(
    [string]$ImageTag = "accelerator-linux-builder:bullseye"
)

$ErrorActionPreference = "Stop"

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    throw "Required command was not found in PATH: docker"
}

$dockerfile = Join-Path $PSScriptRoot "linux-builder.Dockerfile"
if (-not (Test-Path $dockerfile)) {
    throw "Dockerfile was not found: $dockerfile"
}

& docker build -f $dockerfile -t $ImageTag $PSScriptRoot
if ($LASTEXITCODE -ne 0) {
    throw "Linux builder image build failed with exit code $LASTEXITCODE"
}

