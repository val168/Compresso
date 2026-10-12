# Repository guidance

- Compresso is a Windows-only C++23 application built with CMake, Ninja, GCC/MinGW-w64, and Qt 6 Widgets.
- Keep the interface restrained and consistent with ordinary Windows desktop utilities. Prefer standard controls and native platform behavior.
- Compression and restoration use Windows `compact.exe` for transparent NTFS LZX files; keep files in place and do not add archive creation or extraction.
- Run scanning, compression, and restoration outside the GUI thread. Keep worker and process ownership explicit, deliver UI updates through Qt signals and slots, and finish active work safely before destroying its thread.
- Keep public headers under `include/`, implementation under `src/`, tests under `tests/`, project notes under `docs/`, and non-code resources under `resources/`.
- Build with the `debug` or `release` CMake preset.
