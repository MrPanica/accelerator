param(
    [Parameter(Mandatory = $true)]
    [string]$SourceModPath,

    [string]$BuildDir = "build\win-accelerator",
    [string]$Targets = "x86,x86_64",
    [string]$PythonExe = "python",
    [string]$AmbuildExe = "ambuild"
)

$ErrorActionPreference = "Stop"

function Assert-Command([string]$Name) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Required command was not found in PATH: $Name"
    }
}

function Copy-IfExists([string]$SourcePath, [string]$DestinationPath) {
    if (Test-Path $SourcePath) {
        $destinationDir = Split-Path -Parent $DestinationPath
        New-Item -ItemType Directory -Path $destinationDir -Force | Out-Null
        Copy-Item $SourcePath $DestinationPath -Force
    }
}

if (-not (Test-Path $SourceModPath)) {
    throw "SourceMod path does not exist: $SourceModPath"
}

Assert-Command $PythonExe
Assert-Command $AmbuildExe

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw "cl.exe was not found. Run this script from a Visual Studio Developer PowerShell."
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $repoRoot $BuildDir
$targetList = $Targets.Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ }

New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
Push-Location $buildRoot
try {
    & $PythonExe (Join-Path $repoRoot "configure.py") --sm-path $SourceModPath --targets $Targets
    if ($LASTEXITCODE -ne 0) {
        throw "configure.py failed with exit code $LASTEXITCODE"
    }

    & $AmbuildExe
    if ($LASTEXITCODE -ne 0) {
        throw "ambuild failed with exit code $LASTEXITCODE"
    }

    $packageRoot = Join-Path $buildRoot "package"
    $breakpadDumpSyms = Join-Path $repoRoot "third_party\breakpad\src\tools\windows\binaries\dump_syms.exe"
    Copy-IfExists $breakpadDumpSyms (Join-Path $packageRoot "addons\sourcemod\bin\dump_syms.exe")
    Copy-IfExists "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\DIA SDK\bin\msdia140.dll" (Join-Path $packageRoot "addons\sourcemod\bin\msdia140.dll")
    Copy-IfExists "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\DIA SDK\bin\amd64\msdia140.dll" (Join-Path $packageRoot "addons\sourcemod\bin\x64\msdia140.dll")

    if ($targetList -contains "x86") {
        Copy-IfExists (Join-Path $buildRoot "accelerator.ext\windows-x86\accelerator.ext.pdb") (Join-Path $packageRoot "addons\sourcemod\extensions\accelerator.ext.pdb")
    }
    if ($targetList -contains "x86_64") {
        Copy-IfExists (Join-Path $buildRoot "accelerator.ext\windows-x86_64\accelerator.ext.pdb") (Join-Path $packageRoot "addons\sourcemod\extensions\x64\accelerator.ext.pdb")
    }
} finally {
    Pop-Location
}
