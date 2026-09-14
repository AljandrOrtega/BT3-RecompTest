#pragma once
// [texreplace] In-process archive extraction via libarchive.
//
// The launcher used to shell out to 7z/unrar/bsdtar/tar, which made it depend
// on whatever the host distro happened to ship (and on argument differences:
// e.g. unrar has no -bsp1 switch, which aborted with exit code 7). libarchive
// reads 7z/zip/tar/rar/... and is linked into the launcher, so extraction is
// identical on every platform and needs no external tool.

#include <QAtomicInt>
#include <QObject>
#include <QString>

#include <functional>

namespace archivex
{
// Sum of the uncompressed sizes of all regular-file entries. Reading the header
// only (this is cheap even for huge solid 7z archives). Returns false on error.
bool totalBytes(const QString &archivePath, quint64 *total, QString *err);

// Extract every entry of archivePath into destDir, creating parents. Entries
// whose path escapes destDir are rejected. progress(done, total) is called
// periodically and may return false to cancel. Returns false on error/cancel.
using ProgressFn = std::function<bool(quint64 done, quint64 total)>;
bool extract(const QString &archivePath, const QString &destDir,
             const ProgressFn &progress, QString *err);
} // namespace archivex

// Runs archivex::extract() on a worker thread so the dialog keeps painting.
class ArchiveExtractWorker : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    void requestCancel() { m_cancel.storeRelaxed(1); }

public slots:
    void doWork(const QString &archivePath, const QString &destDir);

signals:
    void progress(qint64 done, qint64 total);
    void done(bool ok, const QString &msg);

private:
    QAtomicInt m_cancel{0};
};
