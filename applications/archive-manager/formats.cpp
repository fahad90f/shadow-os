// SPDX-License-Identifier: GPL-3.0-or-later
#include "formats.h"

#include <command.h>

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>

namespace Formats {

namespace {
QString tr(const char *s) { return QCoreApplication::translate("Formats", s); }

Format make(const char *id, const QString &label, Engine engine, bool create, bool modify, bool encrypt,
            const QString &cipher, const QStringList &extensions)
{
    Format f;
    f.id = QString::fromLatin1(id);
    f.label = label;
    f.engine = engine;
    f.canCreate = create;
    f.canModify = modify;
    f.canEncrypt = encrypt;
    f.cipher = cipher;
    f.extensions = extensions;
    return f;
}
} // namespace

const QList<Format> &all()
{
    static const QList<Format> formats = {
        make("zip", tr("ZIP"), Engine::SevenZip, true, true, true,
             tr("AES-256 (WinZip AE-2). File contents are encrypted; file names, sizes and dates stay readable."),
             {QStringLiteral("zip")}),
        make("7z", tr("7-Zip"), Engine::SevenZip, true, true, true,
             tr("AES-256 with a key derived from the password by SHA-256 (7zAES). File names can be encrypted too."),
             {QStringLiteral("7z")}),
        make("tar.gz", tr("Tar, gzip compressed"), Engine::BsdTar, true, true, false, QString(),
             {QStringLiteral("tar.gz"), QStringLiteral("tgz")}),
        make("tar.xz", tr("Tar, xz compressed"), Engine::BsdTar, true, true, false, QString(),
             {QStringLiteral("tar.xz"), QStringLiteral("txz")}),
        make("tar.zst", tr("Tar, Zstandard compressed"), Engine::BsdTar, true, true, false, QString(),
             {QStringLiteral("tar.zst"), QStringLiteral("tzst")}),
        make("tar.bz2", tr("Tar, bzip2 compressed"), Engine::BsdTar, true, true, false, QString(),
             {QStringLiteral("tar.bz2"), QStringLiteral("tbz2"), QStringLiteral("tbz")}),
        make("tar", tr("Tar, uncompressed"), Engine::BsdTar, true, true, false, QString(), {QStringLiteral("tar")}),
        make("cpio", tr("CPIO"), Engine::BsdTar, true, false, false, QString(), {QStringLiteral("cpio")}),
        make("iso", tr("ISO 9660 disc image"), Engine::SevenZip, false, false, false, QString(), {QStringLiteral("iso")}),
        make("rar", tr("RAR"), Engine::BsdTar, false, false, false, QString(), {QStringLiteral("rar")}),
    };
    return formats;
}

Format byId(const QString &id)
{
    for (const Format &f : all()) {
        if (f.id == id)
            return f;
    }
    return {};
}

Format fromFileName(const QString &name)
{
    const QString lower = name.toLower();
    Format best;
    qsizetype bestLength = 0;
    for (const Format &f : all()) {
        for (const QString &extension : f.extensions) {
            if (lower.endsWith(QLatin1Char('.') + extension) && extension.size() > bestLength) {
                best = f;
                bestLength = extension.size();
            }
        }
    }
    return best;
}

QString baseName(const QString &fileName)
{
    const QString lower = fileName.toLower();
    qsizetype cut = 0;
    for (const Format &f : all()) {
        for (const QString &extension : f.extensions) {
            if (lower.endsWith(QLatin1Char('.') + extension))
                cut = qMax(cut, extension.size() + 1);
        }
    }
    if (cut > 0 && cut < fileName.size())
        return fileName.left(fileName.size() - cut);
    const qsizetype dot = fileName.lastIndexOf(QLatin1Char('.'));
    return dot > 0 ? fileName.left(dot) : fileName;
}

Format detect(const QString &path, QString *problem)
{
    QFile file(path);
    const QFileInfo info(path);
    if (!info.exists()) {
        *problem = tr("“%1” does not exist.").arg(path);
        return {};
    }
    if (!info.isFile()) {
        *problem = tr("“%1” is not a regular file.").arg(path);
        return {};
    }
    if (!file.open(QIODevice::ReadOnly)) {
        *problem = tr("Cannot read “%1”: %2").arg(path, file.errorString());
        return {};
    }
    const QByteArray head = file.read(512);
    QByteArray isoMagic;
    if (file.seek(32769))
        isoMagic = file.read(5);

    auto startsWith = [&head](const char *magic, int length) { return head.startsWith(QByteArrayView(magic, length)); };
    const QString lower = info.fileName().toLower();
    auto compressedTar = [&](const char *id, const QStringList &tarNames, const QString &what) -> Format {
        for (const QString &suffix : tarNames) {
            if (lower.endsWith(suffix))
                return byId(QString::fromLatin1(id));
        }
        *problem = tr("“%1” is a single %2-compressed file, not an archive with several entries. "
                      "This application does not unpack plain compressed files.").arg(info.fileName(), what);
        return {};
    };

    if (startsWith("PK\x03\x04", 4) || startsWith("PK\x05\x06", 4) || startsWith("PK\x07\x08", 4))
        return byId(QStringLiteral("zip"));
    if (startsWith("7z\xBC\xAF\x27\x1C", 6))
        return byId(QStringLiteral("7z"));
    if (startsWith("Rar!\x1A\x07", 6))
        return byId(QStringLiteral("rar"));
    if (startsWith("\x1F\x8B", 2))
        return compressedTar("tar.gz", {QStringLiteral(".tar.gz"), QStringLiteral(".tgz")}, QStringLiteral("gzip"));
    if (startsWith("BZh", 3))
        return compressedTar("tar.bz2", {QStringLiteral(".tar.bz2"), QStringLiteral(".tbz2"), QStringLiteral(".tbz")},
                             QStringLiteral("bzip2"));
    if (startsWith("\xFD" "7zXZ\x00", 6))
        return compressedTar("tar.xz", {QStringLiteral(".tar.xz"), QStringLiteral(".txz")}, QStringLiteral("xz"));
    if (startsWith("\x28\xB5\x2F\xFD", 4))
        return compressedTar("tar.zst", {QStringLiteral(".tar.zst"), QStringLiteral(".tzst")}, QStringLiteral("Zstandard"));
    if (head.size() >= 262 && head.mid(257, 5) == "ustar")
        return byId(QStringLiteral("tar"));
    if (startsWith("070701", 6) || startsWith("070702", 6) || startsWith("070707", 6)
        || startsWith("\xC7\x71", 2) || startsWith("\x71\xC7", 2))
        return byId(QStringLiteral("cpio"));
    if (isoMagic == "CD001")
        return byId(QStringLiteral("iso"));
    // Old (pre-POSIX) tar has no magic; trust the name.
    if (lower.endsWith(QLatin1String(".tar")))
        return byId(QStringLiteral("tar"));
    *problem = tr("“%1” is not in a format this application can read (ZIP, 7-Zip, tar and compressed tar, CPIO, ISO 9660, RAR).")
                   .arg(info.fileName());
    return {};
}

QString toolName(Engine engine)
{
    return engine == Engine::SevenZip ? QStringLiteral("7z") : engine == Engine::BsdTar ? QStringLiteral("bsdtar") : QString();
}

bool toolAvailable(Engine engine)
{
    return engine != Engine::None && Nexus::Command::available(toolName(engine));
}

QJsonArray describe()
{
    QJsonArray out;
    for (const Format &f : all()) {
        out.append(QJsonObject{
            {QStringLiteral("id"), f.id},
            {QStringLiteral("label"), f.label},
            {QStringLiteral("tool"), toolName(f.engine)},
            {QStringLiteral("toolAvailable"), toolAvailable(f.engine)},
            {QStringLiteral("create"), f.canCreate},
            {QStringLiteral("modify"), f.canModify},
            {QStringLiteral("encrypt"), f.canEncrypt},
            {QStringLiteral("cipher"), f.cipher},
            {QStringLiteral("extensions"), QJsonArray::fromStringList(f.extensions)},
        });
    }
    return out;
}

} // namespace Formats
