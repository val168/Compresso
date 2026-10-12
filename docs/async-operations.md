# Asynchronous operations

The main window runs each directory scan or compression request on a dedicated `QThread` with one `OperationWorker`. Only one operation can run at a time. The path field, Browse, Analyze, and Compress controls are disabled while the worker is active.

The worker owns the scanner call or the compression backend call. The compression backend creates and waits for each `QProcess` on that worker thread, so process startup and file operations do not block the GUI. Progress, errors, and completed results cross to the window through Qt signals and queued connections; the GUI updates its widgets only on the GUI thread.

When a worker finishes, the thread quits and the worker is deleted through Qt's deferred-deletion mechanism. Closing the window during an operation defers the close until the worker exits. The window destructor also waits for the thread, covering application shutdown paths that bypass the close event. Operations are allowed to finish rather than being interrupted while `compact.exe` may be working on a file.

The compression worker reports completed and total file counts, accounted logical bytes, elapsed time, and the current filename through a typed progress signal. The status text stays visible during work, and a one-second UI timer refreshes the elapsed duration while an individual `compact.exe` process runs. The standard progress bar is indeterminate while totals are unknown and changes to a determinate percentage only after enumeration provides a file count. Each completion increment follows an actual file result, including skips and failures.

The UI tests exercise scanner progress, verify that a timer continues firing while a larger scan runs, and check that directory controls are disabled and restored around scans and compression. Backend tests cover successful, failed, and skipped files and assert their associated progress totals.
