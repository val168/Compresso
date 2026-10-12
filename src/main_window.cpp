#include "compresso/main_window.h"
#include "compresso/directory_validator.h"

#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("Compresso");
    setMinimumWidth(420);
    resize(520, 250);

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

    auto* browseButton = new QPushButton("Browse…", content);
    browseButton->setObjectName("browseButton");
    connect(browseButton, &QPushButton::clicked, this, [this] { browseForDirectory(); });

    auto* directoryRow = new QHBoxLayout;
    directoryRow->setSpacing(8);
    directoryRow->addWidget(directoryPath_, 1);
    directoryRow->addWidget(browseButton);

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

    originalSizeValue_ = addDetail(0, "Original size", "—", "originalSizeValue");
    compressedSizeValue_ = addDetail(1, "Compressed size", "—", "compressedSizeValue");
    addDetail(2, "Compression method", "LZX", "compressionMethodValue");

    auto* analyzeButton = new QPushButton("Analyze", content);
    analyzeButton->setObjectName("analyzeButton");
    analyzeButton->setEnabled(false);
    auto* compressButton = new QPushButton("Compress", content);
    compressButton->setObjectName("compressButton");
    compressButton->setEnabled(false);
    auto* restoreButton = new QPushButton("Restore", content);
    restoreButton->setObjectName("restoreButton");
    restoreButton->setEnabled(false);

    auto* actions = new QHBoxLayout;
    actions->setSpacing(8);
    actions->addStretch();
    actions->addWidget(analyzeButton);
    actions->addWidget(compressButton);
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
    statusBar()->showMessage("Directory is ready. Compression is not implemented.");
}
