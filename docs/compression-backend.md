# Native LZX compression

`compresso::CompressionBackend::compressDirectory` runs the Windows `compact.exe` utility once per accepted file with a structured argument list: `/C`, `/EXE:LZX`, `/Q`, and the file path. It does not invoke a command shell, create an archive, move a file, or rewrite file contents. The command uses transparent NTFS compression, so the files remain at their existing paths.

Before starting, the backend canonicalizes the selected directory and checks that it is not a volume root, a reparse point, a protected Windows location, or Compresso's own installation directory. It also requires a local fixed NTFS volume with the Windows file-compression capability flag.

Inaccessible entries, reparse points, files already compressed or using less allocation than their logical size, encrypted or sparse files, and files the operating system reports as busy are skipped. The backend also handles `compact.exe` failures per file. File identity, logical size, and last-write time are checked again after the command; a detected change is reported as a failure rather than counted as safely compressed. Each result retains the process exit code, stdout, stderr, and a concise error or skip reason. When NTFS does not retain LZX compression for a file and its allocation is unchanged, the result is skipped rather than reported as successful.

The backend reports per-file outcomes because a valid NTFS volume does not imply that every file or every game is a good candidate. LZX is intended for read-heavy files; evaluate performance and compatibility for a specific game before applying it. No compression starts merely by selecting a directory or analyzing it.
