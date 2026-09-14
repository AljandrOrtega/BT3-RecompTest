#pragma once
// [texreplace] Install dialog: obtain the pinned texture-pack archive either from a
// local file (Browse) or by downloading it (pixeldrain), then extract it into
// data/Textures. A single dialog drives both a download bar (with ETA) and an
// extraction bar; both paths verify the pinned sha256 before extracting.

#include <QDialog>
#include <QProcess>

class QLabel;
class QProgressBar;
class QPushButton;
class QNetworkAccessManager;
class QNetworkReply;
class QFile;
class QElapsedTimer;
class QTemporaryDir;

class TexInstallDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TexInstallDialog(QWidget *parent = nullptr);
    ~TexInstallDialog() override;

signals:
    void installed();   // emitted once an extraction finished successfully

protected:
    void closeEvent(QCloseEvent *e) override;

private slots:
    void onBrowse();
    void onDownload();
    void onDownloadReadyRead();
    void onDownloadProgress(qint64 received, qint64 total);
    void onDownloadFinished();
    void onExtractOutput();
    void onExtractFinished(int exitCode, QProcess::ExitStatus status);

private:
    void setStatus(const QString &text);
    void beginExtract(const QString &archivePath);
    void fail(const QString &text);
    void abortDownload();
    void abortExtract();

    QPushButton *m_browse = nullptr;
    QPushButton *m_download = nullptr;
    QPushButton *m_close = nullptr;
    QLabel *m_status = nullptr;
    QProgressBar *m_dlBar = nullptr;
    QLabel *m_dlInfo = nullptr;      // MB/MB + speed + ETA
    QProgressBar *m_exBar = nullptr;

    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply *m_reply = nullptr;
    QFile *m_out = nullptr;
    QElapsedTimer *m_dlClock = nullptr;
    QTemporaryDir *m_tmp = nullptr;
    QString m_tmpPath;

    QProcess *m_proc = nullptr;
    QString m_dest;
    bool m_ok = false;   // set once extraction finished cleanly
};
