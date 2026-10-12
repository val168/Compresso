# Compression direction

Compresso uses Windows `compact.exe` with `/C` and `/EXE:LZX` to apply transparent NTFS compression to individual files in place. Files remain at their original paths and are directly accessible; the application does not create archives or extraction workflows.

The backend requires a local fixed NTFS volume that reports file-compression support. It rejects volume roots, Windows and protected system locations, and Compresso's own installation directory. It also skips reparse points, encrypted or sparse files, files already marked compressed, and files that cannot be safely opened for preflight. Results retain each process exit code, stdout, stderr, and per-file outcome.

LZX is intended for read-heavy, infrequently modified files. The backend performs a busy-file preflight and detects metadata changes around each operation; it reports Windows sharing or compression failures instead of counting a file as safely compressed. It cannot guarantee that a particular game remains compatible, so check a specific game's behavior after compression. The native filesystem operation preserves logical file contents and paths. See [backend implementation details](compression-backend.md).

Never add ZIP, CAB, 7z, or custom archive formats.
