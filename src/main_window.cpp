#include "compresso/main_window.h"

#include "compresso/directory_validator.h"
#include "compresso/operation_worker.h"

#include <QCloseEvent>
#include <QApplication>
#include <QColor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
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
#include <QMenu>
#include <QFileInfo>
#include <QFile>
#include <QMouseEvent>
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
#include <QWindow>
#include <QSet>
#include <QRegularExpression>

#include <array>
#include <algorithm>
#include <chrono>
#include <utility>

namespace
{

class DraggableTitleBar final : public QWidget
{
public:
    using QWidget::QWidget;

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && window()->windowHandle()
            && window()->windowHandle()->startSystemMove()) {
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }
};

class WindowControlButton final : public QPushButton
{
public:
    enum class Icon { minimize, close };

    WindowControlButton(Icon icon, QWidget* parent)
        : QPushButton(parent), icon_(icon)
    {
        setFocusPolicy(Qt::NoFocus);
        setFixedSize(32, 32);
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QPushButton::paintEvent(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor color = palette().color(isEnabled()
            ? QPalette::Active : QPalette::Disabled, QPalette::ButtonText);
        painter.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::SquareCap));
        const QPointF center(width() / 2.0, height() / 2.0);
        if (icon_ == Icon::minimize) {
            painter.drawLine(QPointF(center.x() - 5, center.y() + 3),
                             QPointF(center.x() + 5, center.y() + 3));
        } else {
            painter.drawLine(QPointF(center.x() - 4, center.y() - 4),
                             QPointF(center.x() + 4, center.y() + 4));
            painter.drawLine(QPointF(center.x() + 4, center.y() - 4),
                             QPointF(center.x() - 4, center.y() + 4));
        }
    }

private:
    Icon icon_;
};

class GameSortProxyModel final : public QSortFilterProxyModel
{
protected:
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override
    {
        if (left.column() >= 3 && left.column() <= 5) {
            const QVariant leftSize = sourceModel()->data(left, Qt::UserRole);
            const QVariant rightSize = sourceModel()->data(right, Qt::UserRole);
            if (leftSize.isValid() && rightSize.isValid()) {
                return leftSize.toULongLong() < rightSize.toULongLong();
            }
        }
        return QSortFilterProxyModel::lessThan(left, right);
    }
};

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

QString directoryKey(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toCaseFolded();
}

QIcon aspectRatioIcon(const QImage& image)
{
    constexpr QSize iconCanvasSize(120, 56);
    if (image.isNull()) return {};

    const QImage fitted = image.scaled(iconCanvasSize,
                                       Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QImage canvas(iconCanvasSize, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    painter.drawImage((iconCanvasSize.width() - fitted.width()) / 2,
                      (iconCanvasSize.height() - fitted.height()) / 2, fitted);
    return QIcon(QPixmap::fromImage(canvas));
}

QIcon aspectRatioIcon(const QString& path)
{
    return aspectRatioIcon(QImage(path));
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    themeManager_ = new compresso::ThemeManager(
        *qobject_cast<QApplication*>(QApplication::instance()), this);
    const auto applyPalette = [this](const QPalette& palette) {
        const auto color = [&palette](QPalette::ColorRole role) {
            return palette.color(role).name(QColor::HexRgb);
        };
        const QString window = color(QPalette::Window);
        const QString windowText = color(QPalette::WindowText);
        const QString base = color(QPalette::Base);
        const QString alternateBase = color(QPalette::AlternateBase);
        const QString button = color(QPalette::Button);
        const QString buttonText = color(QPalette::ButtonText);
        const QString disabledButton = palette.color(QPalette::Disabled, QPalette::Button)
                                           .name(QColor::HexRgb);
        const QString disabledButtonText = palette.color(QPalette::Disabled, QPalette::ButtonText)
                                               .name(QColor::HexRgb);
        const QString mid = color(QPalette::Mid);
        const QString midlight = color(QPalette::Midlight);
        const QString highlight = color(QPalette::Highlight);
        const QString highlightedText = color(QPalette::HighlightedText);
        setPalette(palette);
        if (progressBar_) progressBar_->setPalette(palette);
        setStyleSheet(QStringLiteral(
            "QMainWindow{background:transparent;color:%2;}"
            "#libraryShell{background:%1;color:%2;border:1px solid %3;border-bottom:0;"
            "border-top-left-radius:12px;border-top-right-radius:12px;}"
            "#customTitleBar{background:%1;border-top-left-radius:11px;border-top-right-radius:11px;}"
            "#libraryCenter{background:%1;}"
            "#sidebar,#gameDetails{background:%1;border-right:1px solid %3;}"
            "#brand{font-size:18px;font-weight:650;padding:8px 4px;}"
            "QPushButton{background:%4;color:%5;border:1px solid %3;border-radius:5px;padding:7px 12px;}"
            "QPushButton:hover{background:%6;} QPushButton:checked{background:%7;border-color:%7;}"
            "QPushButton:disabled{background:%11;color:%12;border-color:%3;}"
            "QPushButton#windowControlButton,QPushButton#windowCloseButton{background:transparent;border:0;border-radius:5px;padding:0;font-size:16px;}"
            "QPushButton#windowControlButton:hover,QPushButton#windowCloseButton:hover{background:%6;}"
            "QPushButton#windowCloseButton:hover{background:#c42b1c;color:white;}"
            "QLineEdit,QComboBox{background:%8;border:1px solid %3;border-radius:5px;padding:7px;color:%2;}"
            "QFrame#statBox{background:%8;border:1px solid %3;border-radius:7px;}"
            "QLabel#gamesStat,QLabel#totalSizeStat,QLabel#onDiskStat,QLabel#savedStat{font-size:16px;font-weight:600;}"
            "QLabel#statCaption{color:%3;font-size:11px;}"
            "QLabel#selectedGameName{font-size:20px;font-weight:650;}"
            "QTableView{background:%8;alternate-background-color:%9;border:1px solid %3;"
            "selection-background-color:%7;selection-color:%10;outline:0;}"
            "QTableView::item:selected{background:%7;color:%10;border:0;border-radius:0;}"
            "QHeaderView::section{background:%1;color:%2;border:0;padding:7px 6px;}"
            "QStatusBar{background:%1;color:%2;border-left:1px solid %3;"
            "border-right:1px solid %3;border-bottom:1px solid %3;border-top:1px solid %3;"
            "border-bottom-left-radius:12px;border-bottom-right-radius:12px;}")
            .arg(window, windowText, mid, button, buttonText, midlight, highlight, base,
                 alternateBase, highlightedText, disabledButton, disabledButtonText));
    };
    connect(themeManager_, &compresso::ThemeManager::paletteApplied, this, applyPalette);
    applyPalette(qApp->palette());
    qRegisterMetaType<compresso::ScanResult>("compresso::ScanResult");
    qRegisterMetaType<compresso::CompressionReport>("compresso::CompressionReport");
    qRegisterMetaType<compresso::CompressionProgress>("compresso::CompressionProgress");
    qRegisterMetaType<compresso::OperationError>("compresso::OperationError");
    qRegisterMetaType<compresso::GameInstallation>("compresso::GameInstallation");
    qRegisterMetaType<QList<compresso::GameInstallation>>(
        "QList<compresso::GameInstallation>");

    setWindowTitle("Compresso");
    setWindowFlag(Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    QSettings settings;
    const QByteArray savedGeometry = settings.value(QStringLiteral("window/geometry"))
                                         .toByteArray();

    auto* content = new QWidget(this);
    content->setObjectName("mainContent");
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(18, 16, 18, 10);
    layout->setSpacing(12);

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
    actions->addWidget(compressButton_);
    actions->addWidget(restoreButton_);
    auto* openFolderButton = new QPushButton(QStringLiteral("Open folder"), content);
    openFolderButton->setObjectName(QStringLiteral("openFolderButton"));
    connect(openFolderButton, &QPushButton::clicked, this, [this] {
        if (!selectedGamePath_.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(selectedGamePath_));
    });

    layout->addWidget(separator);
    layout->addLayout(details);
    layout->addStretch(1);
    layout->addLayout(actions);
    layout->addWidget(openFolderButton);
    setCentralWidget(content);

    statusBar()->setObjectName("operationStatus");
    statusBar()->setSizeGripEnabled(false);
    if (auto* statusMessageLabel = statusBar()->findChild<QLabel*>()) {
        statusMessageLabel->setContentsMargins(16, 0, 0, 0);
    }
    statusBar()->showMessage("Select a detected game to get started.");
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

    connect(compressButton_, &QPushButton::clicked, this,
            [this] { startCompression(); });
    connect(restoreButton_, &QPushButton::clicked, this,
            [this] { startRestore(); });

    // Compact library shell with operations tied to detected game installations.
    auto* previousContent = takeCentralWidget();
    auto* shell = new QWidget(this);
    shell->setObjectName(QStringLiteral("libraryShell"));
    auto* shellLayout = new QVBoxLayout(shell);
    shellLayout->setContentsMargins(1, 1, 1, 1);
    shellLayout->setSpacing(0);

    auto* titleBar = new DraggableTitleBar(shell);
    titleBar->setObjectName(QStringLiteral("customTitleBar"));
    titleBar->setFixedHeight(42);
    auto* titleLayout = new QHBoxLayout(titleBar);
    titleLayout->setContentsMargins(20, 0, 8, 0);
    titleLayout->setSpacing(6);
    auto* windowIcon = new QLabel(titleBar);
    windowIcon->setObjectName(QStringLiteral("windowIcon"));
    windowIcon->setFixedSize(36, 36);
    windowIcon->setAlignment(Qt::AlignCenter);
    windowIcon->setAttribute(Qt::WA_TransparentForMouseEvents);
    windowIcon->setPixmap(QPixmap(QStringLiteral(":/resources/compresso-cup.png"))
        .scaled(QSize(36, 36), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    windowIcon->setToolTip(QStringLiteral("Compresso"));
    titleLayout->addWidget(windowIcon, 0, Qt::AlignBottom);
    titleLayout->addStretch();
    auto* minimizeButton = new WindowControlButton(WindowControlButton::Icon::minimize,
                                                    titleBar);
    minimizeButton->setObjectName(QStringLiteral("windowControlButton"));
    minimizeButton->setAccessibleName(QStringLiteral("Minimize"));
    titleLayout->addWidget(minimizeButton);
    connect(minimizeButton, &QPushButton::clicked, this, &QWidget::showMinimized);
    auto* closeWindowButton = new WindowControlButton(WindowControlButton::Icon::close,
                                                       titleBar);
    closeWindowButton->setObjectName(QStringLiteral("windowCloseButton"));
    closeWindowButton->setAccessibleName(QStringLiteral("Close"));
    titleLayout->addWidget(closeWindowButton);
    connect(closeWindowButton, &QPushButton::clicked, this, &QWidget::close);

    auto* shellContent = new QWidget(shell);
    auto* shellContentLayout = new QHBoxLayout(shellContent);
    shellContentLayout->setContentsMargins(0, 0, 0, 0);
    shellContentLayout->setSpacing(0);
    shellLayout->addWidget(titleBar);
    shellLayout->addWidget(shellContent, 1);

    auto* center = new QWidget(shellContent);
    center->setObjectName(QStringLiteral("libraryCenter"));
    auto* centerLayout = new QVBoxLayout(center);
    centerLayout->setContentsMargins(0, 0, 0, 0);
    auto* libraryPage = new QWidget(center);
    auto* libraryLayout = new QVBoxLayout(libraryPage);
    libraryLayout->setContentsMargins(16, 10, 4, 12);
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
        QStringLiteral("Unsupported"), QStringLiteral("Unavailable"),
        QStringLiteral("Pending analysis"),
        QStringLiteral("Analyzing"), QStringLiteral("Cancelled"), QStringLiteral("Unknown")});
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

    gameModel_ = new QStandardItemModel(0, 8, this);
    gameModel_->setHorizontalHeaderLabels({QStringLiteral("Game"), QStringLiteral("Launcher"),
        QStringLiteral("Drive"), QStringLiteral("Size"), QStringLiteral("On disk"),
        QStringLiteral("Saved"), QStringLiteral("Status"), QStringLiteral("Filter data")});
    gameProxy_ = new GameSortProxyModel;
    gameProxy_->setParent(this);
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
    gameTable_->setIconSize(QSize(120, 56));
    gameTable_->verticalHeader()->setDefaultSectionSize(64);
    gameTable_->horizontalHeader()->setStretchLastSection(false);
    gameTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 6; ++column)
        gameTable_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Interactive);
    gameTable_->setColumnWidth(1, 82);
    gameTable_->setColumnWidth(2, 52);
    gameTable_->setColumnWidth(3, 76);
    gameTable_->setColumnWidth(4, 82);
    gameTable_->setColumnWidth(5, 66);
    gameTable_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Interactive);
    gameTable_->setColumnWidth(6, 112);
    gameTable_->setColumnHidden(7, true);
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
        gameProxy_->setFilterKeyColumn(7);
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
    connect(gameTable_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex& current) {
        if (!current.isValid()) return;
        const int sourceRow = gameProxy_->mapToSource(current).row();
        auto* nameItem = gameModel_->item(sourceRow, 0);
        if (!nameItem) return;
        selectedGameName_->setText(nameItem->text());
        showGameArtwork(nameItem->data(Qt::UserRole + 1).toString());
        selectedGamePath_ = nameItem->data(Qt::UserRole).toString();
        updateSelectedGameActions();
    });

    centerLayout->addWidget(libraryPage);

    previousContent->setObjectName(QStringLiteral("gameDetails"));
    selectedGameName_ = new QLabel(QStringLiteral("No game selected"), previousContent);
    selectedGameName_->setObjectName(QStringLiteral("selectedGameName"));
    selectedGameName_->setStyleSheet(QStringLiteral("font-size:20px;font-weight:650;"));
    selectedArtwork_ = new QLabel(previousContent);
    selectedArtwork_->setObjectName(QStringLiteral("selectedGameArtwork"));
    selectedArtwork_->setAlignment(Qt::AlignCenter);
    selectedArtwork_->setMinimumHeight(128);
    selectedArtwork_->setMaximumHeight(180);
    selectedArtwork_->setScaledContents(false);
    artworkNetwork_ = new QNetworkAccessManager(this);
    showGameArtwork({});
    previousContent->setMinimumWidth(300);
    previousContent->setMaximumWidth(320);
    if (auto* oldLayout = qobject_cast<QVBoxLayout*>(previousContent->layout())) {
        oldLayout->setContentsMargins(16, 14, 16, 12);
        oldLayout->setSpacing(9);
        oldLayout->insertWidget(0, selectedGameName_);
        oldLayout->insertWidget(0, selectedArtwork_);
    }
    shellContentLayout->addWidget(center, 1);
    shellContentLayout->addWidget(previousContent);
    setCentralWidget(shell);
    setMinimumSize(420, 300);
    resize(1200, 760);
    settings.remove(QStringLiteral("directory/lastSelected"));
    if (!savedGeometry.isEmpty()) {
        if (restoreGeometry(savedGeometry) && size() == QSize(1480, 900)) {
            resize(1200, 760);
        }
    }
    setFixedSize(size());
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
                }
            });
    connect(worker, &compresso::OperationWorker::errorOccurred, this,
            [this, directory](const compresso::OperationError& error) {
                analyzedDirectoryIsEligible_ = false;
                compressButton_->setEnabled(false);
                restoreButton_->setEnabled(false);
                diagnosticLines_ = {diagnosticText(error)};
                detailsButton_->setVisible(true);
                if (activeOperationKind_ == compresso::OperationWorker::Kind::scan)
                    setGameAnalysisState(directory, QStringLiteral("Unknown"));
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
                displayCompressionResult(result, directory);
            });
    connect(worker, &compresso::OperationWorker::restorationCompleted, this,
            [this, directory](const compresso::CompressionReport& result) {
                progressTimer_->stop();
                if (result.target_eligible
                    && (result.restoredCount() > 0 || result.failedCount() > 0)) {
                    pendingRefreshPath_ = directory;
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
        const bool finishedGameAnalysis = !activeAnalysisPath_.isEmpty();
        if (operationThread_ == thread) {
            operationThread_ = nullptr;
        }
        thread->deleteLater();
        progressTimer_->stop();
        hasCompressionProgress_ = false;
        setOperationControlsEnabled(true);
        if (!activeAnalysisPath_.isEmpty()) {
            const QString key = directoryKey(activeAnalysisPath_);
            automaticAnalysisPaths_.remove(key);
            if (operationStopSource_.stop_requested()
                && !analyzedGamePaths_.contains(key)) {
                setGameAnalysisState(activeAnalysisPath_, QStringLiteral("Cancelled"));
            }
            activeAnalysisPath_.clear();
        }
        updateSelectedGameActions();
        cancelButton_->hide();
        progressBar_->hide();
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
        if (!automaticAnalysisTasks_.empty()) {
            QTimer::singleShot(0, this, [this] { startNextAutomaticAnalysis(); });
        } else if (finishedGameAnalysis && !operationStopSource_.stop_requested()) {
            statusBar()->showMessage(QStringLiteral("All games scanned"));
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
    if (!activeAnalysisPath_.isEmpty()) {
        for (const AnalysisTask& task : automaticAnalysisTasks_) {
            automaticAnalysisPaths_.remove(directoryKey(task.path));
            setGameAnalysisState(task.path, QStringLiteral("Cancelled"));
        }
        automaticAnalysisTasks_.clear();
    }
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
    if (enabled) {
        updateSelectedGameActions();
    } else {
        compressButton_->setEnabled(false);
        restoreButton_->setEnabled(false);
    }
}

void MainWindow::enqueueAutomaticAnalysis(const QString& directory, const QString& name)
{
    const QString key = directoryKey(directory);
    if (key.isEmpty() || analyzedGamePaths_.contains(key)
        || automaticAnalysisPaths_.contains(key)
        || compresso::directoryValidationError(directory).size() > 0) {
        return;
    }

    automaticAnalysisPaths_.insert(key);
    automaticAnalysisTasks_.push_back({name, directory});
    setGameAnalysisState(directory, QStringLiteral("Pending analysis"));
    if (!operationThread_ || !operationThread_->isRunning()) {
        QTimer::singleShot(0, this, [this] { startNextAutomaticAnalysis(); });
    }
}

void MainWindow::startNextAutomaticAnalysis()
{
    if (operationThread_ && operationThread_->isRunning()) return;

    while (!automaticAnalysisTasks_.empty()) {
        AnalysisTask task = std::move(automaticAnalysisTasks_.front());
        automaticAnalysisTasks_.erase(automaticAnalysisTasks_.begin());
        const QString key = directoryKey(task.path);
        if (analyzedGamePaths_.contains(key)) {
            automaticAnalysisPaths_.remove(key);
            continue;
        }
        activeAnalysisPath_ = task.path;
        setGameAnalysisState(task.path, QStringLiteral("Analyzing"));
        statusBar()->showMessage(QStringLiteral("Analyzing %1…").arg(task.name));
        analyzedDirectoryIsEligible_ = false;
        compressButton_->setEnabled(false);
        restoreButton_->setEnabled(false);
        startOperation(compresso::OperationWorker::Kind::scan, task.path);
        return;
    }
}

void MainWindow::setGameAnalysisState(const QString& directory, const QString& status)
{
    const QString key = directoryKey(directory);
    for (int row = 0; row < gameModel_->rowCount(); ++row) {
        auto* item = gameModel_->item(row, 0);
        if (!item || directoryKey(item->data(Qt::UserRole).toString()) != key) continue;
        gameModel_->item(row, 6)->setText(status);
        gameModel_->item(row, 7)->setText(QStringList{
            item->text(), gameModel_->item(row, 1)->text(), gameModel_->item(row, 2)->text(),
            status, directory}.join(QLatin1Char(' ')));
        break;
    }
    if (selectedGameName_->text().isEmpty()) return;
    const QModelIndex current = gameTable_->currentIndex();
    if (current.isValid()) {
        const int sourceRow = gameProxy_->mapToSource(current).row();
        auto* item = gameModel_->item(sourceRow, 0);
        if (item && directoryKey(item->data(Qt::UserRole).toString()) == key) {
            analyzedDirectoryIsEligible_ = false;
            compressButton_->setEnabled(false);
            restoreButton_->setEnabled(false);
        }
    }
}

void MainWindow::updateSelectedGameActions()
{
    const QModelIndex current = gameTable_->currentIndex();
    if (!current.isValid()) {
        selectedGamePath_.clear();
        analyzedDirectoryIsEligible_ = false;
        compressButton_->setEnabled(false);
        restoreButton_->setEnabled(false);
        return;
    }

    const int row = gameProxy_->mapToSource(current).row();
    auto* item = gameModel_->item(row, 0);
    if (!item) return;
    selectedGamePath_ = item->data(Qt::UserRole).toString();
    const QString status = gameModel_->item(row, 6)->text();
    selectedGameName_->setText(item->text());
    const bool idle = !operationThread_ || !operationThread_->isRunning();
    analyzedDirectoryIsEligible_ = item->data(Qt::UserRole + 3).toBool();
    compressButton_->setEnabled(idle && analyzedDirectoryIsEligible_
        && (status == QStringLiteral("Uncompressed") || status == QStringLiteral("Partial")));
    restoreButton_->setEnabled(idle && analyzedDirectoryIsEligible_
        && (status == QStringLiteral("Compressed") || status == QStringLiteral("Partial")));

    if (analyzedGamePaths_.contains(directoryKey(item->data(Qt::UserRole).toString()))) {
        const QLocale locale;
        originalSizeValue_->setText(locale.toString(
            item->data(Qt::UserRole + 4).toULongLong()) + QStringLiteral(" bytes"));
        compressedSizeValue_->setText(item->data(Qt::UserRole + 6).toBool()
            ? locale.toString(item->data(Qt::UserRole + 5).toULongLong())
                + QStringLiteral(" bytes")
            : QStringLiteral("Unavailable"));
        fileCountValue_->setText(locale.toString(item->data(Qt::UserRole + 7).toULongLong()));
        filesystemValue_->setText(item->data(Qt::UserRole + 8).toString());
    } else {
        originalSizeValue_->setText(QStringLiteral("—"));
        compressedSizeValue_->setText(QStringLiteral("—"));
        fileCountValue_->setText(QStringLiteral("—"));
        filesystemValue_->setText(QStringLiteral("—"));
    }
}

void MainWindow::displayScanResult(const compresso::ScanResult& result,
                                   const QString& directory)
{
    const bool updateSelectedGame = directoryKey(directory) == directoryKey(selectedGamePath_);

    if (result.cancelled) {
        setGameAnalysisState(directory, QStringLiteral("Cancelled"));
        if (!updateSelectedGame) {
            QString gameName = QFileInfo(directory).fileName();
            if (gameName.isEmpty()) gameName = directory;
            statusBar()->showMessage(QStringLiteral("%1: scan cancelled; partial results discarded.")
                                         .arg(gameName));
            return;
        }
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

    const QString key = directoryKey(directory);
    if (result.traversal_complete) analyzedGamePaths_.insert(key);
    automaticAnalysisPaths_.remove(key);

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
    QString launcher;
    if (modelRow >= 0) {
        gameName = gameModel_->item(modelRow, 0)->text();
        launcher = gameModel_->item(modelRow, 1)->text();
    } else return;
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
    const QStringList values{gameName, launcher, drive, sizeText,
                             diskText, savedText, status};
    for (int column = 0; column < values.size(); ++column) {
        gameModel_->item(modelRow, column)->setText(values[column]);
        if (column >= 1 && column <= 6)
            gameModel_->item(modelRow, column)->setTextAlignment(Qt::AlignCenter);
    }
    gameModel_->item(modelRow, 3)->setData(result.traversal_complete
        ? QVariant::fromValue(static_cast<qulonglong>(result.total_logical_bytes))
        : QVariant{}, Qt::UserRole);
    gameModel_->item(modelRow, 4)->setData(result.allocation_measurements_complete
        ? QVariant::fromValue(static_cast<qulonglong>(result.total_allocated_bytes))
        : QVariant{}, Qt::UserRole);
    const std::uintmax_t bytesSaved = result.total_logical_bytes > result.total_allocated_bytes
        ? result.total_logical_bytes - result.total_allocated_bytes : 0;
    gameModel_->item(modelRow, 5)->setData(result.allocation_measurements_complete
        ? QVariant::fromValue(static_cast<qulonglong>(bytesSaved)) : QVariant{}, Qt::UserRole);
    auto* gameItem = gameModel_->item(modelRow, 0);
    gameItem->setData(directory, Qt::UserRole);
    gameItem->setData(result.volume.compression_eligible && eligibleFiles > 0,
                      Qt::UserRole + 3);
    gameItem->setData(static_cast<qulonglong>(result.total_logical_bytes), Qt::UserRole + 4);
    gameItem->setData(static_cast<qulonglong>(result.total_allocated_bytes), Qt::UserRole + 5);
    gameItem->setData(result.allocation_measurements_complete, Qt::UserRole + 6);
    gameItem->setData(static_cast<qulonglong>(result.total_file_count), Qt::UserRole + 7);
    gameItem->setData(result.volume.filesystem_type.empty()
        ? QStringLiteral("Unavailable")
        : QString::fromStdWString(result.volume.filesystem_type)
            + (result.volume.compression_eligible
                ? QStringLiteral(" (LZX eligible)") : QStringLiteral(" (LZX unavailable)")),
        Qt::UserRole + 8);
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
    gameModel_->item(modelRow, 7)->setText(
        QStringList{gameName, launcher, drive, status, directory}.join(QLatin1Char(' ')));
    gamesStat_->setText(QString::number(gameModel_->rowCount()));
    const QModelIndex gameIndex = gameProxy_->mapFromSource(gameModel_->index(modelRow, 0));
    QString message = result.traversal_complete ? QStringLiteral("Scan complete: ")
                                                : QStringLiteral("Scan incomplete: ");
    message += locale.toString(static_cast<qulonglong>(result.total_file_count))
               + QStringLiteral(" files");
    if (!result.skipped_entries.empty()) {
        message += QStringLiteral("; %1 items skipped").arg(
            locale.toString(static_cast<qulonglong>(result.skipped_entries.size())));
    }
    if (!result.allocation_measurements_complete) {
        message += QStringLiteral("; allocated size unavailable for %1 files").arg(
            locale.toString(static_cast<qulonglong>(
                result.allocation_measurement_failures.size())));
    }
    if (!result.volume.compression_eligible && !result.volume.error) {
        message += QStringLiteral("; LZX unavailable on this volume");
    }
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

    if (!updateSelectedGame) {
        statusBar()->showMessage(gameName + QStringLiteral(": ") + message + QLatin1Char('.'));
        return;
    }
    diagnosticLines_.clear();
    for (const auto& error : result.errors) {
        diagnosticLines_.push_back(diagnosticText(error));
    }
    for (const auto& entry : result.skipped_entries) {
        diagnosticLines_.push_back(skippedEntryText(entry));
    }
    detailsButton_->setVisible(!diagnosticLines_.isEmpty());
    if (!diagnosticLines_.isEmpty()) message += QStringLiteral(". See Details");
    statusBar()->showMessage(message + QLatin1Char('.'));
    analyzedDirectoryIsEligible_ = result.volume.compression_eligible
                                   && eligibleFiles > 0;
    updateSelectedGameActions();
}

void MainWindow::displayDiscoveredGames(const QList<compresso::GameInstallation>& games)
{
    QSet<QString> launchers;
    QSet<QString> drives;
    QSet<QString> discoveredPaths;
    for (const auto& game : games) {
        const QString key = directoryKey(game.install_path);
        discoveredPaths.insert(key);
        const QString root = QDir(game.install_path).rootPath();
        const QString drive = root.size() >= 2 && root.at(1) == QLatin1Char(':')
            ? root.left(2).toUpper() : QStringLiteral("—");
        int rowIndex = -1;
        for (int row = 0; row < gameModel_->rowCount(); ++row) {
            auto* item = gameModel_->item(row, 0);
            if (item && directoryKey(item->data(Qt::UserRole).toString()) == key) {
                rowIndex = row;
                break;
            }
        }
        if (rowIndex < 0) {
            QList<QStandardItem*> row;
            for (int column = 0; column < 8; ++column) {
                row.push_back(new QStandardItem);
                if (column >= 1 && column <= 6)
                    row.back()->setTextAlignment(Qt::AlignCenter);
            }
            rowIndex = gameModel_->rowCount();
            gameModel_->appendRow(row);
            gameModel_->item(rowIndex, 0)->setData(false, Qt::UserRole + 3);
        }
        auto* gameItem = gameModel_->item(rowIndex, 0);
        gameItem->setText(game.name);
        gameItem->setData(game.install_path, Qt::UserRole);
        gameItem->setData(game.id, Qt::UserRole + 1);
        gameItem->setToolTip(game.install_path);
        gameModel_->item(rowIndex, 1)->setText(game.launcher);
        gameModel_->item(rowIndex, 2)->setText(drive);
        const QString cachePath = QDir(
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                                      .filePath(QStringLiteral("artwork/%1.jpg").arg(game.id));
        const bool hasCachedArtwork = QFileInfo::exists(cachePath);
        gameItem->setData(cachePath, Qt::UserRole + 2);
        gameItem->setIcon(hasCachedArtwork ? aspectRatioIcon(cachePath)
                                           : style()->standardIcon(QStyle::SP_MediaPlay));
        if (!game.available) {
            analyzedGamePaths_.remove(key);
            automaticAnalysisPaths_.remove(key);
            std::erase_if(automaticAnalysisTasks_, [&key](const AnalysisTask& task) {
                return directoryKey(task.path) == key;
            });
            gameItem->setData(false, Qt::UserRole + 3);
            gameModel_->item(rowIndex, 6)->setText(QStringLiteral("Unavailable"));
            gameModel_->item(rowIndex, 6)->setToolTip(
                QStringLiteral("The manifest exists, but the installation directory is missing."));
        } else if (analyzedGamePaths_.contains(key)) {
            // Keep the completed analysis values when refreshing the library.
        } else {
            gameModel_->item(rowIndex, 6)->setText(QStringLiteral("Pending analysis"));
            gameModel_->item(rowIndex, 6)->setToolTip(
                QStringLiteral("Analysis starts automatically for newly detected games."));
            enqueueAutomaticAnalysis(game.install_path, game.name);
        }
        gameModel_->item(rowIndex, 7)->setText(QStringList{
            game.name, game.launcher, drive, gameModel_->item(rowIndex, 6)->text(),
            game.install_path}.join(QLatin1Char(' ')));
        launchers.insert(game.launcher);
        drives.insert(drive);
    }

    for (int row = gameModel_->rowCount() - 1; row >= 0; --row) {
        auto* item = gameModel_->item(row, 0);
        if (!item || item->data(Qt::UserRole).toString().isEmpty()) continue;
        const QString key = directoryKey(item->data(Qt::UserRole).toString());
        if (!discoveredPaths.contains(key)) {
            analyzedGamePaths_.remove(key);
            automaticAnalysisPaths_.remove(key);
            gameModel_->removeRow(row);
        }
    }
    std::erase_if(automaticAnalysisTasks_, [this, &discoveredPaths](const AnalysisTask& task) {
        const QString key = directoryKey(task.path);
        if (discoveredPaths.contains(key)) return false;
        automaticAnalysisPaths_.remove(key);
        return true;
    });

    launcherFilter_->clear();
    launcherFilter_->addItem(QStringLiteral("All launchers"));
    for (const QString& launcher : std::as_const(launchers)) launcherFilter_->addItem(launcher);
    driveFilter_->clear();
    driveFilter_->addItem(QStringLiteral("All drives"));
    for (const QString& drive : std::as_const(drives)) {
        if (drive != QStringLiteral("—")) driveFilter_->addItem(drive);
    }
    gamesStat_->setText(QString::number(gameModel_->rowCount()));
    if (!gameTable_->currentIndex().isValid()) {
        totalSizeStat_->setText(QStringLiteral("—"));
        onDiskStat_->setText(QStringLiteral("—"));
        savedStat_->setText(QStringLiteral("—"));
        selectedGameName_->setText(games.isEmpty()
            ? QStringLiteral("No Steam games found") : QStringLiteral("Select a game"));
        showGameArtwork({});
        updateSelectedGameActions();
    }
    const bool allAvailableGamesScanned = !games.isEmpty()
        && std::all_of(games.cbegin(), games.cend(), [this](const auto& game) {
            return !game.available || analyzedGamePaths_.contains(directoryKey(game.install_path));
        });
    if (allAvailableGamesScanned && automaticAnalysisTasks_.empty()) {
        statusBar()->showMessage(QStringLiteral("All games scanned"));
    } else {
        statusBar()->showMessage(games.isEmpty()
            ? QStringLiteral("No Steam game installations found.")
            : QStringLiteral("Found %1 Steam games. New games will be analyzed automatically.")
                  .arg(games.size()));
    }
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
        const QIcon icon = aspectRatioIcon(image);
        for (int row = 0; row < gameModel_->rowCount(); ++row) {
            auto* item = gameModel_->item(row, 0);
            if (item && item->data(Qt::UserRole + 1).toString() == appId) {
                item->setIcon(icon);
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
            const QByteArray responseBody = reply->readAll();
            QUrl artworkUrl;
            if (reply->error() == QNetworkReply::NoError) {
                const QJsonObject app = QJsonDocument::fromJson(responseBody).object()
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
        const QByteArray responseBody = reply->readAll();
        QImage image;
        if (reply->error() == QNetworkReply::NoError && image.loadFromData(responseBody)
            && image.width() >= 100 && image.height() >= 60) {
            QFile file(cachePath);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(responseBody);
                file.close();
            }
            const QIcon icon = aspectRatioIcon(image);
            for (int row = 0; row < gameModel_->rowCount(); ++row) {
                auto* item = gameModel_->item(row, 0);
                if (item && item->data(Qt::UserRole + 1).toString() == appId) {
                    item->setIcon(icon);
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

void MainWindow::startCompression()
{
    if (operationThread_ && operationThread_->isRunning()) {
        return;
    }
    if (!analyzedDirectoryIsEligible_ || selectedGamePath_.isEmpty()) {
        return;
    }
    analyzedDirectoryIsEligible_ = false;
    compressionClock_.start();
    hasCompressionProgress_ = false;
    progressTimer_->start();
    startOperation(compresso::OperationWorker::Kind::compress, selectedGamePath_);
}

void MainWindow::startRestore()
{
    if (operationThread_ && operationThread_->isRunning()) {
        return;
    }
    if (!analyzedDirectoryIsEligible_ || selectedGamePath_.isEmpty()) {
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
    startOperation(compresso::OperationWorker::Kind::restore, selectedGamePath_);
}

void MainWindow::displayCompressionResult(const compresso::CompressionReport& result,
                                          const QString& directory)
{
    if (directoryKey(directory) != directoryKey(selectedGamePath_)) {
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
    if (directoryKey(directory) != directoryKey(selectedGamePath_)) {
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
        message += ". A follow-up scan will refresh the allocated size.";
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
