# Operation errors

`OperationError` is the shared, presentation-independent error record. It carries a stable error code, affected path, optional Windows/native code, optional `compact.exe` exit code, and technical details. Directory scans keep structured issues alongside their skipped-entry records; compression and restoration reports keep target errors, scanner issues, and per-file outcomes separate.

Skipped files remain `skipped`, never `compressed` or `restored`. Failures remain `failed`. The main window uses short inline summaries and exposes a `Details` log only when a scan or operation has diagnostic entries. That log includes paths, native codes, process exit codes, and captured stdout/stderr. Ordinary file and volume issues do not open modal dialogs. The Restore confirmation is the only modal dialog because it asks for a decision before changing file compression state.

Windows errors for missing paths, access denial, sharing conflicts, unsupported filesystems, and disk-full conditions map to stable error categories. A nonzero `compact.exe` exit is retained as a process failure with its exit code and raw output; localized console text is never parsed. Compresso stops processing later files after a launch failure, nonzero exit, or unexpected process termination because those failures cannot be reliably classified as isolated to one file. Remaining targets are explicitly reported as not attempted.

When `compact.exe` fails without a native disk-full code, the inline process-failure message asks the user to check free space, permissions, and file access. The raw process output is available in Details for diagnosis. No alternate compression format or archive fallback is used.
