#include "compresso/operation_worker.h"

#include <exception>

namespace compresso
{

OperationWorker::OperationWorker(Kind kind, std::filesystem::path directory,
                                 std::stop_token cancellation)
    : kind_(kind), directory_(std::move(directory)), cancellation_(cancellation)
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
                }, cancellation_);
            emit scanCompleted(std::move(result));
        } else if (kind_ == Kind::compress) {
            emit progressChanged(QStringLiteral("Preparing transparent NTFS LZX compression…"));
            CompressionReport result = CompressionBackend::compressDirectory(
                directory_, [this](const CompressionProgress& progress) {
                    emit compressionProgress(progress);
                }, {}, cancellation_);
            emit compressionCompleted(std::move(result));
        } else {
            emit progressChanged(QStringLiteral("Preparing transparent NTFS LZX restoration…"));
            CompressionReport result = CompressionBackend::restoreDirectory(
                directory_, [this](const CompressionProgress& progress) {
                    emit compressionProgress(progress);
                }, {}, cancellation_);
            emit restorationCompleted(std::move(result));
        }
    } catch (const std::exception& exception) {
        OperationError error;
        error.code = OperationErrorCode::unexpected_failure;
        error.technical_details = exception.what();
        emit errorOccurred(std::move(error));
    } catch (...) {
        OperationError error;
        error.code = OperationErrorCode::unexpected_failure;
        error.technical_details = "A non-standard C++ exception was thrown.";
        emit errorOccurred(std::move(error));
    }
    emit finished();
}

} // namespace compresso
