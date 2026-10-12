#include "compresso/main_window.h"
#include "compresso/directory_validator.h"

#include <QFutureWatcher>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QLineEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrent/QtConcurrentRun>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
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

    browseButton_ = new QPushButton("Browse…", content);
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

    originalSizeValue_ = addDetail(0, "Original size (logical)", "—", "originalSizeValue");
    compressedSizeValue_ = addDetail(1, "Allocated size (on disk)", "—",
                                     "compressedSizeValue");
    addDetail(2, "Compression method", "LZX", "compressionMethodValue");
    fileCountValue_ = addDetail(3, "Files scanned", "—", "fileCountValue");
    filesystemValue_ = addDetail(4, "Filesystem / LZX", "—", "filesystemValue");

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

    scanWatcher_ = new QFutureWatcher<compresso::ScanResult>(this);
    compressionWatcher_ = new QFutureWatcher<compresso::CompressionReport>(this);

    connect(directoryPath_, &QLineEdit::textChanged, this,
            [this] { handleDirectoryTextChanged(); });
    connect(directoryPath_, &QLineEdit::editingFinished, this,
            [this] { validateDirectoryPath(); });
    connect(analyzeButton_, &QPushButton::clicked, this,
            [this] { startDirectoryScan(); });
    connect(scanWatcher_, &QFutureWatcher<compresso::ScanResult>::finished, this,
            [this] { displayScanResult(); });
    connect(compressButton_, &QPushButton::clicked, this,
            [this] { startCompression(); });
    connect(compressionWatcher_, &QFutureWatcher<compresso::CompressionReport>::finished, this,
            [this] { displayCompressionResult(); });
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
    originalSizeValue_->setText("—");
    compressedSizeValue_->setText("—");
    fileCountValue_->setText("—");
    filesystemValue_->setText("—");
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
    const QString path = directoryPath_->text();
    const QString error = compresso::directoryValidationError(path);
    if (!error.isEmpty()) {
        validatedDirectory_.clear();
        statusBar()->showMessage(error);
        return;
    }

    validatedDirectory_ = path;
    statusBar()->showMessage("Directory is ready. Analyze it before compression.");
    analyzeButton_->setEnabled(!scanWatcher_->isRunning());
}

void MainWindow::startDirectoryScan()
{
    if (scanWatcher_->isRunning()) {
        return;
    }
    if (validatedDirectory_.isEmpty() || validatedDirectory_ != directoryPath_->text()) {
        validateDirectoryPath();
    }
    if (validatedDirectory_.isEmpty()) {
        analyzeButton_->setEnabled(false);
        return;
    }

    scanningDirectory_ = validatedDirectory_;
    analyzeButton_->setEnabled(false);
    statusBar()->showMessage("Scanning directory…");

    const std::filesystem::path root(scanningDirectory_.toStdWString());
    scanWatcher_->setFuture(QtConcurrent::run([root] {
        return compresso::DirectoryScanner::scan(root);
    }));
}

void MainWindow::displayScanResult()
{
    const compresso::ScanResult result = scanWatcher_->result();
    const QString scannedDirectory = scanningDirectory_;
    scanningDirectory_.clear();

    if (scannedDirectory != directoryPath_->text()
        || scannedDirectory != validatedDirectory_) {
        analyzeButton_->setEnabled(!validatedDirectory_.isEmpty());
        return;
    }

    const QLocale locale;
    originalSizeValue_->setText(
        locale.toString(static_cast<qulonglong>(result.total_logical_bytes)) + " bytes");
    if (result.allocation_measurements_complete) {
        compressedSizeValue_->setText(
            locale.toString(static_cast<qulonglong>(result.total_allocated_bytes)) + " bytes");
    } else {
        compressedSizeValue_->setText("Unavailable");
    }
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
        message += "; "
            + locale.toString(static_cast<qulonglong>(result.skipped_entries.size()))
            + " items skipped";
    }
    if (!result.allocation_measurements_complete) {
        message += "; allocated size unavailable for "
            + locale.toString(static_cast<qulonglong>(
                  result.allocation_measurement_failures.size()))
            + " files";
    }
    message += ".";
    statusBar()->showMessage(message);
    analyzeButton_->setEnabled(true);
    analyzedDirectoryIsEligible_ = result.volume.compression_eligible
                                   && result.total_file_count > 0;
    compressButton_->setEnabled(analyzedDirectoryIsEligible_);
}

void MainWindow::startCompression()
{
    if (!analyzedDirectoryIsEligible_ || !compressingDirectory_.isEmpty()
        || validatedDirectory_.isEmpty() || validatedDirectory_ != directoryPath_->text()) {
        return;
    }

    compressingDirectory_ = validatedDirectory_;
    browseButton_->setEnabled(false);
    directoryPath_->setEnabled(false);
    analyzeButton_->setEnabled(false);
    compressButton_->setEnabled(false);
    statusBar()->showMessage("Applying transparent NTFS LZX compression…");

    const std::filesystem::path root(compressingDirectory_.toStdWString());
    compressionWatcher_->setFuture(QtConcurrent::run([root] {
        return compresso::CompressionBackend::compressDirectory(root);
    }));
}

void MainWindow::displayCompressionResult()
{
    const compresso::CompressionReport result = compressionWatcher_->result();
    const QString targetDirectory = compressingDirectory_;
    compressingDirectory_.clear();
    browseButton_->setEnabled(true);
    directoryPath_->setEnabled(true);

    if (targetDirectory != directoryPath_->text() || targetDirectory != validatedDirectory_) {
        analyzeButton_->setEnabled(!validatedDirectory_.isEmpty());
        return;
    }

    if (!result.target_eligible) {
        statusBar()->showMessage(result.rejection_reason.isEmpty()
                                     ? "The selected directory is not eligible for LZX compression."
                                     : result.rejection_reason);
        analyzedDirectoryIsEligible_ = false;
        analyzeButton_->setEnabled(true);
        compressButton_->setEnabled(false);
        return;
    }

    const QLocale locale;
    compressedSizeValue_->setText("—");
    analyzedDirectoryIsEligible_ = false;
    analyzeButton_->setEnabled(true);
    compressButton_->setEnabled(false);

    QString message = "LZX: "
        + locale.toString(static_cast<qulonglong>(result.compressedCount())) + " compressed";
    if (result.skippedCount() > 0) {
        message += ", "
            + locale.toString(static_cast<qulonglong>(result.skippedCount())) + " skipped";
    }
    if (result.failedCount() > 0) {
        message += ", "
            + locale.toString(static_cast<qulonglong>(result.failedCount())) + " failed";
    }
    message += ". Analyze again to refresh allocated size.";
    statusBar()->showMessage(message);
}
