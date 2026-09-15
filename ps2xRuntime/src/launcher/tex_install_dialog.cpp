#include "tex_install_dialog.h"

#include "app_paths.h"
#include "settings_manager.h"
#include "tex_pack.h"

#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressBar>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

namespace
{
QString mb(qint64 bytes)
{
    return QString::number(bytes / (1024.0 * 1024.0), 'f', 1);
}

QString etaString(qint64 seconds)
{
    if (seconds < 0) return QStringLiteral("--:--");
    return QStringLiteral("%1:%2")
        .arg(seconds / 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}
} // namespace

TexInstallDialog::TexInstallDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Install texture pack"));
    setMinimumWidth(460);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(8);

    auto *title = new QLabel(QStringLiteral("Texture pack (Texture-4k.7z)"));
    title->setObjectName(QStringLiteral("sectionLabel"));
    root->addWidget(title);

    m_status = new QLabel(QStringLiteral("Choose a source: a local file or download."));
    m_status->setObjectName(QStringLiteral("hintLabel"));
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    m_dlBar = new QProgressBar;
    m_dlBar->setRange(0, 100);
    m_dlBar->setValue(0);
    m_dlBar->setVisible(false);
    root->addWidget(m_dlBar);

    m_dlInfo = new QLabel;
    m_dlInfo->setObjectName(QStringLiteral("hintLabel"));
    m_dlInfo->setVisible(false);
    root->addWidget(m_dlInfo);

    m_exBar = new QProgressBar;
    m_exBar->setRange(0, 100);
    m_exBar->setValue(0);
    m_exBar->setVisible(false);
    root->addWidget(m_exBar);

    auto *row = new QHBoxLayout;
    m_browse = new QPushButton(QStringLiteral("Browse…"));
    m_browse->setObjectName(QStringLiteral("wizardButton"));
    m_browse->setCursor(Qt::PointingHandCursor);
    m_download = new QPushButton(QStringLiteral("Download"));
    m_download->setObjectName(QStringLiteral("wizardButton"));
    m_download->setCursor(Qt::PointingHandCursor);
    m_close = new QPushButton(QStringLiteral("Close"));
    m_close->setObjectName(QStringLiteral("wizardButton"));
    m_close->setCursor(Qt::PointingHandCursor);
    row->addWidget(m_browse);
    row->addWidget(m_download);
    row->addStretch(1);
    row->addWidget(m_close);
    root->addLayout(row);

    connect(m_browse, &QPushButton::clicked, this, &TexInstallDialog::onBrowse);
    connect(m_download, &QPushButton::clicked, this, &TexInstallDialog::onDownload);
    connect(m_close, &QPushButton::clicked, this, &QDialog::close);
}

TexInstallDialog::~TexInstallDialog()
{
    abortDownload();
    abortExtract();
}

void TexInstallDialog::setStatus(const QString &text)
{
    m_status->setText(text);
}

void TexInstallDialog::fail(const QString &text)
{
    setStatus(text);
    QMessageBox::warning(this, QStringLiteral("Install texture pack"), text);
    m_browse->setEnabled(true);
    m_download->setEnabled(true);
}

// ---------------------------------------------------------------------------
// Browse
// ---------------------------------------------------------------------------

void TexInstallDialog::onBrowse()
{
    const QString file = QFileDialog::getOpenFileName(
        this, QStringLiteral("Select texture pack archive"), QString(),
        QStringLiteral("Texture packs (*.7z *.zip *.rar *.tar *.tar.gz);;All files (*)"));
    if (file.isEmpty())
        return;

    setStatus(QStringLiteral("Verifying pack…"));
    if (texpack::sha256File(file).compare(QLatin1String(texpack::kSha256), Qt::CaseInsensitive) != 0)
    {
        fail(QStringLiteral("This file is not the supported texture pack (sha256 mismatch)."));
        return;
    }
    beginExtract(file);
}

// ---------------------------------------------------------------------------
// Download
// ---------------------------------------------------------------------------

void TexInstallDialog::onDownload()
{
    m_browse->setEnabled(false);
    m_download->setEnabled(false);
    m_dlBar->setVisible(true);
    m_dlBar->setRange(0, 0);   // busy until Content-Length arrives
    m_dlBar->setValue(0);
    m_dlInfo->setVisible(true);
    m_dlInfo->setText(QStringLiteral("Starting download…"));
    setStatus(QStringLiteral("Downloading Texture-4k.7z…"));

    m_tmp = new QTemporaryDir;
    if (!m_tmp->isValid())
    {
        fail(QStringLiteral("Could not create a temporary folder."));
        return;
    }
    m_tmpPath = m_tmp->filePath(QLatin1String(texpack::kFileName));
    m_out = new QFile(m_tmpPath);
    if (!m_out->open(QIODevice::WriteOnly))
    {
        fail(QStringLiteral("Could not open the temporary file for writing."));
        return;
    }

    m_dlClock = new QElapsedTimer;
    m_dlClock->start();

    m_nam = new QNetworkAccessManager(this);
    const QUrl dlUrl = QUrl(QString::fromLatin1(texpack::kUrl));
    QNetworkRequest req(dlUrl);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("BT3-Recomp-Launcher/1.0"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::readyRead, this, &TexInstallDialog::onDownloadReadyRead);
    connect(m_reply, &QNetworkReply::downloadProgress, this, &TexInstallDialog::onDownloadProgress);
    connect(m_reply, &QNetworkReply::finished, this, &TexInstallDialog::onDownloadFinished);
}

void TexInstallDialog::onDownloadReadyRead()
{
    if (m_reply && m_out)
        m_out->write(m_reply->readAll());   // incremental small chunks
}

void TexInstallDialog::onDownloadProgress(qint64 received, qint64 total)
{
    if (total > 0)
    {
        if (m_dlBar->maximum() != static_cast<int>(total / 1024))
        {
            m_dlBar->setRange(0, static_cast<int>(total / 1024));
        }
        m_dlBar->setValue(static_cast<int>(received / 1024));
    }

    const qint64 ms = m_dlClock ? m_dlClock->elapsed() : 0;
    const double speed = (ms > 0) ? (received / (ms / 1000.0)) : 0.0;   // bytes/s
    QString info = QStringLiteral("%1 / %2 MB")
                       .arg(mb(received), total > 0 ? mb(total) : QStringLiteral("?"));
    if (speed > 0.0)
    {
        info += QStringLiteral("  —  %1 MB/s").arg(speed / (1024.0 * 1024.0), 0, 'f', 1);
        if (total > 0)
        {
            const qint64 remain = static_cast<qint64>((total - received) / speed);
            info += QStringLiteral("  —  ETA %1").arg(etaString(remain));
        }
    }
    m_dlInfo->setText(info);
}

void TexInstallDialog::onDownloadFinished()
{
    if (m_out)
    {
        m_out->write(m_reply->readAll());   // drain anything still buffered
        m_out->close();
        m_out->deleteLater();
        m_out = nullptr;
    }

    const auto err = m_reply->error();
    const QString errStr = m_reply->errorString();
    m_reply->deleteLater();
    m_reply = nullptr;

    if (err != QNetworkReply::NoError)
    {
        // The file host (pixeldrain) only allows direct API downloads
        // ("hotlinking") for paid accounts and answers HTTP 403 otherwise, so a
        // plain "Download failed" dead-ends the user. Offer the browser page
        // instead: download there, then install with "Browse…".
        m_browse->setEnabled(true);
        m_download->setEnabled(true);
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(QStringLiteral("Install texture pack"));
        box.setText(QStringLiteral("Direct download is not available."));
        box.setInformativeText(QStringLiteral(
            "The file host does not allow direct downloads for this launcher. "
            "Open the download page in your browser, save %1, then use \"Browse…\" "
            "to install the file.\n\nDetails: %2")
            .arg(QString::fromLatin1(texpack::kFileName), errStr));
        QPushButton *openBtn = box.addButton(QStringLiteral("Open download page"), QMessageBox::AcceptRole);
        box.addButton(QStringLiteral("Close"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == openBtn)
            QDesktopServices::openUrl(QUrl(QString::fromLatin1(texpack::kPageUrl)));
        setStatus(QStringLiteral("Direct download unavailable — open the page in your browser, then use Browse…"));
        return;
    }

    setStatus(QStringLiteral("Download complete. Verifying…"));
    if (texpack::sha256File(m_tmpPath).compare(QLatin1String(texpack::kSha256), Qt::CaseInsensitive) != 0)
    {
        fail(QStringLiteral("The downloaded file failed the sha256 check."));
        return;
    }
    beginExtract(m_tmpPath);
}

// ---------------------------------------------------------------------------
// Extract
// ---------------------------------------------------------------------------

void TexInstallDialog::beginExtract(const QString &archivePath)
{
    m_dest = texpack::dir();
    QDir().mkpath(m_dest);

    m_exBar->setVisible(true);
    m_exBar->setRange(0, 0);   // busy until the worker reports the total
    m_exBar->setValue(0);
    setStatus(QStringLiteral("Extracting to %1…").arg(m_dest));

    // libarchive runs in-process on a worker thread so the dialog stays live.
    m_extThread = new QThread;
    m_worker = new ArchiveExtractWorker;
    m_worker->moveToThread(m_extThread);
    connect(m_extThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_extThread, &QThread::finished, m_extThread, &QObject::deleteLater);
    connect(m_worker, &ArchiveExtractWorker::progress, this, &TexInstallDialog::onExtractProgress);
    connect(m_worker, &ArchiveExtractWorker::done, this, &TexInstallDialog::onExtractDone);
    connect(m_worker, &ArchiveExtractWorker::done, m_extThread, &QThread::quit);
    m_extThread->start();

    QMetaObject::invokeMethod(m_worker, "doWork", Qt::QueuedConnection,
                              Q_ARG(QString, archivePath), Q_ARG(QString, m_dest));
}

void TexInstallDialog::onExtractProgress(qint64 done, qint64 total)
{
    if (total <= 0)
        return;
    if (m_exBar->maximum() != 100)
        m_exBar->setRange(0, 100);
    m_exBar->setValue(static_cast<int>(done * 100 / total));
}

void TexInstallDialog::onExtractDone(bool ok, const QString &msg)
{
    // The thread and worker self-delete via finished() -> deleteLater().
    m_worker = nullptr;
    m_extThread = nullptr;

    // Free the downloaded archive as soon as it is unpacked.
    delete m_tmp;
    m_tmp = nullptr;

    if (ok)
    {
        m_ok = true;
        m_exBar->setValue(100);
        setStatus(QStringLiteral("Installed."));
        // The pack is now indexable: switch Texture Replacement on and persist it so
        // the next run actually uses it (shared [video] texture_pack key).
        SettingsManager::instance().setTexPack(true);
        SettingsManager::instance().save();
        emit installed();
    }
    else if (!m_aborting)
    {
        fail(msg.isEmpty() ? QStringLiteral("Extraction failed.") : msg);
    }
    m_browse->setEnabled(true);
    m_download->setEnabled(true);
}

// ---------------------------------------------------------------------------
// Cancellation
// ---------------------------------------------------------------------------

void TexInstallDialog::abortDownload()
{
    if (m_reply)
    {
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    m_nam = nullptr;
    if (m_out)
    {
        m_out->close();
        m_out->deleteLater();
        m_out = nullptr;
    }
}

void TexInstallDialog::abortExtract()
{
    if (!m_worker && !m_extThread)
        return;
    m_aborting = true;
    if (m_worker)
        m_worker->requestCancel();
    if (m_extThread)
    {
        m_extThread->quit();
        m_extThread->wait(5000);
    }
    m_worker = nullptr;
    m_extThread = nullptr;
}

void TexInstallDialog::closeEvent(QCloseEvent *e)
{
    abortDownload();
    abortExtract();
    e->accept();
}
