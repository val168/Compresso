# Compresso

Compresso is a Windows-only desktop utility for applying native transparent NTFS LZX compression to game folders. Compressed files remain in place and games remain directly accessible; no archive or extraction workflow is involved.

The current application is an initial Qt Widgets shell. Compression is **not implemented yet**.

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
- `tests/` — future automated checks
- `docs/` — design and implementation notes
- `resources/` — application resources

## Compression design constraint

The planned operation is Windows `compact.exe /EXE:LZX`, applied directly to files on NTFS. The implementation must preserve in-place paths and must not introduce ZIP, CAB, 7z, or a custom archive format. See [the compression design note](docs/compression.md).

## License

This project is distributed under the MIT License. See [LICENSE](LICENSE).
