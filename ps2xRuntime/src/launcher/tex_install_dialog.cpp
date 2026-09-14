#include "tex_install_dialog.h"

#include "app_paths.h"
#include "tex_pack.h"

#include <QCloseEvent>
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
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QTemporaryDir>
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
        fail(QStringLiteral("Download failed: %1").arg(errStr));
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
    const QString tool = texpack::pickUnpacker();
    if (tool.isEmpty())
    {
        fail(QStringLiteral("No unpacker found. Install 7-Zip, unrar or libarchive (bsdtar)."));
        return;
    }

    m_dest = texpack::dir();
    QDir().mkpath(m_dest);

    m_exBar->setVisible(true);
    if (tool == QLatin1String("bsdtar") || tool == QLatin1String("tar"))
        m_exBar->setRange(0, 0);   // no percentage from tar
    else
        m_exBar->setRange(0, 100);
    m_exBar->setValue(0);
    setStatus(QStringLiteral("Extracting to %1…").arg(m_dest));

    QStringList args;
    if (tool == QLatin1String("7zz") || tool == QLatin1String("7z"))
        args = {QStringLiteral("x"), QStringLiteral("-y"), QStringLiteral("-bsp1"),
                QStringLiteral("-o") + m_dest, archivePath};
    else if (tool == QLatin1String("unrar"))
        args = {QStringLiteral("x"), QStringLiteral("-y"), QStringLiteral("-bsp1"),
                archivePath, m_dest + QLatin1Char('/')};
    else
        args = {QStringLiteral("-xf"), archivePath, QStringLiteral("-C"), m_dest};

    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_proc, &QProcess::readyReadStandardOutput, this, &TexInstallDialog::onExtractOutput);
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &TexInstallDialog::onExtractFinished);
    m_proc->start(tool, args);
}

void TexInstallDialog::onExtractOutput()
{
    if (!m_proc || m_exBar->maximum() != 100)
        return;
    const QString out = QString::fromLatin1(m_proc->readAllStandardOutput());
    static const QRegularExpression re(QStringLiteral("(\\d{1,3})%"));
    auto it = re.globalMatch(out);
    while (it.hasNext())
    {
        const int pct = it.next().captured(1).toInt();
        m_exBar->setValue(qBound(0, pct, 100));
    }
}

void TexInstallDialog::onExtractFinished(int exitCode, QProcess::ExitStatus status)
{
    if (m_proc)
    {
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    // Free the downloaded archive as soon as it is unpacked.
    delete m_tmp;
    m_tmp = nullptr;

    if (exitCode == 0 && status == QProcess::NormalExit)
    {
        m_ok = true;
        m_exBar->setValue(100);
        setStatus(QStringLiteral("Installed."));
        emit installed();
    }
    else
    {
        fail(QStringLiteral("Extraction failed (exit %1).").arg(exitCode));
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
    if (m_proc)
    {
        m_proc->kill();
        m_proc->waitForFinished(3000);
        m_proc->deleteLater();
        m_proc = nullptr;
    }
}

void TexInstallDialog::closeEvent(QCloseEvent *e)
{
    abortDownload();
    abortExtract();
    e->accept();
}
