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

## Server Installation

Copy the runtime files to your game server like this:

- `addons/sourcemod/extensions/accelerator.ext.so` - 32-bit Linux extension
- `addons/sourcemod/extensions/x64/accelerator.ext.so` - 64-bit Linux extension
- `addons/sourcemod/extensions/accelerator.autoload` - autoload marker
- `addons/sourcemod/gamedata/accelerator.games.txt` - required gamedata
- `addons/sourcemod/plugins/accelerator_local.smx` - local dump command plugin

If you want local raw processing, also provide:

- `addons/sourcemod/bin/carburetor` - 32-bit `carburetor`
- `addons/sourcemod/bin/x64/carburetor` - 64-bit `carburetor`

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
"MinidumpLocalCarburetorPath" "addons/sourcemod/bin/x64/carburetor"
```

Use `addons/sourcemod/bin/carburetor` instead on 32-bit servers, or point to any other absolute / game-relative path where your `carburetor` binary lives.

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
  - prints the full trace plus metadata, without console history
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
