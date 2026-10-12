# Filesystem statistics

`compresso::DirectoryScanner::scan` in `include/compresso/directory_scanner.h` recursively inspects a game directory without depending on Qt. For each regular file it reports the logical size from `std::filesystem::file_size` and the current allocated size returned by Windows `GetCompressedFileSizeW`. The allocated value is current on-disk allocation, including filesystem compression or sparse allocation where applicable; it is not an estimate of future LZX savings. If Windows cannot report allocation for any file, the aggregate is marked incomplete and the failure is retained separately from skipped files.

The scan result also includes file count, skipped paths, traversal completeness, and volume information. The volume is LZX eligible only when Windows reports NTFS and the volume advertises file compression support. Volume query failures and unsupported filesystems are represented explicitly; eligibility does not depend on file extensions or file attributes.

On Windows, the scanner records read-only, hidden, system, archive, compressed, encrypted, and sparse attributes. Every reparse point is treated as a boundary: symbolic links, directory junctions, and mount points are reported as skipped and are not traversed. The selected root is rejected if it is itself a reparse point.

Metadata and traversal failures are returned in `ScanResult` rather than thrown as filesystem exceptions. A failed directory traversal marks the result incomplete; inaccessible entries and files whose logical sizes cannot be read remain listed in `skipped_entries`.

The scanner only inspects filesystem metadata. It does not read or change game contents, change attributes, move files, or create archives. These statistics prepare for a future native transparent NTFS LZX operation through Windows `compact.exe`.
