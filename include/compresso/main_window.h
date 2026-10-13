#pragma once

#include "compresso/compression_backend.h"
#include "compresso/directory_scanner.h"
#include "compresso/operation_worker.h"
#include "compresso/theme_manager.h"

#include <QMainWindow>
#include <QElapsedTimer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <stop_token>
#include <vector>

class QLineEdit;
class QAction;
class QLabel;
class QPushButton;
class QThread;
class QCloseEvent;
class QProgressBar;
class QTimer;
class QTableView;
class QStandardItemModel;
class QSortFilterProxyModel;
class QComboBox;
class QLineEdit;
class QLabel;
class QStackedWidget;
class QNetworkAccessManager;

class MainWindow final : public QMainWindow
{
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
    void refreshLibrary();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void startOperation(compresso::OperationWorker::Kind kind, const QString& directory);
    void cancelOperation();
    void setOperationControlsEnabled(bool enabled);
    void browseForDirectory();
    void handleDirectoryTextChanged();
    void validateDirectoryPath();
    void startDirectoryScan();
    void displayScanResult(const compresso::ScanResult& result, const QString& directory);
    void startCompression();
    void startRestore();
    void displayCompressionResult(const compresso::CompressionReport& result,
                                  const QString& directory);
    void displayRestoreResult(const compresso::CompressionReport& result,
                              const QString& directory);
    void displayCompressionProgress(const compresso::CompressionProgress& progress);
    void showDiagnostics();
    void displayDiscoveredGames(const QList<compresso::GameInstallation>& games);
    void queueSelectedGame(compresso::OperationWorker::Kind kind);
    void startQueue();
    void startNextQueueTask();
    void updateQueueTask(int index, const QString& progress, const QString& status);
    void requestArtwork(const QString& appId, int variant = 0);
    void showGameArtwork(const QString& appId);

    QLineEdit* directoryPath_ = nullptr;
    QLabel* originalSizeLabel_ = nullptr;
    QLabel* compressedSizeLabel_ = nullptr;
    QLabel* originalSizeValue_ = nullptr;
    QLabel* compressedSizeValue_ = nullptr;
    QLabel* fileCountValue_ = nullptr;
    QLabel* filesystemValue_ = nullptr;
    QPushButton* browseButton_ = nullptr;
    QPushButton* analyzeButton_ = nullptr;
    QPushButton* compressButton_ = nullptr;
    QPushButton* restoreButton_ = nullptr;
    QPushButton* detailsButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QTimer* progressTimer_ = nullptr;
    QElapsedTimer compressionClock_;
    compresso::CompressionProgress latestCompressionProgress_;
    bool hasCompressionProgress_ = false;
    QThread* operationThread_ = nullptr;
    QString validatedDirectory_;
    bool closeAfterOperation_ = false;
    bool analyzedDirectoryIsEligible_ = false;
    compresso::OperationWorker::Kind activeOperationKind_ = compresso::OperationWorker::Kind::scan;
    bool activeOperationIsRestore_ = false;
    std::stop_source operationStopSource_;
    QStringList diagnosticLines_;
    compresso::ThemeManager* themeManager_ = nullptr;
    QAction* systemThemeAction_ = nullptr;
    QAction* lightThemeAction_ = nullptr;
    QAction* darkThemeAction_ = nullptr;
    QTableView* gameTable_ = nullptr;
    QStandardItemModel* gameModel_ = nullptr;
    QSortFilterProxyModel* gameProxy_ = nullptr;
    QLineEdit* searchField_ = nullptr;
    QComboBox* launcherFilter_ = nullptr;
    QComboBox* driveFilter_ = nullptr;
    QComboBox* statusFilter_ = nullptr;
    QLabel* gamesStat_ = nullptr;
    QLabel* totalSizeStat_ = nullptr;
    QLabel* onDiskStat_ = nullptr;
    QLabel* savedStat_ = nullptr;
    QLabel* selectedGameName_ = nullptr;
    QLabel* selectedLauncher_ = nullptr;
    QLabel* selectedStatus_ = nullptr;
    QLabel* selectedArtwork_ = nullptr;
    QNetworkAccessManager* artworkNetwork_ = nullptr;
    QSet<QString> artworkRequests_;
    QString selectedArtworkAppId_;
    QStackedWidget* centerPages_ = nullptr;
    QStandardItemModel* queueModel_ = nullptr;
    QTableView* queueTable_ = nullptr;
    QPushButton* startQueueButton_ = nullptr;
    QPushButton* cancelQueueButton_ = nullptr;
    struct QueueTask
    {
        QString name;
        QString path;
        compresso::OperationWorker::Kind kind = compresso::OperationWorker::Kind::compress;
        QString status = QStringLiteral("Queued");
    };
    std::vector<QueueTask> queueTasks_;
    bool queueRunning_ = false;
    int activeQueueTask_ = -1;
};
