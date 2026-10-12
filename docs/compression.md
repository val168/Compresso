# Compression direction

Compresso is intended to use Windows' native transparent NTFS compression through `compact.exe /EXE:LZX`. This keeps game files at their existing paths, so games can continue to open them without an extraction step.

Compression is not implemented in the current application. Before implementation, define handling for non-NTFS volumes, unsupported Windows versions, permissions, cancellation, progress reporting, and partial failures. Invoke the system tool without constructing shell command strings from user input, and report its exit status and output clearly.

Do not add ZIP, CAB, 7z, or custom archive formats.
