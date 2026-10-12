# Compresso

Compresso is a Windows-only desktop utility for applying native transparent NTFS LZX compression to game folders. Compressed files remain in place and games remain directly accessible; no archive or extraction workflow is involved.

The Qt Widgets interface supports selecting a game directory with the Windows folder picker or entering its path manually. Analyze reports logical size, current allocated size, file count, and filesystem LZX eligibility. Compress applies native `compact.exe /C /EXE:LZX` compression in place to eligible files on local NTFS volumes. Restore asks for confirmation, then reverses Compresso's WOF LZX compression with `compact.exe /U /EXE:LZX`; other algorithms are skipped. Files stay in the selected installation and retain their contents.

Background compression and restoration report completed files, logical bytes accounted for, elapsed time, and the current filename. Their standard progress bar remains indeterminate until the target file count is known, then reflects completed file outcomes.

## Requirements

- Windows 10 or later with NTFS volumes
- CMake 3.25 or later
- Ninja
- GCC/MinGW-w64 with C++23 support
- Qt 6 Widgets built for the same MinGW toolchain

If Qt is not in a standard CMake search location, set `CMAKE_PREFIX_PATH` to its installation prefix (the directory containing `lib/cmake/Qt6`). For example, in PowerShell: `$env:CMAKE_PREFIX_PATH = 'C:/Qt/6.x.x/mingw_64'`.

## Configure and build

The included presets use Ninja and select GCC from `PATH`.

```powershell
cmake --preset debug
cmake --build --preset debug
ctest --test-dir out/build/debug --output-on-failure
```

For an optimized build:

```powershell
cmake --preset release
cmake --build --preset release
```

Run `out/build/debug/Compresso.exe`. Ensure the Qt 6 and MinGW runtime `bin` directories are on `PATH` when launching from a terminal.

## Project layout

- `src/` — application implementation
- `include/` — public project headers
- `tests/` — automated UI and directory validation checks
- `docs/` — design and implementation notes
- `resources/` — application resources

## Compression design constraint

Compression is per file and reports successes, failures, and skipped entries. It excludes volume roots, protected Windows locations, and Compresso's own installation directory. LZX is optimized for read-heavy files; test a specific game before using it. See the [compression design note](docs/compression.md) and [backend details](docs/compression-backend.md).

Scanning, LZX compression, and restoration run on an owned Qt worker thread. Progress and results are delivered to the window through queued signals, and closing the window waits for an active operation to finish. See [the filesystem scanning note](docs/filesystem-scanning.md) for scanner behavior and [the operation worker note](docs/async-operations.md) for the worker lifecycle.

Failures use structured error codes and detailed diagnostics in the optional operation log. See [error handling](docs/error-handling.md) for the error categories and process policy.

## License

This project is distributed under the MIT License. See [LICENSE](LICENSE).
