#pragma once

#include "compresso/compression_backend.h"
#include "compresso/directory_scanner.h"

#include <QObject>

namespace compresso
{

class OperationWorker final : public QObject
{
    Q_OBJECT

public:
    enum class Kind
    {
        scan,
        compress
    };

    OperationWorker(Kind kind, std::filesystem::path directory);

public slots:
    void run();

signals:
    void scanCompleted(compresso::ScanResult result);
    void compressionCompleted(compresso::CompressionReport result);
    void progressChanged(QString message);
    void errorOccurred(QString message);
    void finished();

private:
    Kind kind_;
    std::filesystem::path directory_;
};

} // namespace compresso

Q_DECLARE_METATYPE(compresso::ScanResult)
Q_DECLARE_METATYPE(compresso::CompressionReport)
