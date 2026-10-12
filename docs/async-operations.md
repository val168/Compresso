# Asynchronous operations

The main window runs each directory scan or compression request on a dedicated `QThread` with one `OperationWorker`. Only one operation can run at a time. The path field, Browse, Analyze, and Compress controls are disabled while the worker is active.

The worker owns the scanner call or the compression backend call. The compression backend creates and waits for each `QProcess` on that worker thread, so process startup and file operations do not block the GUI. Progress, errors, and completed results cross to the window through Qt signals and queued connections; the GUI updates its widgets only on the GUI thread.

When a worker finishes, the thread quits and the worker is deleted through Qt's deferred-deletion mechanism. Closing the window during an operation defers the close until the worker exits. The window destructor also waits for the thread, covering application shutdown paths that bypass the close event. Operations are allowed to finish rather than being interrupted while `compact.exe` may be working on a file.

The UI tests exercise scanner progress, verify that a timer continues firing while a larger scan runs, and check that directory controls are disabled and restored around scans and compression.
