# Repository guidance

- Compresso is a Windows-only C++23 application built with CMake, Ninja, GCC/MinGW-w64, and Qt 6 Widgets.
- Keep the interface restrained and consistent with ordinary Windows desktop utilities. Prefer standard controls and native platform behavior.
- Compression uses Windows `compact.exe /C /EXE:LZX` for transparent NTFS compression; keep it in place and do not add archive creation or extraction.
- Keep public headers under `include/`, implementation under `src/`, tests under `tests/`, project notes under `docs/`, and non-code resources under `resources/`.
- Build with the `debug` or `release` CMake preset.
