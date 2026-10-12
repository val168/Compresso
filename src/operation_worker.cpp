#include "compresso/operation_worker.h"

#include <exception>

namespace compresso
{

OperationWorker::OperationWorker(Kind kind, std::filesystem::path directory)
    : kind_(kind), directory_(std::move(directory))
{
}

void OperationWorker::run()
{
    try {
        if (kind_ == Kind::scan) {
            emit progressChanged(QStringLiteral("Scanning directory…"));
            ScanResult result = DirectoryScanner::scan(
                directory_, [this](std::size_t entries, std::size_t files) {
                    if ((entries & 0x1ffU) == 0U) {
                        emit progressChanged(
                            QStringLiteral("Scanning directory… %1 files found")
                                .arg(static_cast<qulonglong>(files)));
                    }
                });
            emit scanCompleted(std::move(result));
        } else {
            emit progressChanged(QStringLiteral("Preparing transparent NTFS LZX compression…"));
            CompressionReport result = CompressionBackend::compressDirectory(
                directory_, [this](std::size_t completed, std::size_t total) {
                    if (total == 0) {
                        emit progressChanged(
                            QStringLiteral("Scanning before LZX compression… %1 entries")
                                .arg(static_cast<qulonglong>(completed)));
                    } else {
                        emit progressChanged(
                            QStringLiteral("Applying LZX compression… %1 of %2 files")
                                .arg(static_cast<qulonglong>(completed))
                                .arg(static_cast<qulonglong>(total)));
                    }
                });
            emit compressionCompleted(std::move(result));
        }
    } catch (const std::exception& exception) {
        emit errorOccurred(QString::fromLocal8Bit(exception.what()));
    } catch (...) {
        emit errorOccurred(QStringLiteral("The operation failed unexpectedly."));
    }
    emit finished();
}

} // namespace compresso
