#include "tex_pack.h"

#include "app_paths.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace texpack
{
const char *const kUrl = "https://pixeldrain.com/api/file/5UzM4yox";
const char *const kFileName = "Texture-4k.7z";
const char *const kSha256 = "9d2d225d281545b08b3a8f4f02f2ebf79c8a6d61d62b84fea7db0fb5b569ed6f";

QString dir()
{
    return apppaths::userRoot() + QStringLiteral("/data/Textures");
}

QString pickUnpacker()
{
    static const char *kTools[] = {"7zz", "7z", "unrar", "bsdtar", "tar"};
    for (const char *t : kTools)
        if (!QStandardPaths::findExecutable(QLatin1String(t)).isEmpty())
            return QLatin1String(t);
    return QString();
}

QString sha256File(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f))
        return QString();
    return QString::fromLatin1(h.result().toHex());
}

quint64 countReplacements()
{
    const QString d = dir();
    if (!QDir(d).exists())
        return 0;
    quint64 n = 0;
    QDirIterator it(d, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString e = QFileInfo(it.next()).suffix().toLower();
        if (e == QLatin1String("png") || e == QLatin1String("dds"))
            ++n;
    }
    return n;
}
} // namespace texpack
