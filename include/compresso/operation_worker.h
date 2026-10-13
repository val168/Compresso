#pragma once

#include "compresso/compression_backend.h"
#include "compresso/directory_scanner.h"
#include "compresso/game_discovery.h"

#include <QObject>
#include <stop_token>

namespace compresso
{

class OperationWorker final : public QObject
{
    Q_OBJECT

public:
    enum class Kind
    {
        scan,
        compress,
        restore,
        discover_steam
    };

    OperationWorker(Kind kind, std::filesystem::path directory,
                    std::stop_token cancellation);

public slots:
    void run();

signals:
    void scanCompleted(compresso::ScanResult result);
    void compressionCompleted(compresso::CompressionReport result);
    void restorationCompleted(compresso::CompressionReport result);
    void gamesDiscovered(QList<compresso::GameInstallation> games);
    void compressionProgress(compresso::CompressionProgress progress);
    void progressChanged(QString message);
    void errorOccurred(compresso::OperationError error);
    void finished();

private:
    Kind kind_;
    std::filesystem::path directory_;
    std::stop_token cancellation_;
};

} // namespace compresso

Q_DECLARE_METATYPE(compresso::ScanResult)
Q_DECLARE_METATYPE(compresso::CompressionReport)
Q_DECLARE_METATYPE(compresso::CompressionProgress)
Q_DECLARE_METATYPE(compresso::OperationError)
