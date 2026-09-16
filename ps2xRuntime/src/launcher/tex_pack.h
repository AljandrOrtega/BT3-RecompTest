#pragma once
// [texreplace] Shared texture-pack constants/helpers for the launcher (Misc tab +
// install dialog). The pack lives in <deploy>/data/Textures and is delivered as
// a single archive whose sha256 is pinned below.

#include <QString>

namespace texpack
{
    // Direct download (pixeldrain API file endpoint).
    extern const char *const kUrl;        // https://pixeldrain.com/api/file/5UzM4yox
    // Browser page for the same file. pixeldrain only allows direct API
    // downloads for paid accounts, so on 403 we send the user here instead.
    extern const char *const kPageUrl;    // https://pixeldrain.com/u/5UzM4yox
    extern const char *const kFileName;   // Texture-4k.7z
    // Only this exact archive is accepted (same as pixeldrain's hash_sha256).
    extern const char *const kSha256;

    // <deploy>/data/Textures
    QString dir();

    // Lowercase hex sha256 of a file, or empty on error.
    QString sha256File(const QString &path);

    // Count replacement files (.png/.dds) under dir(), recursively.
    quint64 countReplacements();
}
