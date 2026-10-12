# Development notes

Compresso is a Windows-only C++23 application. CMake generates Ninja build files and Qt 6 Widgets supplies the desktop UI. GCC/MinGW-w64 is the supported compiler toolchain.

The `debug` and `release` configure/build presets provide the initial local workflows. Keep application code in `src/`, public headers in `include/`, future checks in `tests/`, and assets in `resources/`.
