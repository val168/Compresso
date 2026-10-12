#include "compresso/main_window.h"

#include "compresso/directory_validator.h"
#include "compresso/operation_worker.h"

#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <array>
#include <algorithm>
#include <chrono>

namespace
{

QString diagnosticText(const compresso::OperationError& error)
{
    QString text = QString::fromStdString(compresso::operationErrorCodeName(error.code))
                   + ": " + QString::fromStdString(compresso::operationErrorMessage(error.code));
    if (!error.path.empty()) {
        text += QStringLiteral("\nPath: ") + QString::fromStdWString(error.path.native());
    }
    if (error.native_code != 0) {
        text += QStringLiteral("\nNative code: %1").arg(error.native_code);
    }
    if (error.process_exit_code >= 0) {
        text += QStringLiteral("\ncompact.exe exit code: %1").arg(error.process_exit_code);
    }
    if (!error.technical_details.empty()) {
        text += QStringLiteral("\n") + QString::fromUtf8(error.technical_details.c_str());
    }
    return text;
}

QString skippedEntryText(const compresso::SkippedEntry& entry)
{
    QString reason;
    switch (entry.reason) {
    case compresso::SkipReason::inaccessible_directory: reason = "inaccessible directory"; break;
    case compresso::SkipReason::inaccessible_entry: reason = "inaccessible entry"; break;
    case compresso::SkipReason::reparse_point: reason = "symbolic link or junction"; break;
    case compresso::SkipReason::unsupported_file_type: reason = "unsupported entry"; break;
    case compresso::SkipReason::file_size_error: reason = "file size unavailable"; break;
    case compresso::SkipReason::traversal_error: reason = "traversal error"; break;
    }
    QString text = QStringLiteral("Skipped (%1): ").arg(reason)
                   + QString::fromStdWString(entry.path.native());
    if (entry.error) {
        text += QStringLiteral("\n") + QString::fromStdString(entry.error.message());
    }
    return text;
}

QString skippedFileText(const compresso::CompressionFileResult& file)
{
    QString text = QStringLiteral("Skipped: ")
                   + QString::fromStdWString(file.path.native());
    if (!file.error_message.isEmpty()) {
        text += QStringLiteral("\n") + file.error_message;
    }
    return text;
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    qRegisterMetaType<compresso::ScanResult>("compresso::ScanResult");
    qRegisterMetaType<compresso::CompressionReport>("compresso::CompressionReport");
    qRegisterMetaType<compresso::CompressionProgress>("compresso::CompressionProgress");
    qRegisterMetaType<compresso::OperationError>("compresso::OperationError");

    setWindowTitle("Compresso");
    setMinimumWidth(420);
    resize(520, 270);

    auto* content = new QWidget(this);
    content->setObjectName("mainContent");
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(18, 16, 18, 10);
    layout->setSpacing(12);

    auto* directoryLabel = new QLabel("Game directory:", content);
    directoryPath_ = new QLineEdit(content);
    directoryPath_->setObjectName("gameDirectoryPath");
    directoryPath_->setPlaceholderText("Select a game folder");
    directoryPath_->setClearButtonEnabled(true);
    directoryLabel->setBuddy(directoryPath_);

    browseButton_ = new QPushButton("Browse\u2026", content);
    browseButton_->setObjectName("browseButton");
    connect(browseButton_, &QPushButton::clicked, this, [this] { browseForDirectory(); });

    auto* directoryRow = new QHBoxLayout;
    directoryRow->setSpacing(8);
    directoryRow->addWidget(directoryPath_, 1);
    directoryRow->addWidget(browseButton_);

    auto* separator = new QFrame(content);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);

    auto* details = new QGridLayout;
    details->setHorizontalSpacing(20);
    details->setVerticalSpacing(8);

    const auto addDetail = [content, details](int row, const QString& labelText,
                                              const QString& valueText,
                                              const char* valueObjectName,
                                              QLabel** labelOut = nullptr) {
        auto* label = new QLabel(labelText, content);
        auto* value = new QLabel(valueText, content);
        value->setObjectName(QString::fromLatin1(valueObjectName));
        details->addWidget(label, row, 0);
        details->addWidget(value, row, 1, Qt::AlignLeft);
        if (labelOut != nullptr) {
            *labelOut = label;
        }
        return value;
    };

    originalSizeValue_ = addDetail(0, "Original size (logical)", "\u2014", "originalSizeValue",
                                   &originalSizeLabel_);
    compressedSizeValue_ = addDetail(1, "Allocated size (on disk)", "\u2014",
                                     "compressedSizeValue", &compressedSizeLabel_);
    addDetail(2, "Compression method", "LZX", "compressionMethodValue");
    fileCountValue_ = addDetail(3, "Files scanned", "\u2014", "fileCountValue");
    filesystemValue_ = addDetail(4, "Filesystem / LZX", "\u2014", "filesystemValue");

    analyzeButton_ = new QPushButton("Analyze", content);
    analyzeButton_->setObjectName("analyzeButton");
    analyzeButton_->setEnabled(false);
    compressButton_ = new QPushButton("Compress", content);
    compressButton_->setObjectName("compressButton");
    compressButton_->setEnabled(false);
    restoreButton_ = new QPushButton("Restore", content);
    restoreButton_->setObjectName("restoreButton");
    restoreButton_->setEnabled(false);

    auto* actions = new QHBoxLayout;
    actions->setSpacing(8);
    actions->addStretch();
    actions->addWidget(analyzeButton_);
    actions->addWidget(compressButton_);
    actions->addWidget(restoreButton_);

    layout->addWidget(directoryLabel);
    layout->addLayout(directoryRow);
    layout->addWidget(separator);
    layout->addLayout(details);
    layout->addStretch(1);
    layout->addLayout(actions);
    setCentralWidget(content);

    statusBar()->setObjectName("operationStatus");
    statusBar()->showMessage("Select a game directory to get started.");
    progressBar_ = new QProgressBar(statusBar());
    progressBar_->setObjectName("operationProgress");
    progressBar_->setRange(0, 0);
    progressBar_->setFixedWidth(150);
    progressBar_->setMaximumHeight(16);
    progressBar_->setTextVisible(true);
    progressBar_->setFormat("%p%");
    progressBar_->hide();
    statusBar()->addPermanentWidget(progressBar_);
    detailsButton_ = new QPushButton("Details", statusBar());
    detailsButton_->setObjectName("operationDetailsButton");
    detailsButton_->hide();
    statusBar()->addPermanentWidget(detailsButton_);
    connect(detailsButton_, &QPushButton::clicked, this, [this] { showDiagnostics(); });
    progressTimer_ = new QTimer(this);
    progressTimer_->setInterval(1000);
    connect(progressTimer_, &QTimer::timeout, this, [this] {
        if (hasCompressionProgress_ && operationThread_ && operationThread_->isRunning()) {
            compresso::CompressionProgress progress = latestCompressionProgress_;
            progress.elapsed = std::chrono::milliseconds(compressionClock_.elapsed());
            displayCompressionProgress(progress);
        }
    });

    connect(directoryPath_, &QLineEdit::textChanged, this,
            [this] { handleDirectoryTextChanged(); });
    connect(directoryPath_, &QLineEdit::editingFinished, this,
            [this] { validateDirectoryPath(); });
    connect(analyzeButton_, &QPushButton::clicked, this,
            [this] { startDirectoryScan(); });
    connect(compressButton_, &QPushButton::clicked, this,
            [this] { startCompression(); });
    connect(restoreButton_, &QPushButton::clicked, this,
            [this] { startRestore(); });
}

MainWindow::~MainWindow()
{
    // This also covers application shutdown paths that bypass closeEvent (for example,
    // QApplication::quit()). A running worker is allowed to finish before its QThread dies.
    if (operationThread_ && operationThread_->isRunning()) {
        operationThread_->quit();
        operationThread_->wait();
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (operationThread_ && operationThread_->isRunning()) {
        closeAfterOperation_ = true;
        statusBar()->showMessage("Finishing the current operation before closing…");
        event->ignore();
        return;
    }
    event->accept();
}

void MainWindow::startOperation(compresso::OperationWorker::Kind kind, const QString& directory)
{
    if (operationThread_ && operationThread_->isRunning()) {
        return;
    }

    setOperationControlsEnabled(false);
    activeOperationIsRestore_ = kind == compresso::OperationWorker::Kind::restore;
    diagnosticLines_.clear();
    detailsButton_->hide();
    progressBar_->setRange(0, 0);
    progressBar_->show();

    auto* thread = new QThread(this);
    auto* worker = new compresso::OperationWorker(kind,
        std::filesystem::path(directory.toStdWString()));
    worker->moveToThread(thread);

    connect(thread, &QThread::started, worker, &compresso::OperationWorker::run);
    connect(worker, &compresso::OperationWorker::progressChanged, this,
            [this](const QString& message) { statusBar()->showMessage(message); });
    connect(worker, &compresso::OperationWorker::compressionProgress, this,
            [this](const compresso::CompressionProgress& progress) {
                latestCompressionProgress_ = progress;
                hasCompressionProgress_ = true;
                displayCompressionProgress(progress);
            });
    connect(worker, &compresso::OperationWorker::errorOccurred, this,
            [this](const compresso::OperationError& error) {
                analyzedDirectoryIsEligible_ = false;
                compressButton_->setEnabled(false);
                restoreButton_->setEnabled(false);
                diagnosticLines_ = {diagnosticText(error)};
                detailsButton_->setVisible(true);
                statusBar()->showMessage(QString::fromStdString(
                    compresso::operationErrorMessage(error.code)));
            });
    connect(worker, &compresso::OperationWorker::scanCompleted, this,
            [this, directory](const compresso::ScanResult& result) {
                displayScanResult(result, directory);
            });
    connect(worker, &compresso::OperationWorker::compressionCompleted, this,
            [this, directory](const compresso::CompressionReport& result) {
                progressTimer_->stop();
                displayCompressionResult(result, directory);
            });
    connect(worker, &compresso::OperationWorker::restorationCompleted, this,
            [this, directory](const compresso::CompressionReport& result) {
                progressTimer_->stop();
                displayRestoreResult(result, directory);
            });
    connect(worker, &compresso::OperationWorker::finished, thread, &QThread::quit);
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, thread] {
        if (operationThread_ == thread) {
            operationThread_ = nullptr;
        }
        thread->deleteLater();
        progressTimer_->stop();
        hasCompressionProgress_ = false;
        setOperationControlsEnabled(true);
        progressBar_->hide();
        if (closeAfterOperation_) {
            closeAfterOperation_ = false;
            QMetaObject::invokeMethod(this, &QWidget::close, Qt::QueuedConnection);
        }
    });

    operationThread_ = thread;
    thread->start();
}

void MainWindow::displayCompressionProgress(const compresso::CompressionProgress& progress)
{
    if (!progress.totals_known || progress.total_files == 0) {
        progressBar_->setRange(0, 0);
    } else {
        constexpr int progressScale = 1000;
        progressBar_->setRange(0, progressScale);
        const long double fraction = static_cast<long double>(progress.completed_files)
                                     / static_cast<long double>(progress.total_files);
        progressBar_->setValue(static_cast<int>(fraction * progressScale));
    }

    const auto formatBytes = [](std::uintmax_t bytes) {
        constexpr std::array<const char*, 5> units{"B", "KiB", "MiB", "GiB", "TiB"};
        long double value = static_cast<long double>(bytes);
        std::size_t unit = 0;
        while (value >= 1024.0L && unit + 1 < units.size()) {
            value /= 1024.0L;
            ++unit;
        }
        return QStringLiteral("%1 %2").arg(static_cast<double>(value), 0, 'f', unit == 0 ? 0 : 1)
            .arg(QString::fromLatin1(units[unit]));
    };
    const auto elapsedSeconds = std::chrono::duration_cast<std::chrono::seconds>(
        progress.elapsed).count();
    const auto elapsedMinutes = elapsedSeconds / 60;
    const QString elapsed = elapsedSeconds >= 3600
        ? QStringLiteral("%1:%2:%3")
              .arg(elapsedSeconds / 3600)
              .arg(elapsedMinutes % 60, 2, 10, QLatin1Char('0'))
              .arg(elapsedSeconds % 60, 2, 10, QLatin1Char('0'))
        : QStringLiteral("%1:%2")
              .arg(elapsedMinutes, 2, 10, QLatin1Char('0'))
              .arg(elapsedSeconds % 60, 2, 10, QLatin1Char('0'));
    QString message;
    if (!progress.totals_known) {
        message = activeOperationIsRestore_
            ? QStringLiteral("Scanning restoration targets…")
            : QStringLiteral("Scanning compression targets…");
    } else {
        message = QStringLiteral("%1 %2/%3 files | logical %4 / %5 | elapsed %6")
            .arg(activeOperationIsRestore_ ? QStringLiteral("Restore")
                                           : QStringLiteral("LZX"))
            .arg(static_cast<qulonglong>(progress.completed_files))
            .arg(static_cast<qulonglong>(progress.total_files))
            .arg(formatBytes(progress.processed_logical_bytes),
                 formatBytes(progress.total_logical_bytes))
            .arg(elapsed);
        if (!progress.current_file.empty()) {
            message += QStringLiteral(" | ")
                       + QString::fromStdWString(progress.current_file.filename().native());
        }
    }
    statusBar()->showMessage(message);
}

void MainWindow::setOperationControlsEnabled(bool enabled)
{
    browseButton_->setEnabled(enabled);
    directoryPath_->setEnabled(enabled);
    analyzeButton_->setEnabled(enabled && !validatedDirectory_.isEmpty());
    compressButton_->setEnabled(enabled && analyzedDirectoryIsEligible_);
    restoreButton_->setEnabled(enabled && analyzedDirectoryIsEligible_);
}

void MainWindow::browseForDirectory()
{
    const QString selectedDirectory = QFileDialog::getExistingDirectory(
        this, "Select game directory", directoryPath_->text(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (selectedDirectory.isEmpty()) {
        return;
    }

    directoryPath_->setText(selectedDirectory);
    validateDirectoryPath();
}

void MainWindow::handleDirectoryTextChanged()
{
    validatedDirectory_.clear();
    originalSizeLabel_->setText("Original size (logical)");
    compressedSizeLabel_->setText("Allocated size (on disk)");
    originalSizeValue_->setText("\u2014");
    compressedSizeValue_->setText("\u2014");
    fileCountValue_->setText("\u2014");
    filesystemValue_->setText("\u2014");
    analyzedDirectoryIsEligible_ = false;
    compressButton_->setEnabled(false);
    restoreButton_->setEnabled(false);
    analyzeButton_->setEnabled(false);

    if (directoryPath_->text().isEmpty()) {
        statusBar()->showMessage("Select a game directory to get started.");
    } else {
        statusBar()->showMessage("Directory changed. Press Enter to validate.");
    }
}

void MainWindow::validateDirectoryPath()
{
    if (operationThread_ && operationThread_->isRunning()) {
        return;
    }
    const QString path = directoryPath_->text();
    const QString error = compresso::directoryValidationError(path);
    if (!error.isEmpty()) {
        validatedDirectory_.clear();
        statusBar()->showMessage(error);
        return;
    }

    validatedDirectory_ = path;
    statusBar()->showMessage("Directory is ready. Analyze it before compression.");
    analyzeButton_->setEnabled(true);
}

void MainWindow::startDirectoryScan()
{
    if (operationThread_ && operationThread_->isRunning()) {
        return;
    }
    if (validatedDirectory_.isEmpty() || validatedDirectory_ != directoryPath_->text()) {
        validateDirectoryPath();
    }
    if (validatedDirectory_.isEmpty()) {
        analyzeButton_->setEnabled(false);
        return;
    }
    analyzedDirectoryIsEligible_ = false;
    compressButton_->setEnabled(false);
    restoreButton_->setEnabled(false);
    startOperation(compresso::OperationWorker::Kind::scan, validatedDirectory_);
}

void MainWindow::displayScanResult(const compresso::ScanResult& result,
                                   const QString& directory)
{
    if (directory != directoryPath_->text() || directory != validatedDirectory_) {
        return;
    }

    originalSizeLabel_->setText("Original size (logical)");
    compressedSizeLabel_->setText("Allocated size (on disk)");
    const QLocale locale;
    originalSizeValue_->setText(
        locale.toString(static_cast<qulonglong>(result.total_logical_bytes)) + " bytes");
    compressedSizeValue_->setText(result.allocation_measurements_complete
        ? locale.toString(static_cast<qulonglong>(result.total_allocated_bytes)) + " bytes"
        : "Unavailable");
    fileCountValue_->setText(locale.toString(static_cast<qulonglong>(result.total_file_count)));

    if (result.volume.error) {
        filesystemValue_->setText("Unavailable");
    } else if (result.volume.compression_eligible) {
        filesystemValue_->setText(QString::fromStdWString(result.volume.filesystem_type)
                                  + " (LZX eligible)");
    } else {
        filesystemValue_->setText(QString::fromStdWString(result.volume.filesystem_type)
                                  + " (LZX unavailable)");
    }

    QString message = result.traversal_complete ? "Scan complete: " : "Scan incomplete: ";
    message += locale.toString(static_cast<qulonglong>(result.total_file_count)) + " files";
    if (!result.skipped_entries.empty()) {
        message += "; " + locale.toString(
            static_cast<qulonglong>(result.skipped_entries.size())) + " items skipped";
    }
    if (!result.allocation_measurements_complete) {
        message += "; allocated size unavailable for " + locale.toString(
            static_cast<qulonglong>(result.allocation_measurement_failures.size())) + " files";
    }
    diagnosticLines_.clear();
    for (const auto& error : result.errors) {
        diagnosticLines_.push_back(diagnosticText(error));
    }
    for (const auto& entry : result.skipped_entries) {
        diagnosticLines_.push_back(skippedEntryText(entry));
    }
    detailsButton_->setVisible(!diagnosticLines_.isEmpty());
    if (!result.volume.compression_eligible && !result.volume.error) {
        message += "; LZX unavailable on this volume";
    }
    message += ".";
    if (!diagnosticLines_.isEmpty()) message += " See Details.";
    statusBar()->showMessage(message);
    analyzedDirectoryIsEligible_ = result.volume.compression_eligible
                                   && result.total_file_count > 0;
    restoreButton_->setEnabled(analyzedDirectoryIsEligible_);
}

void MainWindow::startCompression()
{
    if (operationThread_ && operationThread_->isRunning()) {
        return;
    }
    if (!analyzedDirectoryIsEligible_ || validatedDirectory_.isEmpty()
        || validatedDirectory_ != directoryPath_->text()) {
        return;
    }
    analyzedDirectoryIsEligible_ = false;
    compressionClock_.start();
    hasCompressionProgress_ = false;
    progressTimer_->start();
    startOperation(compresso::OperationWorker::Kind::compress, validatedDirectory_);
}

void MainWindow::startRestore()
{
    if (operationThread_ && operationThread_->isRunning()) {
        return;
    }
    if (!analyzedDirectoryIsEligible_ || validatedDirectory_.isEmpty()
        || validatedDirectory_ != directoryPath_->text()) {
        return;
    }

    const auto answer = QMessageBox::question(
        this, "Restore files",
        "Remove transparent LZX compression from supported files in this game folder?\n"
        "Files stay in place and keep their contents.",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }

    analyzedDirectoryIsEligible_ = false;
    compressionClock_.start();
    hasCompressionProgress_ = false;
    progressTimer_->start();
    startOperation(compresso::OperationWorker::Kind::restore, validatedDirectory_);
}

void MainWindow::displayCompressionResult(const compresso::CompressionReport& result,
                                          const QString& directory)
{
    if (directory != directoryPath_->text() || directory != validatedDirectory_) {
        return;
    }

    if (!result.target_eligible) {
        diagnosticLines_.clear();
        if (result.error) {
            diagnosticLines_.push_back(diagnosticText(*result.error));
        }
        detailsButton_->setVisible(!diagnosticLines_.isEmpty());
        QString message = result.rejection_reason.isEmpty()
            ? "The selected directory is not eligible for LZX compression."
            : result.rejection_reason;
        if (!diagnosticLines_.isEmpty()) message += " See Details.";
        statusBar()->showMessage(message);
        analyzedDirectoryIsEligible_ = false;
        return;
    }

    const QLocale locale;
    analyzedDirectoryIsEligible_ = false;
    diagnosticLines_.clear();
    for (const auto& issue : result.issues) {
        diagnosticLines_.push_back(diagnosticText(issue));
    }
    for (const auto& file : result.files) {
        if (file.state == compresso::CompressionFileState::failed && file.error) {
            diagnosticLines_.push_back(diagnosticText(*file.error));
        } else if (file.state == compresso::CompressionFileState::skipped) {
            diagnosticLines_.push_back(file.error ? diagnosticText(*file.error)
                                                  : skippedFileText(file));
        }
    }
    if (result.size_summary) {
        const auto& summary = *result.size_summary;
        originalSizeLabel_->setText("Original allocated size");
        compressedSizeLabel_->setText("Final allocated size");
        originalSizeValue_->setText(
            locale.toString(static_cast<qulonglong>(summary.original_allocated_bytes))
            + " bytes");
        compressedSizeValue_->setText(
            locale.toString(static_cast<qulonglong>(summary.final_allocated_bytes))
            + " bytes");
    } else {
        originalSizeLabel_->setText("Original allocated size");
        compressedSizeLabel_->setText("Final allocated size");
        originalSizeValue_->setText("Unavailable");
        compressedSizeValue_->setText("Unavailable");
        if (result.size_measurement_error) {
            diagnosticLines_.push_back(diagnosticText(*result.size_measurement_error));
        }
    }
    detailsButton_->setVisible(!diagnosticLines_.isEmpty());

    QString message = "LZX: "
        + locale.toString(static_cast<qulonglong>(result.compressedCount())) + " compressed, "
        + locale.toString(static_cast<qulonglong>(result.skippedCount())) + " skipped, "
        + locale.toString(static_cast<qulonglong>(result.failedCount())) + " failed. ";
    if (result.size_summary) {
        const auto& summary = *result.size_summary;
        if (summary.bytes_increased > 0) {
            message += "Allocated usage increased by "
                + locale.toString(static_cast<qulonglong>(summary.bytes_increased)) + " bytes";
            if (summary.percentage_available) {
                message += " (" + locale.toString(
                    static_cast<double>(-summary.percentage_saved), 'f', 1) + "% more)";
            } else {
                message += " (percentage unavailable for zero original allocation)";
            }
        } else {
            message += "Saved "
                + locale.toString(static_cast<qulonglong>(summary.bytes_saved)) + " bytes";
            if (summary.percentage_available) {
                message += " (" + locale.toString(
                    static_cast<double>(summary.percentage_saved), 'f', 1) + "% allocated)";
            }
        }
    } else {
        message += "Allocated size unavailable (scan incomplete)";
    }
    message += ".";
    if (!diagnosticLines_.isEmpty()) message += " See Details.";
    statusBar()->showMessage(message);
}

void MainWindow::displayRestoreResult(const compresso::CompressionReport& result,
                                      const QString& directory)
{
    if (directory != directoryPath_->text() || directory != validatedDirectory_) {
        return;
    }

    if (!result.target_eligible) {
        diagnosticLines_.clear();
        if (result.error) {
            diagnosticLines_.push_back(diagnosticText(*result.error));
        }
        detailsButton_->setVisible(!diagnosticLines_.isEmpty());
        QString message = result.rejection_reason.isEmpty()
            ? "The selected directory is not eligible for LZX restoration."
            : result.rejection_reason;
        if (!diagnosticLines_.isEmpty()) message += " See Details.";
        statusBar()->showMessage(message);
        analyzedDirectoryIsEligible_ = false;
        return;
    }

    const QLocale locale;
    compressedSizeValue_->setText("\u2014");
    analyzedDirectoryIsEligible_ = false;
    diagnosticLines_.clear();
    for (const auto& issue : result.issues) {
        diagnosticLines_.push_back(diagnosticText(issue));
    }
    for (const auto& file : result.files) {
        if (file.state == compresso::CompressionFileState::failed && file.error) {
            diagnosticLines_.push_back(diagnosticText(*file.error));
        } else if (file.state == compresso::CompressionFileState::skipped) {
            diagnosticLines_.push_back(file.error ? diagnosticText(*file.error)
                                                  : skippedFileText(file));
        }
    }
    detailsButton_->setVisible(!diagnosticLines_.isEmpty());

    QString message = "Restore: "
        + locale.toString(static_cast<qulonglong>(result.restoredCount())) + " restored";
    if (result.skippedCount() > 0) {
        message += ", " + locale.toString(static_cast<qulonglong>(result.skippedCount()))
                   + " skipped";
        if (result.failedCount() == 0) {
            const auto skippedIssue = std::find_if(result.files.begin(), result.files.end(),
                [](const auto& file) {
                    return file.state == compresso::CompressionFileState::skipped
                           && file.error.has_value();
                });
            if (skippedIssue != result.files.end()) {
                message += ". " + skippedIssue->error_message;
            }
        }
    }
    if (result.failedCount() > 0) {
        message += ", " + locale.toString(static_cast<qulonglong>(result.failedCount()))
                   + " failed";
        const auto firstFailure = std::find_if(result.files.begin(), result.files.end(),
            [](const auto& file) { return file.state == compresso::CompressionFileState::failed; });
        if (firstFailure != result.files.end() && firstFailure->error) {
            message += ". " + firstFailure->error_message;
        }
    }
    message += ". Analyze again to refresh allocated size.";
    if (!diagnosticLines_.isEmpty()) message += " See Details.";
    statusBar()->showMessage(message);
}

void MainWindow::showDiagnostics()
{
    if (diagnosticLines_.isEmpty()) {
        return;
    }

    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("Operation details");
    dialog->resize(620, 360);
    auto* layout = new QVBoxLayout(dialog);
    auto* details = new QPlainTextEdit(dialog);
    details->setObjectName("operationDiagnosticLog");
    details->setReadOnly(true);
    details->setPlainText(diagnosticLines_.join("\n\n"));
    layout->addWidget(details);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::close);
    layout->addWidget(buttons);
    dialog->show();
}
