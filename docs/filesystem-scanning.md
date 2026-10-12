# Filesystem scanning

`compresso::DirectoryScanner::scan` in `include/compresso/directory_scanner.h` recursively inspects a game directory without depending on Qt. Each accepted regular file is returned with its path, logical size from `std::filesystem::file_size`, and relevant attributes. The result also includes the accepted file count, summed logical size, skipped paths with reasons, and whether traversal completed.

On Windows, the scanner reads file attributes such as read-only, hidden, system, archive, compressed, encrypted, and sparse. It treats every reparse point as a boundary: symbolic links, directory junctions, and mount points are reported as skipped and are not traversed. The selected root is rejected if it is itself a reparse point.

Metadata and traversal failures are returned in `ScanResult` rather than thrown as filesystem exceptions. A failed directory traversal marks the result incomplete; inaccessible entries and files whose sizes cannot be read remain listed in `skipped_entries`. Callers can show a concise skipped count while retaining each path and error for details.

The scanner only inspects filesystem metadata. It does not read file contents, change attributes, move files, or create archives. Its logical sizes are intended as input for a future native NTFS LZX operation through Windows `compact.exe`.
