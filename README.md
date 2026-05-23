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
- Linux server runtime for the provided `.so` binaries.
- `carburetor` binary for local raw processing:
  - `addons/sourcemod/bin/carburetor` for 32-bit servers
  - `addons/sourcemod/bin/x64/carburetor` for 64-bit servers
- Writable directories:
  - `addons/sourcemod/data/dumps`
  - `addons/sourcemod/data/dumps/symbols`
  - `addons/sourcemod/data/dumps/outputs`

`webternet.ext` is required only for remote upload mode. It is not required in local mode.

## Core Config Keys

- `MinidumpMode`
  - `site` - traditional remote upload mode
  - `local` - offline mode, no upload attempts
- `MinidumpDeleteAfterProcessing`
  - `1` by default
  - `0` / `no` / `n` keeps dumps after successful processing
- `MinidumpLocalCarburetorPath`
  - optional override for the `carburetor` binary path
- `MinidumpLocalSymbolPath`
  - optional extra symbol search paths, separated with `;` or `,`

Recommended local configuration:

```cfg
"MinidumpMode" "local"
"MinidumpDeleteAfterProcessing" "0"
"MinidumpLocalSymbolPath" "addons/sourcemod/data/dumps/symbols"
```

## Local Commands

These commands are provided by `accelerator_local.smx`:

- `sm_dump_list`
  - lists pending local dumps
- `sm_proc_stack_dump <dump>`
  - prints the short trace plus metadata, without console history
- `sm_proc_dump <dump> Trace [output-file]`
  - writes the trace and metadata to a file
- `sm_proc_dump <dump> rawstack [output-file]`
- `sm_proc_dump <dump> rawmemory [output-file]`
- `sm_proc_dump <dump> rawraw [output-file]`
- `sm_proc_dump <dump> all [output-file]`
- `sm_proc_dump <dump> console`
  - prints only the captured console history
- `sm_dump_crash_test`
  - intentionally crashes the server so Accelerator can capture a test dump

If no output filename is provided, dump results are written to:

```text
addons/sourcemod/data/dumps/outputs/<mode>_<dump>.txt
```

## Building

This project builds the extension only. It does not build `carburetor`.

Requirements:

- Python
- `ambuild2`
- a SourceMod source checkout available via `--sm-path` or the `SOURCEMOD` environment variable
- a working C/C++ toolchain for the requested targets

Example:

```bash
python configure.py --sm-path=/path/to/sourcemod
ambuild
```

By default the build attempts both `x86` and `x86_64` when supported by the local toolchain.

## Packaging

`buildbot/PackageScript` packages Accelerator runtime files only:

- extension binaries
- `accelerator.autoload`
- gamedata
- SourcePawn include/example files

External runtime dependencies such as `carburetor` are expected to be copied into the final release artifact separately.

## Notes About Carburetor

Accelerator uses `carburetor` as an external companion tool for local raw dump processing. The binary is not modified by Accelerator at runtime. Accelerator simply resolves the path, executes it, passes symbol paths, and consumes the resulting output.

## License

See the project source headers and upstream licensing terms for Accelerator, SourceMod SDK pieces, and bundled third-party components.
