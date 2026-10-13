#include "compresso/main_window.h"

#include "compresso/directory_validator.h"
#include "compresso/operation_worker.h"

#include <QCloseEvent>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
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
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QTableView>
#include <QStandardItemModel>
#include <QSortFilterProxyModel>
#include <QComboBox>
#include <QHeaderView>
#include <QStyle>
#include <QItemSelectionModel>
#include <QFileInfo>
#include <QDesktopServices>
#include <QUrl>

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

QString compactBytes(std::uintmax_t bytes)
{
    constexpr std::array<const char*, 6> units{"B", "KB", "MB", "GB", "TB", "PB"};
    long double value = static_cast<long double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0L && unit + 1 < units.size()) {
        value /= 1024.0L;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(static_cast<double>(value), 0, 'f', unit == 0 ? 0 : 1)
        .arg(QString::fromLatin1(units[unit]));
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    themeManager_ = new compresso::ThemeManager(
        *qobject_cast<QApplication*>(QApplication::instance()), this);
    setPalette(qApp->palette());
    connect(themeManager_, &compresso::ThemeManager::paletteApplied,
            this, [this](const QPalette& palette) {
                setPalette(palette);
                if (directoryPath_) directoryPath_->setPalette(palette);
                if (progressBar_) progressBar_->setPalette(palette);
            });
    qRegisterMetaType<compresso::ScanResult>("compresso::ScanResult");
    qRegisterMetaType<compresso::CompressionReport>("compresso::CompressionReport");
    qRegisterMetaType<compresso::CompressionProgress>("compresso::CompressionProgress");
    qRegisterMetaType<compresso::OperationError>("compresso::OperationError");

    setWindowTitle("Compresso");
    setMinimumWidth(420);
    resize(520, 270);
    QSettings settings;
    const QByteArray savedGeometry = settings.value(QStringLiteral("window/geometry"))
                                         .toByteArray();

    auto* appearanceMenu = menuBar()->addMenu(tr("Appearance"));
    auto* themeActions = new QActionGroup(this);
    themeActions->setExclusive(true);
    const auto addThemeAction = [this, appearanceMenu, themeActions](const QString& text,
                                                                     const char* name,
                                                                     compresso::ThemePreference preference) {
        auto* action = appearanceMenu->addAction(text);
        action->setObjectName(QString::fromLatin1(name));
        action->setCheckable(true);
        themeActions->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, preference] { themeManager_->setPreference(preference); });
        return action;
    };
    systemThemeAction_ = addThemeAction(tr("System"), "themeSystemAction",
                                        compresso::ThemePreference::system);
    lightThemeAction_ = addThemeAction(tr("Light"), "themeLightAction",
                                       compresso::ThemePreference::light);
    darkThemeAction_ = addThemeAction(tr("Dark"), "themeDarkAction",
                                      compresso::ThemePreference::dark);
    const auto updateThemeChecks = [this](compresso::ThemePreference preference) {
        systemThemeAction_->setChecked(preference == compresso::ThemePreference::system);
        lightThemeAction_->setChecked(preference == compresso::ThemePreference::light);
        darkThemeAction_->setChecked(preference == compresso::ThemePreference::dark);
    };
    updateThemeChecks(themeManager_->preference());
    connect(themeManager_, &compresso::ThemeManager::preferenceChanged,
            this, updateThemeChecks);

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
    cancelButton_ = new QPushButton("Cancel", content);
    cancelButton_->setObjectName("cancelButton");
    cancelButton_->hide();
    connect(cancelButton_, &QPushButton::clicked, this, [this] { cancelOperation(); });

    auto* actions = new QHBoxLayout;
    actions->setSpacing(8);
    actions->addStretch();
    actions->addWidget(cancelButton_);
    actions->addWidget(analyzeButton_);
    actions->addWidget(compressButton_);
    actions->addWidget(restoreButton_);

    auto* openFolderButton = new QPushButton(QStringLiteral("Open folder"), content);
    openFolderButton->setObjectName(QStringLiteral("openFolderButton"));
    connect(openFolderButton, &QPushButton::clicked, this, [this] {
        if (!validatedDirectory_.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(validatedDirectory_));
    });

    layout->addWidget(directoryLabel);
    layout->addLayout(directoryRow);
    layout->addWidget(separator);
    layout->addLayout(details);
    layout->addStretch(1);
    layout->addLayout(actions);
    layout->addWidget(openFolderButton);
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

    // Compact library shell. The existing directory controls remain the manual-add
    // fallback and continue to drive the same validated worker/backend operations.
    auto* previousContent = takeCentralWidget();
    auto* shell = new QWidget(this);
    shell->setObjectName(QStringLiteral("libraryShell"));
    auto* shellLayout = new QHBoxLayout(shell);
    shellLayout->setContentsMargins(0, 0, 0, 0);
    shellLayout->setSpacing(0);

    auto* sidebar = new QWidget(shell);
    sidebar->setObjectName(QStringLiteral("sidebar"));
    sidebar->setFixedWidth(176);
    auto* sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(12, 14, 12, 12);
    sideLayout->setSpacing(8);
    auto* brand = new QLabel(QStringLiteral("◈  Compresso"), sidebar);
    brand->setObjectName(QStringLiteral("brand"));
    sideLayout->addWidget(brand);
    sideLayout->addSpacing(14);
    const auto addNavButton = [sidebar, sideLayout](const QString& label,
                                                    const QString& name,
                                                    QStyle::StandardPixmap icon) {
        auto* button = new QPushButton(sidebar->style()->standardIcon(icon), label, sidebar);
        button->setObjectName(name);
        button->setCheckable(true);
        button->setFlat(true);
        button->setMinimumHeight(38);
        sideLayout->addWidget(button);
        return button;
    };
    auto* libraryNav = addNavButton(QStringLiteral("Library"), QStringLiteral("libraryNav"),
                                    QStyle::SP_DirHomeIcon);
    libraryNav->setChecked(true);
    auto* queueNav = addNavButton(QStringLiteral("Queue"), QStringLiteral("queueNav"),
                                  QStyle::SP_BrowserReload);
    auto* settingsNav = addNavButton(QStringLiteral("Settings"), QStringLiteral("settingsNav"),
                                     QStyle::SP_FileDialogDetailedView);
    sideLayout->addStretch(1);
    connect(settingsNav, &QPushButton::clicked, this, [this, settingsNav] {
        settingsNav->setChecked(true);
        if (!menuBar()->actions().isEmpty() && menuBar()->actions().front()->menu())
            menuBar()->actions().front()->menu()->popup(
                settingsNav->mapToGlobal(QPoint(settingsNav->width(), 0)));
    });
    connect(queueNav, &QPushButton::clicked, this, [this] {
        statusBar()->showMessage(QStringLiteral("No queued operations."));
    });
    connect(libraryNav, &QPushButton::clicked, this, [libraryNav, queueNav, settingsNav] {
        libraryNav->setChecked(true);
        queueNav->setChecked(false);
        settingsNav->setChecked(false);
    });
    connect(queueNav, &QPushButton::clicked, this, [libraryNav, queueNav, settingsNav] {
        libraryNav->setChecked(false);
        queueNav->setChecked(true);
        settingsNav->setChecked(false);
    });

    auto* center = new QWidget(shell);
    center->setObjectName(QStringLiteral("libraryCenter"));
    auto* centerLayout = new QVBoxLayout(center);
    centerLayout->setContentsMargins(16, 14, 4, 12);
    centerLayout->setSpacing(10);
    auto* toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);
    searchField_ = new QLineEdit(center);
    searchField_->setObjectName(QStringLiteral("gameSearch"));
    searchField_->setPlaceholderText(QStringLiteral("Search games…"));
    searchField_->setClearButtonEnabled(true);
    launcherFilter_ = new QComboBox(center);
    launcherFilter_->setObjectName(QStringLiteral("launcherFilter"));
    launcherFilter_->addItem(QStringLiteral("All launchers"));
    driveFilter_ = new QComboBox(center);
    driveFilter_->setObjectName(QStringLiteral("driveFilter"));
    driveFilter_->addItem(QStringLiteral("All drives"));
    statusFilter_ = new QComboBox(center);
    statusFilter_->setObjectName(QStringLiteral("statusFilter"));
    statusFilter_->addItems({QStringLiteral("All games"), QStringLiteral("Compressed"),
                             QStringLiteral("Uncompressed"), QStringLiteral("Partial")});
    toolbar->addWidget(searchField_, 1);
    toolbar->addWidget(launcherFilter_);
    toolbar->addWidget(driveFilter_);
    toolbar->addWidget(statusFilter_);
    auto* refresh = new QPushButton(QStringLiteral("Refresh"), center);
    refresh->setObjectName(QStringLiteral("libraryRefreshButton"));
    toolbar->addWidget(refresh);
    connect(refresh, &QPushButton::clicked, this, [this] { startDirectoryScan(); });
    centerLayout->addLayout(toolbar);

    auto* stats = new QHBoxLayout;
    stats->setSpacing(8);
    const auto addStat = [center, stats](const QString& title, const QString& name) {
        auto* box = new QFrame(center);
        box->setObjectName(QStringLiteral("statBox"));
        auto* boxLayout = new QVBoxLayout(box);
        boxLayout->setContentsMargins(12, 7, 12, 7);
        boxLayout->setSpacing(1);
        auto* value = new QLabel(QStringLiteral("—"), box);
        value->setObjectName(name);
        auto* caption = new QLabel(title, box);
        caption->setObjectName(QStringLiteral("statCaption"));
        boxLayout->addWidget(value);
        boxLayout->addWidget(caption);
        stats->addWidget(box, 1);
        return value;
    };
    gamesStat_ = addStat(QStringLiteral("Games"), QStringLiteral("gamesStat"));
    totalSizeStat_ = addStat(QStringLiteral("Total size"), QStringLiteral("totalSizeStat"));
    onDiskStat_ = addStat(QStringLiteral("On disk"), QStringLiteral("onDiskStat"));
    savedStat_ = addStat(QStringLiteral("Saved"), QStringLiteral("savedStat"));
    centerLayout->addLayout(stats);

    gameModel_ = new QStandardItemModel(0, 8, this);
    gameModel_->setHorizontalHeaderLabels({QStringLiteral("Game"), QStringLiteral("Launcher"),
        QStringLiteral("Drive"), QStringLiteral("Size"), QStringLiteral("On disk"),
        QStringLiteral("Saved"), QStringLiteral("Status"), QStringLiteral("Action")});
    gameProxy_ = new QSortFilterProxyModel(this);
    gameProxy_->setSourceModel(gameModel_);
    gameProxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    gameProxy_->setFilterKeyColumn(0);
    gameTable_ = new QTableView(center);
    gameTable_->setObjectName(QStringLiteral("gameTable"));
    gameTable_->setModel(gameProxy_);
    gameTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    gameTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    gameTable_->setAlternatingRowColors(true);
    gameTable_->setShowGrid(false);
    gameTable_->verticalHeader()->hide();
    gameTable_->horizontalHeader()->setStretchLastSection(false);
    gameTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 8; ++column)
        gameTable_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    gameTable_->setSortingEnabled(true);
    centerLayout->addWidget(gameTable_, 1);
    connect(searchField_, &QLineEdit::textChanged, gameProxy_, &QSortFilterProxyModel::setFilterFixedString);
    connect(statusFilter_, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        if (gameModel_->rowCount() == 0) return;
        if (text == QStringLiteral("All games")) gameProxy_->setFilterKeyColumn(0);
        else gameProxy_->setFilterKeyColumn(6);
        gameProxy_->setFilterFixedString(text == QStringLiteral("All games")
                                             ? searchField_->text() : text);
    });
    connect(driveFilter_, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        gameProxy_->setFilterKeyColumn(text == QStringLiteral("All drives") ? 0 : 2);
        gameProxy_->setFilterFixedString(text == QStringLiteral("All drives")
                                             ? searchField_->text() : text);
    });
    connect(gameTable_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        if (index.column() == 7) {
            if (compressButton_->isEnabled()) startCompression();
            else if (restoreButton_->isEnabled()) startRestore();
        }
    });

    previousContent->setObjectName(QStringLiteral("gameDetails"));
    selectedGameName_ = new QLabel(QStringLiteral("No game selected"), previousContent);
    selectedGameName_->setObjectName(QStringLiteral("selectedGameName"));
    selectedGameName_->setStyleSheet(QStringLiteral("font-size:20px;font-weight:650;"));
    selectedLauncher_ = new QLabel(QStringLiteral("Manual folder"), previousContent);
    selectedLauncher_->setObjectName(QStringLiteral("selectedLauncher"));
    selectedStatus_ = new QLabel(QStringLiteral("Unknown"), previousContent);
    selectedStatus_->setObjectName(QStringLiteral("selectedStatus"));
    previousContent->setMinimumWidth(300);
    previousContent->setMaximumWidth(380);
    if (auto* oldLayout = qobject_cast<QVBoxLayout*>(previousContent->layout())) {
        oldLayout->setContentsMargins(16, 18, 16, 12);
        oldLayout->setSpacing(9);
        oldLayout->insertWidget(0, selectedGameName_);
        oldLayout->insertWidget(1, selectedLauncher_);
        oldLayout->insertWidget(2, selectedStatus_);
    }
    shellLayout->addWidget(sidebar);
    shellLayout->addWidget(center, 1);
    shellLayout->addWidget(previousContent);
    setCentralWidget(shell);
    setMinimumSize(420, 300);
    resize(1480, 900);
    setStyleSheet(QStringLiteral(
        "QMainWindow,#libraryShell{background:#101417;color:#edf2f6;}"
        "#sidebar,#gameDetails{background:#12181c;border-right:1px solid #242d33;}"
        "#brand{font-size:18px;font-weight:650;padding:8px 4px;}"
        "QPushButton{background:#20282e;border:1px solid #303b43;border-radius:5px;padding:7px 12px;}"
        "QPushButton:hover{background:#29343b;} QPushButton:checked{background:#123354;border-color:#1479df;}"
        "QLineEdit,QComboBox{background:#171e23;border:1px solid #303a41;border-radius:5px;padding:7px;color:#edf2f6;}"
        "QFrame#statBox{background:#171e23;border:1px solid #222c32;border-radius:7px;}"
        "QLabel#gamesStat,QLabel#totalSizeStat,QLabel#onDiskStat,QLabel#savedStat{font-size:16px;font-weight:600;}"
        "QLabel#statCaption{color:#aab4bb;font-size:11px;}"
        "QLabel#selectedGameName{font-size:20px;font-weight:650;}"
        "QTableView{background:#101518;alternate-background-color:#141b1f;border:1px solid #202a30;"
        "selection-background-color:#123c62;selection-color:#fff;outline:0;}"
        "QHeaderView::section{background:#12191d;color:#c5cdd2;border:0;padding:7px 6px;}"
        "QStatusBar{background:#12181c;color:#aeb9c0;}"));

    const QString savedDirectory = settings.value(QStringLiteral("directory/lastSelected"))
                                       .toString();
    if (!savedDirectory.isEmpty()) {
        if (compresso::directoryValidationError(savedDirectory).isEmpty()) {
            directoryPath_->setText(savedDirectory);
            validateDirectoryPath();
        } else {
            settings.remove(QStringLiteral("directory/lastSelected"));
        }
    }
    if (!savedGeometry.isEmpty()) {
        restoreGeometry(savedGeometry);
    }
}

MainWindow::~MainWindow()
{
    // This also covers application shutdown paths that bypass closeEvent (for example,
    // QApplication::quit()). A running worker is allowed to finish before its QThread dies.
    if (operationThread_ && operationThread_->isRunning()) {
        operationStopSource_.request_stop();
        operationThread_->quit();
        operationThread_->wait();
    }
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (operationThread_ && operationThread_->isRunning()) {
        closeAfterOperation_ = true;
        cancelOperation();
        statusBar()->showMessage("Cancelling the current operation before closing…");
        event->ignore();
        return;
    }
    event->accept();
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
}

void MainWindow::startOperation(compresso::OperationWorker::Kind kind, const QString& directory)
{
    if (operationThread_ && operationThread_->isRunning()) {
        return;
    }

    setOperationControlsEnabled(false);
    operationStopSource_ = std::stop_source{};
    cancelButton_->setEnabled(true);
    cancelButton_->show();
    activeOperationKind_ = kind;
    activeOperationIsRestore_ = kind == compresso::OperationWorker::Kind::restore;
    diagnosticLines_.clear();
    detailsButton_->hide();
    progressBar_->setRange(0, 0);
    progressBar_->show();

    auto* thread = new QThread(this);
    auto* worker = new compresso::OperationWorker(
        kind, std::filesystem::path(directory.toStdWString()), operationStopSource_.get_token());
    worker->moveToThread(thread);

    connect(thread, &QThread::started, worker, &compresso::OperationWorker::run);
    connect(worker, &compresso::OperationWorker::progressChanged, this,
            [this](const QString& message) {
                if (!operationStopSource_.stop_requested()) {
                    statusBar()->showMessage(message);
                }
            });
    connect(worker, &compresso::OperationWorker::compressionProgress, this,
            [this](const compresso::CompressionProgress& progress) {
                latestCompressionProgress_ = progress;
                hasCompressionProgress_ = true;
                if (!operationStopSource_.stop_requested()) {
                    displayCompressionProgress(progress);
                }
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
        cancelButton_->hide();
        progressBar_->hide();
        if (closeAfterOperation_) {
            closeAfterOperation_ = false;
            QMetaObject::invokeMethod(this, &QWidget::close, Qt::QueuedConnection);
        }
    });

    operationThread_ = thread;
    thread->start();
}

void MainWindow::cancelOperation()
{
    if (!operationThread_ || !operationThread_->isRunning()
        || operationStopSource_.stop_requested()) {
        return;
    }

    operationStopSource_.request_stop();
    cancelButton_->setEnabled(false);
    statusBar()->showMessage(activeOperationKind_ == compresso::OperationWorker::Kind::scan
        ? QStringLiteral("Stopping directory scan…")
        : QStringLiteral("Cancellation requested. Waiting for compact.exe to stop safely…"));
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
    QSettings settings;
    settings.setValue(QStringLiteral("directory/lastSelected"), validatedDirectory_);
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

    if (result.cancelled) {
        originalSizeValue_->setText("\u2014");
        compressedSizeValue_->setText("\u2014");
        fileCountValue_->setText("\u2014");
        filesystemValue_->setText("\u2014");
        analyzedDirectoryIsEligible_ = false;
        compressButton_->setEnabled(false);
        restoreButton_->setEnabled(false);
        diagnosticLines_.clear();
        for (const auto& error : result.errors) {
            diagnosticLines_.push_back(diagnosticText(error));
        }
        detailsButton_->setVisible(!diagnosticLines_.isEmpty());
        statusBar()->showMessage("Scan cancelled; partial results were discarded.");
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

    gameModel_->removeRows(0, gameModel_->rowCount());
    const QString gameName = QFileInfo(directory).fileName().isEmpty()
        ? directory : QFileInfo(directory).fileName();
    std::size_t compressedFiles = 0;
    for (const auto& file : result.files) {
        if (file.attributes.compressed) ++compressedFiles;
    }
    const QString status = !result.traversal_complete || result.total_file_count == 0
        ? QStringLiteral("Unknown")
        : compressedFiles == 0 ? QStringLiteral("Uncompressed")
                               : QStringLiteral("Unknown");
    const QString sizeText = result.traversal_complete
        ? compactBytes(result.total_logical_bytes) : QStringLiteral("—");
    const QString diskText = result.allocation_measurements_complete
        ? compactBytes(result.total_allocated_bytes) : QStringLiteral("—");
    const QString savedText = result.allocation_measurements_complete
            && result.total_logical_bytes > result.total_allocated_bytes
        ? compactBytes(result.total_logical_bytes - result.total_allocated_bytes)
        : QStringLiteral("—");
    const QString volumeRoot = QDir(directory).rootPath();
    const QString drive = volumeRoot.size() >= 2 && volumeRoot.at(1) == QLatin1Char(':')
        ? volumeRoot.left(2).toUpper() : QStringLiteral("—");
    const QString action = status == QStringLiteral("Uncompressed")
        ? QStringLiteral("Compress") : QStringLiteral("Analyze");
    QList<QStandardItem*> row;
    for (const QString& value : {gameName, QStringLiteral("Manual"), drive, sizeText,
                                 diskText, savedText, status, action}) {
        row.push_back(new QStandardItem(value));
    }
    row[0]->setToolTip(directory);
    row[6]->setToolTip(compressedFiles > 0
        ? QStringLiteral("Windows reports compressed files, but the algorithm is not verified.")
        : QStringLiteral("No files were reported as compressed by Windows."));
    gameModel_->appendRow(row);
    gamesStat_->setText(QStringLiteral("1"));
    totalSizeStat_->setText(sizeText);
    onDiskStat_->setText(diskText);
    savedStat_->setText(savedText);
    selectedGameName_->setText(gameName);
    selectedLauncher_->setText(QStringLiteral("Manual folder  ·  %1").arg(drive));
    selectedStatus_->setText(status);
    selectedStatus_->setToolTip(row[6]->toolTip());
    const QModelIndex gameIndex = gameProxy_->index(0, 0);
    if (gameIndex.isValid()) gameTable_->selectRow(0);

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
    compressButton_->setEnabled(analyzedDirectoryIsEligible_);
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
        } else if (file.state == compresso::CompressionFileState::cancelled) {
            QString details = QStringLiteral("Cancelled during: ")
                + QString::fromStdWString(file.path.native()) + "\n" + file.error_message;
            if (!file.standard_output.isEmpty()) {
                details += "\nstdout:\n" + QString::fromLocal8Bit(file.standard_output);
            }
            if (!file.standard_error.isEmpty()) {
                details += "\nstderr:\n" + QString::fromLocal8Bit(file.standard_error);
            }
            diagnosticLines_.push_back(std::move(details));
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
    if (result.cancelled) {
        message = "LZX cancelled: "
            + locale.toString(static_cast<qulonglong>(result.compressedCount())) + " compressed, "
            + locale.toString(static_cast<qulonglong>(result.skippedCount())) + " skipped, "
            + locale.toString(static_cast<qulonglong>(result.failedCount())) + " failed. ";
    }
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
    if (result.cancelled) {
        message += result.cancelledCount() > 0
            ? " Completed file changes were kept; the active file's state is unverified."
            : " Completed file changes were kept.";
    }
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
        } else if (file.state == compresso::CompressionFileState::cancelled) {
            QString details = QStringLiteral("Cancelled during: ")
                + QString::fromStdWString(file.path.native()) + "\n" + file.error_message;
            if (!file.standard_output.isEmpty()) {
                details += "\nstdout:\n" + QString::fromLocal8Bit(file.standard_output);
            }
            if (!file.standard_error.isEmpty()) {
                details += "\nstderr:\n" + QString::fromLocal8Bit(file.standard_error);
            }
            diagnosticLines_.push_back(std::move(details));
        } else if (file.state == compresso::CompressionFileState::skipped) {
            diagnosticLines_.push_back(file.error ? diagnosticText(*file.error)
                                                  : skippedFileText(file));
        }
    }
    detailsButton_->setVisible(!diagnosticLines_.isEmpty());

    QString message = result.cancelled ? "Restore cancelled: " : "Restore: ";
    message += locale.toString(static_cast<qulonglong>(result.restoredCount())) + " restored, "
        + locale.toString(static_cast<qulonglong>(result.skippedCount())) + " skipped, "
        + locale.toString(static_cast<qulonglong>(result.failedCount())) + " failed";
    if (result.skippedCount() > 0) {
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
        const auto firstFailure = std::find_if(result.files.begin(), result.files.end(),
            [](const auto& file) { return file.state == compresso::CompressionFileState::failed; });
        if (firstFailure != result.files.end() && firstFailure->error) {
            message += ". " + firstFailure->error_message;
        }
    }
    if (result.cancelled) {
        message += result.cancelledCount() > 0
            ? ". Completed restorations were kept; the active file's state is unverified."
            : ". Completed restorations were kept.";
    } else {
        message += ". Analyze again to refresh allocated size.";
    }
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
