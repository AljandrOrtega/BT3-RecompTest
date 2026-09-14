#include "archive_extract.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include <archive.h>
#include <archive_entry.h>

namespace
{
QString archiveError(struct archive *a)
{
    const char *s = archive_error_string(a);
    return s ? QString::fromUtf8(s) : QStringLiteral("archive error");
}

bool openArchive(struct archive **out, const QString &path, QString *err)
{
    struct archive *a = archive_read_new();
    if (!a)
    {
        if (err) *err = QStringLiteral("out of memory");
        return false;
    }
    // Enable every reader/filter libarchive was built with: 7z, zip, tar, rar,
    // gzip/xz/bzip2/zstd, ...
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    // 64 KiB read blocks keep memory bounded on multi-GB solid archives.
    if (archive_read_open_filename(a, QFile::encodeName(path).constData(), 1 << 16) != ARCHIVE_OK)
    {
        if (err) *err = archiveError(a);
        archive_read_free(a);
        return false;
    }
    *out = a;
    return true;
}
} // namespace

namespace archivex
{
bool totalBytes(const QString &archivePath, quint64 *total, QString *err)
{
    struct archive *a = nullptr;
    if (!openArchive(&a, archivePath, err))
        return false;

    quint64 sum = 0;
    struct archive_entry *e = nullptr;
    for (;;)
    {
        const int r = archive_read_next_header(a, &e);
        if (r == ARCHIVE_EOF)
            break;
        if (r == ARCHIVE_WARN)
            continue;   // recoverable: keep walking
        if (r < ARCHIVE_WARN)
        {
            if (err) *err = archiveError(a);
            archive_read_free(a);
            return false;
        }
        if (archive_entry_filetype(e) == AE_IFREG && archive_entry_size(e) > 0)
            sum += static_cast<quint64>(archive_entry_size(e));
        archive_read_data_skip(a);
    }

    archive_read_free(a);
    if (total) *total = sum;
    return true;
}

bool extract(const QString &archivePath, const QString &destDir,
             const ProgressFn &progress, QString *err)
{
    quint64 total = 0;
    totalBytes(archivePath, &total, nullptr);

    struct archive *a = nullptr;
    if (!openArchive(&a, archivePath, err))
        return false;

    const QString root = QDir(destDir).absolutePath();
    if (!QDir().mkpath(root))
    {
        if (err) *err = QStringLiteral("failed to create %1").arg(root);
        archive_read_free(a);
        return false;
    }

    quint64 done = 0;
    QElapsedTimer tick;
    tick.start();
    QByteArray chunk(1 << 20, Qt::Uninitialized);

    struct archive_entry *e = nullptr;
    for (;;)
    {
        const int r = archive_read_next_header(a, &e);
        if (r == ARCHIVE_EOF)
            break;
        if (r == ARCHIVE_WARN)
            continue;
        if (r < ARCHIVE_WARN)
        {
            if (err) *err = archiveError(a);
            archive_read_free(a);
            return false;
        }

        QString rel = QString::fromUtf8(archive_entry_pathname(e));
        while (rel.startsWith(QLatin1Char('/')))
            rel.remove(0, 1);
        if (rel.isEmpty())
        {
            archive_read_data_skip(a);
            continue;
        }

        // Reject zip-slip: the resolved path must stay under destDir.
        const QString outPath = QDir::cleanPath(root + QLatin1Char('/') + rel);
        if (outPath != root && !outPath.startsWith(root + QLatin1Char('/')))
        {
            if (err) *err = QStringLiteral("unsafe path in archive: %1").arg(rel);
            archive_read_free(a);
            return false;
        }

        const auto ft = archive_entry_filetype(e);
        if (ft == AE_IFDIR)
        {
            QDir().mkpath(outPath);
            archive_read_data_skip(a);
            continue;
        }
        if (ft != AE_IFREG)
        {
            // Texture packs / disc images contain no symlinks or devices.
            archive_read_data_skip(a);
            continue;
        }

        QDir().mkpath(QFileInfo(outPath).absolutePath());
        QFile f(outPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            if (err) *err = QStringLiteral("failed to write %1").arg(outPath);
            archive_read_free(a);
            return false;
        }

        for (;;)
        {
            const la_ssize_t n = archive_read_data(a, chunk.data(),
                                                   static_cast<size_t>(chunk.size()));
            if (n < 0)
            {
                if (err) *err = archiveError(a);
                f.close();
                archive_read_free(a);
                return false;
            }
            if (n == 0)
                break;
            if (f.write(chunk.constData(), n) != n)
            {
                if (err) *err = QStringLiteral("failed to write %1").arg(outPath);
                f.close();
                archive_read_free(a);
                return false;
            }
            done += static_cast<quint64>(n);
            if (tick.elapsed() >= 100)
            {
                tick.restart();
                if (progress && !progress(done, total))
                {
                    if (err) *err = QStringLiteral("cancelled");
                    f.close();
                    archive_read_free(a);
                    return false;
                }
            }
        }
        f.close();
    }

    archive_read_free(a);
    if (progress)
        progress(done, total);
    return true;
}
} // namespace archivex

void ArchiveExtractWorker::doWork(const QString &archivePath, const QString &destDir)
{
    QString err;
    const bool ok = archivex::extract(
        archivePath, destDir,
        [this](quint64 done, quint64 total) {
            emit progress(static_cast<qint64>(done), static_cast<qint64>(total));
            return m_cancel.loadRelaxed() == 0;
        },
        &err);
    emit done(ok, err);
}
