#include "compresso/main_window.h"

#include "compresso/directory_validator.h"
#include "compresso/operation_worker.h"

#include <QCloseEvent>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QLineEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QThread>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    qRegisterMetaType<compresso::ScanResult>("compresso::ScanResult");
    qRegisterMetaType<compresso::CompressionReport>("compresso::CompressionReport");

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
                                              const char* valueObjectName) {
        auto* label = new QLabel(labelText, content);
        auto* value = new QLabel(valueText, content);
        value->setObjectName(QString::fromLatin1(valueObjectName));
        details->addWidget(label, row, 0);
        details->addWidget(value, row, 1, Qt::AlignLeft);
        return value;
    };

    originalSizeValue_ = addDetail(0, "Original size (logical)", "\u2014", "originalSizeValue");
    compressedSizeValue_ = addDetail(1, "Allocated size (on disk)", "\u2014",
                                     "compressedSizeValue");
    addDetail(2, "Compression method", "LZX", "compressionMethodValue");
    fileCountValue_ = addDetail(3, "Files scanned", "\u2014", "fileCountValue");
    filesystemValue_ = addDetail(4, "Filesystem / LZX", "\u2014", "filesystemValue");

    analyzeButton_ = new QPushButton("Analyze", content);
    analyzeButton_->setObjectName("analyzeButton");
    analyzeButton_->setEnabled(false);
    compressButton_ = new QPushButton("Compress", content);
    compressButton_->setObjectName("compressButton");
    compressButton_->setEnabled(false);
    auto* restoreButton = new QPushButton("Restore", content);
    restoreButton->setObjectName("restoreButton");
    restoreButton->setEnabled(false);

    auto* actions = new QHBoxLayout;
    actions->setSpacing(8);
    actions->addStretch();
    actions->addWidget(analyzeButton_);
    actions->addWidget(compressButton_);
    actions->addWidget(restoreButton);

    layout->addWidget(directoryLabel);
    layout->addLayout(directoryRow);
    layout->addWidget(separator);
    layout->addLayout(details);
    layout->addStretch(1);
    layout->addLayout(actions);
    setCentralWidget(content);

    statusBar()->setObjectName("operationStatus");
    statusBar()->showMessage("Select a game directory to get started.");

    connect(directoryPath_, &QLineEdit::textChanged, this,
            [this] { handleDirectoryTextChanged(); });
    connect(directoryPath_, &QLineEdit::editingFinished, this,
            [this] { validateDirectoryPath(); });
    connect(analyzeButton_, &QPushButton::clicked, this,
            [this] { startDirectoryScan(); });
    connect(compressButton_, &QPushButton::clicked, this,
            [this] { startCompression(); });
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

void MainWindow::startOperation(bool compressing, const QString& directory)
{
    if (operationThread_ && operationThread_->isRunning()) {
        return;
    }

    setOperationControlsEnabled(false);

    auto* thread = new QThread(this);
    auto* worker = new compresso::OperationWorker(
        compressing ? compresso::OperationWorker::Kind::compress
                    : compresso::OperationWorker::Kind::scan,
        std::filesystem::path(directory.toStdWString()));
    worker->moveToThread(thread);

    connect(thread, &QThread::started, worker, &compresso::OperationWorker::run);
    connect(worker, &compresso::OperationWorker::progressChanged, this,
            [this](const QString& message) { statusBar()->showMessage(message); });
    connect(worker, &compresso::OperationWorker::errorOccurred, this,
            [this](const QString& message) {
                analyzedDirectoryIsEligible_ = false;
                compressButton_->setEnabled(false);
                statusBar()->showMessage(message);
            });
    connect(worker, &compresso::OperationWorker::scanCompleted, this,
            [this, directory](const compresso::ScanResult& result) {
                displayScanResult(result, directory);
            });
    connect(worker, &compresso::OperationWorker::compressionCompleted, this,
            [this, directory](const compresso::CompressionReport& result) {
                displayCompressionResult(result, directory);
            });
    connect(worker, &compresso::OperationWorker::finished, thread, &QThread::quit);
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, thread] {
        if (operationThread_ == thread) {
            operationThread_ = nullptr;
        }
        thread->deleteLater();
        setOperationControlsEnabled(true);
        if (closeAfterOperation_) {
            closeAfterOperation_ = false;
            QMetaObject::invokeMethod(this, &QWidget::close, Qt::QueuedConnection);
        }
    });

    operationThread_ = thread;
    thread->start();
}

void MainWindow::setOperationControlsEnabled(bool enabled)
{
    browseButton_->setEnabled(enabled);
    directoryPath_->setEnabled(enabled);
    analyzeButton_->setEnabled(enabled && !validatedDirectory_.isEmpty());
    compressButton_->setEnabled(enabled && analyzedDirectoryIsEligible_);
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
    originalSizeValue_->setText("\u2014");
    compressedSizeValue_->setText("\u2014");
    fileCountValue_->setText("\u2014");
    filesystemValue_->setText("\u2014");
    analyzedDirectoryIsEligible_ = false;
    compressButton_->setEnabled(false);
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
    startOperation(false, validatedDirectory_);
}

void MainWindow::displayScanResult(const compresso::ScanResult& result,
                                   const QString& directory)
{
    if (directory != directoryPath_->text() || directory != validatedDirectory_) {
        return;
    }

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
    message += ".";
    statusBar()->showMessage(message);
    analyzedDirectoryIsEligible_ = result.volume.compression_eligible
                                   && result.total_file_count > 0;
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
    startOperation(true, validatedDirectory_);
}

void MainWindow::displayCompressionResult(const compresso::CompressionReport& result,
                                          const QString& directory)
{
    if (directory != directoryPath_->text() || directory != validatedDirectory_) {
        return;
    }

    if (!result.target_eligible) {
        statusBar()->showMessage(result.rejection_reason.isEmpty()
            ? "The selected directory is not eligible for LZX compression."
            : result.rejection_reason);
        analyzedDirectoryIsEligible_ = false;
        return;
    }

    const QLocale locale;
    compressedSizeValue_->setText("\u2014");
    analyzedDirectoryIsEligible_ = false;

    QString message = "LZX: "
        + locale.toString(static_cast<qulonglong>(result.compressedCount())) + " compressed";
    if (result.skippedCount() > 0) {
        message += ", " + locale.toString(static_cast<qulonglong>(result.skippedCount()))
                   + " skipped";
    }
    if (result.failedCount() > 0) {
        message += ", " + locale.toString(static_cast<qulonglong>(result.failedCount()))
                   + " failed";
    }
    message += ". Analyze again to refresh allocated size.";
    statusBar()->showMessage(message);
}
