#include "compresso/main_window.h"

#include "compresso/directory_validator.h"
#include "compresso/operation_worker.h"

#include <QCloseEvent>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QColor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QIcon>
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
#include <QStackedWidget>
#include <QMenu>
#include <QAction>
#include <QToolButton>
#include <QFileInfo>
#include <QFile>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPixmap>
#include <QStandardPaths>
#include <QDesktopServices>
#include <QDebug>
#include <QUrl>
#include <QSet>
#include <QRegularExpression>

#include <array>
#include <algorithm>
#include <chrono>
#include <utility>

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

QIcon navigationIcon(int symbol)
{
    QPixmap image(20, 20);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(QStringLiteral("#d8e0e8")), 1.6,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    if (symbol == 0) {
        painter.drawRoundedRect(QRectF(3.5, 3.5, 5.5, 5.5), 1.0, 1.0);
        painter.drawRoundedRect(QRectF(11.0, 3.5, 5.5, 5.5), 1.0, 1.0);
        painter.drawRoundedRect(QRectF(3.5, 11.0, 5.5, 5.5), 1.0, 1.0);
        painter.drawRoundedRect(QRectF(11.0, 11.0, 5.5, 5.5), 1.0, 1.0);
    } else if (symbol == 1) {
        for (int y : {5, 10, 15}) {
            painter.drawEllipse(QPointF(4.5, y), 1.1, 1.1);
            painter.drawLine(QPointF(8.0, y), QPointF(17.0, y));
        }
    }
    return QIcon(image);
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
    qRegisterMetaType<compresso::GameInstallation>("compresso::GameInstallation");
    qRegisterMetaType<QList<compresso::GameInstallation>>(
        "QList<compresso::GameInstallation>");

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
    auto* queueGameButton = new QToolButton(content);
    queueGameButton->setObjectName(QStringLiteral("queueGameButton"));
    queueGameButton->setText(QStringLiteral("Queue"));
    queueGameButton->setPopupMode(QToolButton::InstantPopup);
    auto* queueGameMenu = new QMenu(queueGameButton);
    auto* queueCompressAction = queueGameMenu->addAction(QStringLiteral("Queue compress"));
    auto* queueRestoreAction = queueGameMenu->addAction(QStringLiteral("Queue restore"));
    queueGameButton->setMenu(queueGameMenu);
    connect(queueCompressAction, &QAction::triggered, this, [this] {
        queueSelectedGame(compresso::OperationWorker::Kind::compress);
    });
    connect(queueRestoreAction, &QAction::triggered, this, [this] {
        queueSelectedGame(compresso::OperationWorker::Kind::restore);
    });
    actions->addWidget(queueGameButton);

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
    auto* brand = new QLabel(QStringLiteral("Compresso"), sidebar);
    brand->setObjectName(QStringLiteral("brand"));
    sideLayout->addWidget(brand);
    sideLayout->addSpacing(14);
    const auto addNavButton = [sidebar, sideLayout](const QString& label,
                                                    const QString& name, int symbol) {
        auto* button = new QPushButton(navigationIcon(symbol), label, sidebar);
        button->setObjectName(name);
        button->setCheckable(true);
        button->setFlat(true);
        button->setMinimumHeight(38);
        sideLayout->addWidget(button);
        return button;
    };
    auto* libraryNav = addNavButton(QStringLiteral("Library"), QStringLiteral("libraryNav"), 0);
    libraryNav->setChecked(true);
    auto* queueNav = addNavButton(QStringLiteral("Queue"), QStringLiteral("queueNav"), 1);
    sideLayout->addStretch(1);
    connect(queueNav, &QPushButton::clicked, this, [this, libraryNav, queueNav] {
        libraryNav->setChecked(false);
        queueNav->setChecked(true);
        centerPages_->setCurrentIndex(1);
        statusBar()->showMessage(QStringLiteral("Queue runs sequentially."));
    });
    connect(libraryNav, &QPushButton::clicked, this, [libraryNav, queueNav] {
        libraryNav->setChecked(true);
        queueNav->setChecked(false);
    });
    connect(libraryNav, &QPushButton::clicked, this, [this] {
        centerPages_->setCurrentIndex(0);
    });
    auto* center = new QWidget(shell);
    center->setObjectName(QStringLiteral("libraryCenter"));
    auto* centerLayout = new QVBoxLayout(center);
    centerLayout->setContentsMargins(0, 0, 0, 0);
    centerPages_ = new QStackedWidget(center);
    auto* libraryPage = new QWidget(centerPages_);
    auto* libraryLayout = new QVBoxLayout(libraryPage);
    libraryLayout->setContentsMargins(16, 14, 4, 12);
    libraryLayout->setSpacing(10);
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
        QStringLiteral("Uncompressed"), QStringLiteral("Partial"), QStringLiteral("Other"),
        QStringLiteral("Unsupported"), QStringLiteral("Unavailable"), QStringLiteral("Unknown")});
    toolbar->addWidget(searchField_, 1);
    toolbar->addWidget(launcherFilter_);
    toolbar->addWidget(driveFilter_);
    toolbar->addWidget(statusFilter_);
    auto* refresh = new QPushButton(QStringLiteral("Refresh"), center);
    refresh->setObjectName(QStringLiteral("libraryRefreshButton"));
    toolbar->addWidget(refresh);
    connect(refresh, &QPushButton::clicked, this, [this] { refreshLibrary(); });
    libraryLayout->addLayout(toolbar);

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
    libraryLayout->addLayout(stats);

    gameModel_ = new QStandardItemModel(0, 9, this);
    gameModel_->setHorizontalHeaderLabels({QStringLiteral("Game"), QStringLiteral("Launcher"),
        QStringLiteral("Drive"), QStringLiteral("Size"), QStringLiteral("On disk"),
        QStringLiteral("Saved"), QStringLiteral("Status"), QStringLiteral("Action"),
        QStringLiteral("Filter data")});
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
    gameTable_->setIconSize(QSize(44, 44));
    gameTable_->verticalHeader()->setDefaultSectionSize(48);
    gameTable_->horizontalHeader()->setStretchLastSection(false);
    gameTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 9; ++column)
        gameTable_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    gameTable_->setColumnHidden(8, true);
    gameTable_->setSortingEnabled(true);
    libraryLayout->addWidget(gameTable_, 1);
    const auto applyFilters = [this] {
        QStringList terms;
        const QString search = searchField_->text().trimmed();
        if (!search.isEmpty()) terms.push_back(search);
        if (launcherFilter_->currentIndex() > 0) terms.push_back(launcherFilter_->currentText());
        if (driveFilter_->currentIndex() > 0) terms.push_back(driveFilter_->currentText());
        if (statusFilter_->currentIndex() > 0) terms.push_back(statusFilter_->currentText());
        QString pattern;
        for (const QString& term : terms)
            pattern += QStringLiteral("(?=.*%1)").arg(QRegularExpression::escape(term));
        gameProxy_->setFilterKeyColumn(8);
        gameProxy_->setFilterRegularExpression(QRegularExpression(
            pattern, QRegularExpression::CaseInsensitiveOption));
    };
    connect(searchField_, &QLineEdit::textChanged, this, [applyFilters] { applyFilters(); });
    connect(launcherFilter_, &QComboBox::currentTextChanged, this,
            [applyFilters] { applyFilters(); });
    connect(driveFilter_, &QComboBox::currentTextChanged, this,
            [applyFilters] { applyFilters(); });
    connect(statusFilter_, &QComboBox::currentTextChanged, this,
            [applyFilters] { applyFilters(); });
    connect(gameTable_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        if (index.column() != 7) return;
        if (!validatedDirectory_.isEmpty() && analyzedDirectoryIsEligible_) {
            if (compressButton_->isEnabled()) startCompression();
            else if (restoreButton_->isEnabled()) startRestore();
        } else startDirectoryScan();
    });

    gameTable_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(gameTable_, &QWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        const QModelIndex index = gameTable_->indexAt(point);
        if (!index.isValid()) return;
        gameTable_->selectRow(index.row());
        const bool available = !directoryPath_->text().isEmpty()
            && compresso::directoryValidationError(directoryPath_->text()).isEmpty();
        auto* menu = new QMenu(gameTable_);
        auto* analyze = menu->addAction(QStringLiteral("Analyze"));
        analyze->setEnabled(available);
        auto* compress = menu->addAction(QStringLiteral("Queue compress"));
        compress->setEnabled(available);
        auto* restore = menu->addAction(QStringLiteral("Queue restore"));
        restore->setEnabled(available);
        connect(analyze, &QAction::triggered, this, [this] { startDirectoryScan(); });
        connect(compress, &QAction::triggered, this, [this] {
            queueSelectedGame(compresso::OperationWorker::Kind::compress);
        });
        connect(restore, &QAction::triggered, this, [this] {
            queueSelectedGame(compresso::OperationWorker::Kind::restore);
        });
        menu->popup(gameTable_->viewport()->mapToGlobal(point));
    });
    connect(gameTable_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex& current) {
        if (!current.isValid()) return;
        const int sourceRow = gameProxy_->mapToSource(current).row();
        auto* nameItem = gameModel_->item(sourceRow, 0);
        if (!nameItem) return;
        selectedGameName_->setText(nameItem->text());
        selectedLauncher_->setText(gameModel_->item(sourceRow, 1)->text());
        selectedStatus_->setText(gameModel_->item(sourceRow, 6)->text());
        showGameArtwork(nameItem->data(Qt::UserRole + 1).toString());
        const QString path = nameItem->data(Qt::UserRole).toString();
        if (!path.isEmpty() && path != directoryPath_->text()) {
            directoryPath_->setText(path);
            validateDirectoryPath();
        }
    });

    auto* queuePage = new QWidget(centerPages_);
    auto* queueLayout = new QVBoxLayout(queuePage);
    queueLayout->setContentsMargins(16, 14, 12, 12);
    queueLayout->setSpacing(10);
    auto* queueTitle = new QLabel(QStringLiteral("Queue"), queuePage);
    queueTitle->setStyleSheet(QStringLiteral("font-size:18px;font-weight:600;"));
    queueLayout->addWidget(queueTitle);
    queueModel_ = new QStandardItemModel(0, 4, this);
    queueModel_->setHorizontalHeaderLabels({QStringLiteral("Game"), QStringLiteral("Action"),
        QStringLiteral("Progress"), QStringLiteral("Status")});
    queueTable_ = new QTableView(queuePage);
    queueTable_->setObjectName(QStringLiteral("queueTable"));
    queueTable_->setModel(queueModel_);
    queueTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    queueTable_->setAlternatingRowColors(true);
    queueTable_->setShowGrid(false);
    queueTable_->verticalHeader()->hide();
    queueTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 4; ++column)
        queueTable_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    queueLayout->addWidget(queueTable_, 1);
    auto* queueActions = new QHBoxLayout;
    startQueueButton_ = new QPushButton(QStringLiteral("Start queue"), queuePage);
    startQueueButton_->setObjectName(QStringLiteral("startQueueButton"));
    cancelQueueButton_ = new QPushButton(QStringLiteral("Stop"), queuePage);
    cancelQueueButton_->setObjectName(QStringLiteral("cancelQueueButton"));
    auto* cancelPendingButton = new QPushButton(QStringLiteral("Cancel pending"), queuePage);
    cancelPendingButton->setObjectName(QStringLiteral("cancelPendingButton"));
    queueActions->addWidget(cancelPendingButton);
    queueActions->addStretch(1);
    queueActions->addWidget(cancelQueueButton_);
    queueActions->addWidget(startQueueButton_);
    queueLayout->addLayout(queueActions);
    connect(startQueueButton_, &QPushButton::clicked, this, [this] { startQueue(); });
    connect(cancelQueueButton_, &QPushButton::clicked, this, [this] {
        queueRunning_ = false;
        if (activeQueueTask_ >= 0) cancelOperation();
    });
    connect(cancelPendingButton, &QPushButton::clicked, this, [this] {
        const QModelIndex selected = queueTable_->currentIndex();
        if (!selected.isValid()) return;
        const int row = selected.row();
        if (row == activeQueueTask_ || row < 0 || row >= static_cast<int>(queueTasks_.size())) return;
        if (queueTasks_[static_cast<std::size_t>(row)].status != QStringLiteral("Queued")) return;
        queueTasks_[static_cast<std::size_t>(row)].status = QStringLiteral("Cancelled");
        updateQueueTask(row, QStringLiteral("—"), QStringLiteral("Cancelled"));
    });
    centerPages_->addWidget(libraryPage);
    centerPages_->addWidget(queuePage);
    centerLayout->addWidget(centerPages_);

    previousContent->setObjectName(QStringLiteral("gameDetails"));
    selectedGameName_ = new QLabel(QStringLiteral("No game selected"), previousContent);
    selectedGameName_->setObjectName(QStringLiteral("selectedGameName"));
    selectedGameName_->setStyleSheet(QStringLiteral("font-size:20px;font-weight:650;"));
    selectedArtwork_ = new QLabel(previousContent);
    selectedArtwork_->setObjectName(QStringLiteral("selectedGameArtwork"));
    selectedArtwork_->setAlignment(Qt::AlignCenter);
    selectedArtwork_->setMinimumHeight(128);
    selectedArtwork_->setMaximumHeight(180);
    selectedArtwork_->setScaledContents(true);
    artworkNetwork_ = new QNetworkAccessManager(this);
    showGameArtwork({});
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
        oldLayout->insertWidget(0, selectedArtwork_);
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

void MainWindow::refreshLibrary()
{
    if (operationThread_ && operationThread_->isRunning()) return;
    statusBar()->showMessage(QStringLiteral("Finding Steam libraries…"));
    startOperation(compresso::OperationWorker::Kind::discover_steam, {});
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
                    if (activeQueueTask_ >= 0 && progress.totals_known
                        && progress.total_files > 0) {
                        const int percent = static_cast<int>(
                            (100.0 * progress.completed_files) / progress.total_files);
                        updateQueueTask(activeQueueTask_, QStringLiteral("%1%").arg(percent),
                            activeOperationIsRestore_ ? QStringLiteral("Restoring")
                                                      : QStringLiteral("Compressing"));
                    }
                }
            });
    connect(worker, &compresso::OperationWorker::errorOccurred, this,
            [this](const compresso::OperationError& error) {
                if (activeQueueTask_ >= 0)
                    updateQueueTask(activeQueueTask_, QStringLiteral("—"), QStringLiteral("Failed"));
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
                if (result.target_eligible
                    && (result.compressedCount() > 0 || result.failedCount() > 0)) {
                    pendingRefreshPath_ = directory;
                }
                if (activeQueueTask_ >= 0) {
                    const QString state = result.cancelled ? QStringLiteral("Cancelled")
                        : (!result.target_eligible || result.failedCount() > 0)
                            ? QStringLiteral("Failed") : QStringLiteral("Done");
                    updateQueueTask(activeQueueTask_,
                        QStringLiteral("%1 files").arg(result.compressedCount()), state);
                }
                displayCompressionResult(result, directory);
            });
    connect(worker, &compresso::OperationWorker::restorationCompleted, this,
            [this, directory](const compresso::CompressionReport& result) {
                progressTimer_->stop();
                if (result.target_eligible
                    && (result.restoredCount() > 0 || result.failedCount() > 0)) {
                    pendingRefreshPath_ = directory;
                }
                if (activeQueueTask_ >= 0) {
                    const QString state = result.cancelled ? QStringLiteral("Cancelled")
                        : (!result.target_eligible || result.failedCount() > 0)
                            ? QStringLiteral("Failed") : QStringLiteral("Done");
                    updateQueueTask(activeQueueTask_,
                        QStringLiteral("%1 files").arg(result.restoredCount()), state);
                }
                displayRestoreResult(result, directory);
            });
    connect(worker, &compresso::OperationWorker::gamesDiscovered, this,
            [this](const QList<compresso::GameInstallation>& games) {
                displayDiscoveredGames(games);
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
        const bool finishedQueueTask = activeQueueTask_ >= 0;
        if (finishedQueueTask) activeQueueTask_ = -1;
        if (closeAfterOperation_) {
            closeAfterOperation_ = false;
            QMetaObject::invokeMethod(this, &QWidget::close, Qt::QueuedConnection);
            return;
        }
        if (!pendingRefreshPath_.isEmpty()) {
            const QString refreshPath = std::exchange(pendingRefreshPath_, {});
            QTimer::singleShot(0, this, [this, refreshPath] {
                startOperation(compresso::OperationWorker::Kind::scan, refreshPath);
            });
            return;
        }
        if (queueRunning_) {
            QTimer::singleShot(0, this, [this] { startNextQueueTask(); });
        } else if (finishedQueueTask) {
            startQueueButton_->setEnabled(true);
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
    const bool updateSelectedGame = directory == directoryPath_->text()
                                    && directory == validatedDirectory_;

    if (result.cancelled) {
        if (!updateSelectedGame) return;
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

    const QLocale locale;

    int modelRow = -1;
    for (int candidate = 0; candidate < gameModel_->rowCount(); ++candidate) {
        if (QDir::cleanPath(gameModel_->item(candidate, 0)->data(Qt::UserRole).toString())
            .compare(QDir::cleanPath(directory), Qt::CaseInsensitive) == 0) {
            modelRow = candidate;
            break;
        }
    }
    QString gameName = QFileInfo(directory).fileName();
    if (gameName.isEmpty()) gameName = directory;
    QString launcher = QStringLiteral("Manual");
    if (modelRow >= 0) {
        gameName = gameModel_->item(modelRow, 0)->text();
        launcher = gameModel_->item(modelRow, 1)->text();
    } else {
        QList<QStandardItem*> newRow;
        for (int column = 0; column < 9; ++column) newRow.push_back(new QStandardItem);
        modelRow = gameModel_->rowCount();
        gameModel_->appendRow(newRow);
        gameModel_->item(modelRow, 0)->setData(directory, Qt::UserRole);
    }
    std::size_t eligibleFiles = 0;
    std::size_t lzxFiles = 0;
    std::size_t otherCompressedFiles = 0;
    std::size_t uncompressedFiles = 0;
    std::size_t unknownCompressionFiles = 0;
    for (const auto& file : result.files) {
        if (file.attributes.encrypted || file.attributes.sparse) continue;
        ++eligibleFiles;
        switch (file.compression_state) {
        case compresso::FileCompressionState::unknown:
            ++unknownCompressionFiles;
            break;
        case compresso::FileCompressionState::uncompressed:
            ++uncompressedFiles;
            break;
        case compresso::FileCompressionState::lzx_compressed:
            ++lzxFiles;
            break;
        case compresso::FileCompressionState::other:
            ++otherCompressedFiles;
            break;
        }
    }
    QString status = QStringLiteral("Unknown");
    if (result.volume.error) {
        status = QStringLiteral("Unknown");
    } else if (!result.volume.compression_eligible) {
        status = QStringLiteral("Unsupported");
    } else if (!result.traversal_complete || eligibleFiles == 0
               || unknownCompressionFiles > 0) {
        status = QStringLiteral("Unknown");
    } else if (otherCompressedFiles > 0) {
        status = QStringLiteral("Other");
    } else if (lzxFiles == eligibleFiles) {
        status = QStringLiteral("Compressed");
    } else if (lzxFiles == 0) {
        status = QStringLiteral("Uncompressed");
    } else {
        status = QStringLiteral("Partial");
    }
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
    const bool canCompress = status == QStringLiteral("Uncompressed")
                             || status == QStringLiteral("Partial");
    const bool canRestore = status == QStringLiteral("Compressed")
                            || status == QStringLiteral("Partial");
    const QString action = canCompress ? QStringLiteral("Compress")
        : canRestore ? QStringLiteral("Restore") : QStringLiteral("Analyze");
    const QStringList values{gameName, launcher, drive, sizeText,
                             diskText, savedText, status, action};
    for (int column = 0; column < values.size(); ++column)
        gameModel_->item(modelRow, column)->setText(values[column]);
    auto* gameItem = gameModel_->item(modelRow, 0);
    gameItem->setData(directory, Qt::UserRole);
    if (gameItem->icon().isNull())
        gameItem->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    gameItem->setToolTip(directory);
    QString statusDetails = QStringLiteral(
        "LZX: %1; uncompressed: %2; other: %3; eligible: %4; unknown: %5")
        .arg(static_cast<qulonglong>(lzxFiles))
        .arg(static_cast<qulonglong>(uncompressedFiles))
        .arg(static_cast<qulonglong>(otherCompressedFiles))
        .arg(static_cast<qulonglong>(eligibleFiles))
        .arg(static_cast<qulonglong>(unknownCompressionFiles));
    gameModel_->item(modelRow, 6)->setToolTip(statusDetails);
    gameModel_->item(modelRow, 8)->setText(
        QStringList{gameName, launcher, drive, status, directory}.join(QLatin1Char(' ')));
    gamesStat_->setText(QString::number(gameModel_->rowCount()));
    const QModelIndex gameIndex = gameProxy_->mapFromSource(gameModel_->index(modelRow, 0));
    if (updateSelectedGame) {
        originalSizeLabel_->setText("Original size (logical)");
        compressedSizeLabel_->setText("Allocated size (on disk)");
        originalSizeValue_->setText(
            locale.toString(static_cast<qulonglong>(result.total_logical_bytes)) + " bytes");
        compressedSizeValue_->setText(result.allocation_measurements_complete
            ? locale.toString(static_cast<qulonglong>(result.total_allocated_bytes)) + " bytes"
            : "Unavailable");
        fileCountValue_->setText(
            locale.toString(static_cast<qulonglong>(result.total_file_count)));
        totalSizeStat_->setText(gameModel_->rowCount() == 1 ? sizeText : QStringLiteral("—"));
        onDiskStat_->setText(gameModel_->rowCount() == 1 ? diskText : QStringLiteral("—"));
        savedStat_->setText(gameModel_->rowCount() == 1 ? savedText : QStringLiteral("—"));
        selectedGameName_->setText(gameName);
        selectedLauncher_->setText(QStringLiteral("%1  ·  %2").arg(launcher, drive));
        selectedStatus_->setText(status);
        selectedStatus_->setToolTip(statusDetails);
        if (gameIndex.isValid()) gameTable_->selectRow(gameIndex.row());

        if (result.volume.error) {
            filesystemValue_->setText("Unavailable");
        } else if (result.volume.compression_eligible) {
            filesystemValue_->setText(QString::fromStdWString(result.volume.filesystem_type)
                                      + " (LZX eligible)");
        } else {
            filesystemValue_->setText(QString::fromStdWString(result.volume.filesystem_type)
                                      + " (LZX unavailable)");
        }
    }

    if (!updateSelectedGame) return;
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
                                   && eligibleFiles > 0;
    compressButton_->setEnabled(analyzedDirectoryIsEligible_ && canCompress);
    restoreButton_->setEnabled(analyzedDirectoryIsEligible_ && canRestore);
}

void MainWindow::displayDiscoveredGames(const QList<compresso::GameInstallation>& games)
{
    gameModel_->removeRows(0, gameModel_->rowCount());
    QSet<QString> launchers;
    QSet<QString> drives;
    for (const auto& game : games) {
        const QString root = QDir(game.install_path).rootPath();
        const QString drive = root.size() >= 2 && root.at(1) == QLatin1Char(':')
            ? root.left(2).toUpper() : QStringLiteral("—");
        const QString status = game.available ? QStringLiteral("Unknown")
                                              : QStringLiteral("Unavailable");
        const QStringList values{game.name, game.launcher, drive, QStringLiteral("—"),
            QStringLiteral("—"), QStringLiteral("—"), status,
            game.available ? QStringLiteral("Analyze") : QStringLiteral("Unavailable"),
            QStringList{game.name, game.launcher, drive, status, game.install_path}
                .join(QLatin1Char(' '))};
        QList<QStandardItem*> row;
        for (const QString& value : values) row.push_back(new QStandardItem(value));
        row[0]->setData(game.install_path, Qt::UserRole);
        row[0]->setData(game.id, Qt::UserRole + 1);
        row[0]->setToolTip(game.install_path);
        const QString cachePath = QDir(
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                                      .filePath(QStringLiteral("artwork/%1.jpg").arg(game.id));
        const QPixmap cachedArtwork(cachePath);
        row[0]->setData(cachePath, Qt::UserRole + 2);
        row[0]->setIcon(cachedArtwork.isNull()
            ? style()->standardIcon(QStyle::SP_MediaPlay) : QIcon(cachedArtwork));
        row[6]->setToolTip(game.available
            ? QStringLiteral("Analyze this installation to measure its current state.")
            : QStringLiteral("The manifest exists, but the installation directory is missing."));
        gameModel_->appendRow(row);
        if (cachedArtwork.isNull()) requestArtwork(game.id);
        launchers.insert(game.launcher);
        drives.insert(drive);
    }
    launcherFilter_->clear();
    launcherFilter_->addItem(QStringLiteral("All launchers"));
    for (const QString& launcher : std::as_const(launchers)) launcherFilter_->addItem(launcher);
    driveFilter_->clear();
    driveFilter_->addItem(QStringLiteral("All drives"));
    for (const QString& drive : std::as_const(drives)) {
        if (drive != QStringLiteral("—")) driveFilter_->addItem(drive);
    }
    gamesStat_->setText(QString::number(games.size()));
    totalSizeStat_->setText(QStringLiteral("—"));
    onDiskStat_->setText(QStringLiteral("—"));
    savedStat_->setText(QStringLiteral("—"));
    selectedGameName_->setText(games.isEmpty()
        ? QStringLiteral("No Steam games found") : QStringLiteral("Select a game"));
    selectedLauncher_->setText(games.isEmpty()
        ? QStringLiteral("Add a folder manually") : QStringLiteral("Steam library"));
    selectedStatus_->setText(QStringLiteral("—"));
    showGameArtwork({});
    analyzedDirectoryIsEligible_ = false;
    compressButton_->setEnabled(false);
    restoreButton_->setEnabled(false);
    statusBar()->showMessage(games.isEmpty()
        ? QStringLiteral("No Steam installations found. You can add a folder manually.")
        : QStringLiteral("Found %1 Steam games across available libraries.").arg(games.size()));
}

void MainWindow::showGameArtwork(const QString& appId)
{
    selectedArtworkAppId_ = appId;
    QPixmap artwork;
    if (!appId.isEmpty()) {
        const QString path = QDir(
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                                 .filePath(QStringLiteral("artwork/%1.jpg").arg(appId));
        artwork.load(path);
    }
    if (artwork.isNull()) {
        QPixmap placeholder(320, 150);
        placeholder.fill(QColor(QStringLiteral("#192126")));
        QPainter painter(&placeholder);
        const QPixmap icon = style()->standardIcon(QStyle::SP_MediaPlay)
                                 .pixmap(QSize(52, 52));
        painter.drawPixmap((placeholder.width() - icon.width()) / 2,
                           (placeholder.height() - icon.height()) / 2, icon);
        selectedArtwork_->setPixmap(placeholder);
        selectedArtwork_->setToolTip(appId.isEmpty()
            ? QStringLiteral("Artwork unavailable")
            : QStringLiteral("Loading game artwork…"));
        if (!appId.isEmpty()) requestArtwork(appId);
        return;
    }
    selectedArtwork_->setPixmap(artwork.scaled(selectedArtwork_->size(), Qt::KeepAspectRatio,
                                                Qt::SmoothTransformation));
    selectedArtwork_->setToolTip(QStringLiteral("Steam game artwork"));
}

void MainWindow::requestArtwork(const QString& appId, int variant)
{
    static const QStringList artworkNames{
        QStringLiteral("header.jpg"), QStringLiteral("library_600x900.jpg")};
    static const QRegularExpression validAppId(QStringLiteral("^\\d+$"));
    if (!validAppId.match(appId).hasMatch() || variant > artworkNames.size()
        || artworkRequests_.contains(appId)) return;

    const QString cacheDirectory = QDir(
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                                      .filePath(QStringLiteral("artwork"));
    if (!QDir().mkpath(cacheDirectory)) return;
    const QString cachePath = QDir(cacheDirectory).filePath(appId + QStringLiteral(".jpg"));
    if (QFileInfo::exists(cachePath)) {
        QPixmap cached(cachePath);
        if (!cached.isNull()) return;
        QFile::remove(cachePath);
    }

    for (const QString& steamArtwork : compresso::SteamDiscovery::artworkCacheFiles(appId)) {
        QImage image;
        if (!image.load(steamArtwork) || image.width() < 100 || image.height() < 60) continue;
        QFile::remove(cachePath);
        if (!QFile::copy(steamArtwork, cachePath)) continue;
        const QPixmap pixmap = QPixmap::fromImage(image);
        for (int row = 0; row < gameModel_->rowCount(); ++row) {
            auto* item = gameModel_->item(row, 0);
            if (item && item->data(Qt::UserRole + 1).toString() == appId) {
                item->setIcon(QIcon(pixmap));
                item->setData(cachePath, Qt::UserRole + 2);
            }
        }
        if (selectedArtworkAppId_ == appId) showGameArtwork(appId);
        return;
    }

    artworkRequests_.insert(appId);
    if (variant == artworkNames.size()) {
        const QUrl metadataUrl(QStringLiteral("https://store.steampowered.com/api/appdetails?appids=%1&filters=basic")
                                   .arg(appId));
        QNetworkRequest request(metadataUrl);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = artworkNetwork_->get(request);
        QTimer::singleShot(15000, reply, [reply] {
            if (reply->isRunning()) reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, appId, cachePath] {
            const QByteArray data = reply->readAll();
            QUrl artworkUrl;
            if (reply->error() == QNetworkReply::NoError) {
                const QJsonObject app = QJsonDocument::fromJson(data).object()
                                            .value(appId).toObject();
                artworkUrl = QUrl(app.value(QStringLiteral("data")).toObject()
                                      .value(QStringLiteral("header_image")).toString());
            }
            if (!artworkUrl.isValid() || artworkUrl.scheme() != QStringLiteral("https")) {
                qWarning().noquote() << "Steam artwork metadata unavailable for app" << appId
                                     << reply->errorString();
                artworkRequests_.remove(appId);
                if (selectedArtworkAppId_ == appId) {
                    selectedArtwork_->setToolTip(QStringLiteral("Artwork unavailable"));
                }
                reply->deleteLater();
                return;
            }
            reply->deleteLater();
            downloadArtwork(appId, artworkUrl, cachePath, 2);
        });
        return;
    }

    const QUrl url(QStringLiteral("https://cdn.akamai.steamstatic.com/steam/apps/%1/%2")
                       .arg(appId, artworkNames.at(variant)));
    downloadArtwork(appId, url, cachePath, variant);
}

void MainWindow::downloadArtwork(const QString& appId, const QUrl& url,
                                 const QString& cachePath, int variant)
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = artworkNetwork_->get(request);
    QTimer::singleShot(15000, reply, [reply] {
        if (reply->isRunning()) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, appId, cachePath, variant] {
        const QByteArray data = reply->readAll();
        QImage image;
        if (reply->error() == QNetworkReply::NoError && image.loadFromData(data)
            && image.width() >= 100 && image.height() >= 60) {
            QFile file(cachePath);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(data);
                file.close();
            }
            const QPixmap pixmap = QPixmap::fromImage(image);
            for (int row = 0; row < gameModel_->rowCount(); ++row) {
                auto* item = gameModel_->item(row, 0);
                if (item && item->data(Qt::UserRole + 1).toString() == appId) {
                    item->setIcon(QIcon(pixmap));
                    item->setData(cachePath, Qt::UserRole + 2);
                }
            }
            if (selectedArtworkAppId_ == appId) showGameArtwork(appId);
            artworkRequests_.remove(appId);
            reply->deleteLater();
            return;
        }
        const QString error = reply->errorString();
        qWarning().noquote() << "Steam artwork request failed:" << reply->url().toString()
                             << error << "HTTP"
                             << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        artworkRequests_.remove(appId);
        reply->deleteLater();
        if (variant < 2) {
            requestArtwork(appId, variant + 1);
        } else if (selectedArtworkAppId_ == appId) {
            selectedArtwork_->setToolTip(QStringLiteral("Artwork unavailable: %1").arg(error));
        }
    });
}

void MainWindow::queueSelectedGame(compresso::OperationWorker::Kind kind)
{
    if (kind != compresso::OperationWorker::Kind::compress
        && kind != compresso::OperationWorker::Kind::restore) return;
    const QModelIndex current = gameTable_->currentIndex();
    if (!current.isValid()) return;
    const int row = gameProxy_->mapToSource(current).row();
    const QString path = gameModel_->item(row, 0)->data(Qt::UserRole).toString();
    const QString name = gameModel_->item(row, 0)->text();
    if (path.isEmpty() || compresso::directoryValidationError(path).size() > 0) {
        statusBar()->showMessage(QStringLiteral("Installation folder is unavailable."));
        return;
    }
    for (const auto& task : queueTasks_) {
        if (QDir::cleanPath(task.path).compare(QDir::cleanPath(path), Qt::CaseInsensitive) == 0
            && (task.status == QStringLiteral("Queued")
                || task.status == QStringLiteral("Analyzing")
                || task.status == QStringLiteral("Compressing")
                || task.status == QStringLiteral("Restoring"))) {
            statusBar()->showMessage(QStringLiteral("This game already has a pending task."));
            return;
        }
    }
    const int index = static_cast<int>(queueTasks_.size());
    const QString action = kind == compresso::OperationWorker::Kind::compress
        ? QStringLiteral("Compress") : QStringLiteral("Restore");
    queueTasks_.push_back({name, path, kind, QStringLiteral("Queued")});
    queueModel_->appendRow({new QStandardItem(name), new QStandardItem(action),
        new QStandardItem(QStringLiteral("—")), new QStandardItem(QStringLiteral("Queued"))});
    queueTable_->selectRow(index);
    statusBar()->showMessage(QStringLiteral("%1 queued.").arg(action));
}

void MainWindow::updateQueueTask(int index, const QString& progress, const QString& status)
{
    if (index < 0 || index >= queueModel_->rowCount()) return;
    if (index < static_cast<int>(queueTasks_.size()))
        queueTasks_[static_cast<std::size_t>(index)].status = status;
    queueModel_->item(index, 2)->setText(progress);
    queueModel_->item(index, 3)->setText(status);
}

void MainWindow::startQueue()
{
    if (operationThread_ && operationThread_->isRunning()) return;
    bool hasPending = false;
    bool hasRestore = false;
    for (const auto& task : queueTasks_) {
        if (task.status == QStringLiteral("Queued")) {
            hasPending = true;
            hasRestore = hasRestore || task.kind == compresso::OperationWorker::Kind::restore;
        }
    }
    if (!hasPending) {
        statusBar()->showMessage(QStringLiteral("No pending tasks."));
        return;
    }
    if (hasRestore) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Restore queued games"),
            QStringLiteral("Remove transparent compression from queued games? Files stay in place."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
    }
    queueRunning_ = true;
    startNextQueueTask();
}

void MainWindow::startNextQueueTask()
{
    if (!queueRunning_ || (operationThread_ && operationThread_->isRunning())) return;
    int next = -1;
    for (int index = 0; index < static_cast<int>(queueTasks_.size()); ++index) {
        if (queueTasks_[static_cast<std::size_t>(index)].status == QStringLiteral("Queued")) {
            next = index;
            break;
        }
    }
    if (next < 0) {
        queueRunning_ = false;
        activeQueueTask_ = -1;
        startQueueButton_->setEnabled(true);
        statusBar()->showMessage(QStringLiteral("Queue finished."));
        return;
    }
    const auto task = queueTasks_[static_cast<std::size_t>(next)];
    if (!compresso::directoryValidationError(task.path).isEmpty()) {
        updateQueueTask(next, QStringLiteral("—"), QStringLiteral("Failed"));
        QTimer::singleShot(0, this, [this] { startNextQueueTask(); });
        return;
    }
    activeQueueTask_ = next;
    startQueueButton_->setEnabled(false);
    updateQueueTask(next, QStringLiteral("—"),
        task.kind == compresso::OperationWorker::Kind::compress
            ? QStringLiteral("Compressing") : QStringLiteral("Restoring"));
    compressionClock_.start();
    hasCompressionProgress_ = false;
    progressTimer_->start();
    startOperation(task.kind, task.path);
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
