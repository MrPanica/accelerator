# Accelerator

Accelerator is a SourceMod extension that captures SRCDS crashes, preserves minidumps, and can process them locally without sending anything to a remote website.

This repository builds the `accelerator` extension itself. It does not vendor or auto-fetch `carburetor` sources. Local raw dump processing uses an external `carburetor` binary that must be provided separately at runtime.

## Features

- Crash capture for Source dedicated servers.
- Traditional remote upload mode for existing Accelerator/Throttle style workflows.
- Fully local mode that never uploads dumps, symbols, or binaries.
- Local dump inspection commands exposed through the `accelerator_local.smx` plugin.
- Automatic local symbol generation into `addons/sourcemod/data/dumps/symbols`.
- Human-readable output files in `addons/sourcemod/data/dumps/outputs`.

## Repository Layout

- `extension/` - native extension source.
- `scripting/` - SourcePawn include files and the local command plugin.
- `gamedata/` - gamedata required by the extension.
- `buildbot/PackageScript` - packages extension runtime files only.

## Runtime Requirements

- SourceMod 1.11+ or newer.
- Supported runtime targets:
  - Linux `x86` / `x86_64`
  - Windows `x86` / `x86_64`
- `carburetor` binary for local raw processing:
  - Linux `x86`: `addons/sourcemod/bin/carburetor`
  - Linux `x86_64`: `addons/sourcemod/bin/x64/carburetor`
  - Windows `x86`: `addons/sourcemod/bin/carburetor.exe`
  - Windows `x86_64`: `addons/sourcemod/bin/x64/carburetor.exe`
- Optional `dump_syms.exe` for automatic local symbol generation on Windows:
  - Windows `x86`: `addons/sourcemod/bin/dump_syms.exe`
  - Windows `x86_64`: `addons/sourcemod/bin/x64/dump_syms.exe`
- Writable directories:
  - `addons/sourcemod/data/dumps`
  - `addons/sourcemod/data/dumps/symbols`
  - `addons/sourcemod/data/dumps/outputs`

`webternet.ext` is required only for remote upload mode. It is not required in local mode.

## Server Installation

Copy the common runtime files to your game server:

- `addons/sourcemod/extensions/accelerator.autoload` - autoload marker
- `addons/sourcemod/gamedata/accelerator.games.txt` - required gamedata
- `addons/sourcemod/plugins/accelerator_local.smx` - local dump command plugin

Linux runtime files:

- `addons/sourcemod/extensions/accelerator.ext.so` - 32-bit Linux extension
- `addons/sourcemod/extensions/x64/accelerator.ext.so` - 64-bit Linux extension
- `addons/sourcemod/bin/carburetor` - 32-bit Linux `carburetor`
- `addons/sourcemod/bin/x64/carburetor` - 64-bit Linux `carburetor`

Windows runtime files:

- `addons/sourcemod/extensions/accelerator.ext.dll` - 32-bit Windows extension
- `addons/sourcemod/extensions/accelerator.ext.pdb` - 32-bit Windows symbols for local symbol generation
- `addons/sourcemod/extensions/x64/accelerator.ext.dll` - 64-bit Windows extension
- `addons/sourcemod/extensions/x64/accelerator.ext.pdb` - 64-bit Windows symbols for local symbol generation
- `addons/sourcemod/bin/carburetor.exe` - 32-bit Windows `carburetor`
- `addons/sourcemod/bin/x64/carburetor.exe` - 64-bit Windows `carburetor`

Optional Windows symbol-generation helper:

- `addons/sourcemod/bin/dump_syms.exe` - 32-bit Windows `dump_syms`
- `addons/sourcemod/bin/x64/dump_syms.exe` - 64-bit Windows `dump_syms`

The following directories should exist and be writable:

- `addons/sourcemod/data/dumps`
- `addons/sourcemod/data/dumps/symbols`
- `addons/sourcemod/data/dumps/outputs`

## Operating Modes

Accelerator supports two distinct modes controlled by `MinidumpMode`.

### Site Mode

Use `site` mode when you want the traditional Accelerator workflow:

- dumps are prepared for upload
- symbol and binary upload logic stays active
- remote presubmit / upload logic is used
- `webternet.ext` is required

Typical `core.cfg` keys for `site` mode:

```cfg
"MinidumpMode" "site"
"MinidumpDeleteAfterProcessing" "1"
"MinidumpAccount" "7656119..."
"MinidumpPresubmit" "yes"
"MinidumpSymbolUpload" "3"
"MinidumpBinaryUpload" "yes"
"MinidumpUrl" "http://example.com/submit?token=YOUR_TOKEN"
"MinidumpSymbolUrl" "http://example.com/symbols/submit?token=YOUR_TOKEN"
"MinidumpBinaryUrl" "http://example.com/binary/submit?token=YOUR_TOKEN"
```

Notes:

- `MinidumpDeleteAfterProcessing "1"` keeps the historical behavior and removes successfully processed dumps.
- Local helper commands can still exist, but the extension itself remains in remote upload mode.
- `MinidumpAccount` should be the Steam account used for dump ownership / attribution.
- `MinidumpSymbolUpload` controls how aggressively symbols are uploaded in remote mode.
- `MinidumpPresubmit`, `MinidumpUrl`, `MinidumpSymbolUrl`, and `MinidumpBinaryUrl` are only meaningful in `site` mode.

### Local Mode

Use `local` mode when you want the server to stay fully offline:

- no presubmit requests
- no dump upload
- no symbol upload
- no binary upload
- local symbol generation is used for analysis
- `carburetor` is used only as an external runtime tool for local raw dump processing
- `webternet.ext` is not required

Typical `core.cfg` keys for `local` mode:

```cfg
"MinidumpMode" "local"
"MinidumpDeleteAfterProcessing" "0"
"MinidumpLocalSymbolPath" "addons/sourcemod/data/dumps/symbols"
```

Optional `core.cfg` keys for local mode:

```cfg
"MinidumpLocalCarburetorPath" "bin/x64/carburetor"
"MinidumpLocalCarburetorPath" "addons/sourcemod/bin/x64/carburetor"
```

Use `bin/carburetor` or `addons/sourcemod/bin/carburetor` instead on 32-bit Linux servers.
Use `bin/carburetor.exe` or `addons/sourcemod/bin/carburetor.exe` on 32-bit Windows servers.
Absolute paths are also accepted.

On Windows, automatic local symbol generation also expects `dump_syms.exe` in the matching `addons/sourcemod/bin[/x64]` directory.
Keep the matching `accelerator.ext.pdb` next to each `accelerator.ext.dll` if you want local stack traces to resolve Accelerator function names instead of only `module + offset`.

The recommended local workflow is asynchronous. The plugin queues the heavy dump analysis work onto a background worker so normal use does not require `-nowatchdog`.

## Core Config Keys

- `MinidumpMode`
  - `site` - traditional remote upload mode
  - `local` - fully offline local processing mode
- `MinidumpAccount`
  - Steam account identifier used by the remote workflow
- `MinidumpDeleteAfterProcessing`
  - `1` by default
  - `0` / `no` / `n` keeps dumps after successful processing
- `MinidumpPresubmit`
  - enables the remote presubmit flow in `site` mode
- `MinidumpSymbolUpload`
  - controls remote symbol upload behavior in `site` mode
- `MinidumpBinaryUpload`
  - enables remote binary upload in `site` mode
- `MinidumpUrl`
  - remote dump submit endpoint
- `MinidumpSymbolUrl`
  - remote symbol submit endpoint
- `MinidumpBinaryUrl`
  - remote binary submit endpoint
- `MinidumpLocalCarburetorPath`
  - optional override for the `carburetor` binary path
- `MinidumpLocalSymbolPath`
  - optional extra symbol search paths, separated with `;` or `,`

## Local Commands

These commands are provided by `accelerator_local.smx`:

- `sm_dump_list`
  - lists pending local dumps
- `sm_proc_stack_dump <dump>`
  - starts an asynchronous stack trace job
  - prints the final stack trace automatically when the job finishes
- `sm_proc_dump <dump> Trace [output-file]`
  - starts an asynchronous dump processing job
  - prints the final completion status automatically when the job finishes
- `sm_proc_dump <dump> rawstack [output-file]`
- `sm_proc_dump <dump> rawmemory [output-file]`
- `sm_proc_dump <dump> rawraw [output-file]`
- `sm_proc_dump <dump> all [output-file]`
- `sm_proc_dump <dump> console`
  - prints only the captured console history
- `sm_proc_jobs`
  - lists processing jobs
- `sm_proc_status <job-id>`
  - shows job state and output path
- `sm_proc_result <job-id>`
  - optional manual fetch for the finished stack report or final file-processing status
- `sm_dump_crash_test`
  - intentionally crashes the server so Accelerator can capture a test dump

If no output filename is provided, dump results are written to:

```text
addons/sourcemod/data/dumps/outputs/<mode>_<dump>.txt
```

Legacy blocking natives and workflows still exist for compatibility. If you explicitly use the old synchronous path on Linux and the server spends too long in local analysis, `-nowatchdog` is still a valid fallback launch option.

## Building

The default AMBuild path builds the `accelerator` extension. `carburetor` remains a separate project and is built separately.

Requirements:

- Python
- `ambuild2`
- a SourceMod source checkout available via `--sm-path` or the `SOURCEMOD` environment variable
- a working C/C++ toolchain for the requested targets

Typical native build:

```bash
python configure.py --sm-path=/path/to/sourcemod --targets=x86,x86_64
ambuild
```

The local command plugin can be rebuilt separately with `spcomp`:

```bash
spcomp scripting/accelerator_local.sp -i=scripting/include -o=accelerator_local.smx
```

Linux container build helper:

```bash
bash ./dockerbuild/accelerator_docker_build.sh
```

This produces Linux `x86` and `x86_64` extension binaries under `build/accelerator.ext/`.

Windows native build helper:

- run `configure.py` from a Visual Studio Developer PowerShell or another shell with a working MSVC toolchain
- build both `x86` and `x86_64`

Windows helper scripts:

- `buildbot/build_windows_accelerator.ps1`
  - builds `accelerator` with AMBuild from a Visual Studio Developer PowerShell
- `buildbot/build_windows_carburetor.ps1`
  - stages a local copy of the sibling `carburetor` repository
  - copies locally cached `breakpad`, `distorm`, `json`, and `cppcodec`
  - applies the local Breakpad patch set in the staging directory
  - configures and builds `carburetor.exe` for `x86` or `x64`

## Packaging

`buildbot/PackageScript` packages Accelerator runtime files only:

- extension binaries
- `accelerator.autoload`
- gamedata
- `accelerator_local.smx` when a prebuilt plugin is present

The ready-to-deploy runtime packages used in this repository are stored in `dist/`:

- `dist/windows-sm112-api8`
- `dist/linux`
- `dist/accelerator-windows-sm112-api8.zip`
- `dist/accelerator-linux.tar.gz`

These `dist` packages are runtime-only and contain:

- extension binaries
- `accelerator.autoload`
- gamedata
- `accelerator_local.smx`
- runtime helper binaries such as `carburetor`, `dump_syms.exe`, `msdia140.dll`, and Windows `.pdb` files where applicable

If you package from raw `buildbot/PackageScript` output instead of the prepared `dist` directory, add:

- Linux `carburetor` for `x86` and `x86_64`
- Windows `carburetor.exe` for `x86` and `x86_64`
- Windows `dump_syms.exe` for `x86` and `x86_64`
- Windows `msdia140.dll` for `x86` and `x86_64`
- Windows `accelerator.ext.pdb` for `x86` and `x86_64`

## Notes About Carburetor

Accelerator uses `carburetor` as an external companion tool for local raw dump processing. The binary is not modified by Accelerator at runtime. Accelerator simply resolves the path, executes it, passes symbol paths, and consumes the resulting output.

On Windows, local symbol generation is done by invoking an external `dump_syms.exe` helper if it is present in the architecture-matching `addons/sourcemod/bin` directory.

## License

See the project source headers and upstream licensing terms for Accelerator, SourceMod SDK pieces, and bundled third-party components.
