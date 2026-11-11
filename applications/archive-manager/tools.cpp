// SPDX-License-Identifier: GPL-3.0-or-later
#include "tools.h"
#include "formats.h"

#include <command.h>

#include <QDir>
#include <QFileInfo>

QVariantList ArchiveTools::createFormats() const
{
    QVariantList out;
    for (const Formats::Format &f : Formats::all()) {
        if (!f.canCreate)
            continue;
        out.append(QVariantMap{
            {QStringLiteral("id"), f.id},
            {QStringLiteral("label"), QStringLiteral("%1 (.%2)").arg(f.label, f.extensions.first())},
            {QStringLiteral("extension"), f.extensions.first()},
            {QStringLiteral("encrypt"), f.canEncrypt},
            {QStringLiteral("cipher"), f.cipher},
            {QStringLiteral("store"), f.engine == Formats::Engine::SevenZip},
            {QStringLiteral("compressed"), f.id != QLatin1String("tar") && f.id != QLatin1String("cpio")},
            {QStringLiteral("available"), Formats::toolAvailable(f.engine)},
            {QStringLiteral("tool"), Formats::toolName(f.engine)},
        });
    }
    return out;
}

QVariantList ArchiveTools::tools() const
{
    QVariantList out;
    const struct { const char *name; const char *usedFor; } tools[] = {
        {"bsdtar", "tar, tar.gz, tar.bz2, tar.xz, tar.zst, cpio, rar"},
        {"7z", "zip, 7z, iso"},
    };
    for (const auto &tool : tools) {
        const QString name = QString::fromLatin1(tool.name);
        const bool available = Nexus::Command::available(name);
        QString version;
        if (available) {
            const Nexus::CommandResult r = name == QLatin1String("bsdtar")
                ? Nexus::Command::runSync(name, {QStringLiteral("--version")}, 3000)
                : Nexus::Command::runSync(name, {QStringLiteral("i")}, 3000);
            const QStringList lines = r.outText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            version = lines.value(0).trimmed();
        }
        out.append(QVariantMap{{QStringLiteral("name"), name}, {QStringLiteral("available"), available},
                               {QStringLiteral("version"), version}, {QStringLiteral("usedFor"), QString::fromLatin1(tool.usedFor)}});
    }
    return out;
}

QString ArchiveTools::formatForName(const QString &fileName) const
{
    const Formats::Format f = Formats::fromFileName(fileName);
    return f.canCreate ? f.id : QString();
}

QString ArchiveTools::baseName(const QString &fileName) const { return Formats::baseName(fileName); }

bool ArchiveTools::looksLikeArchive(const QString &path) const
{
    QString problem;
    return QFileInfo(path).isFile() && Formats::detect(path, &problem).valid();
}

QString ArchiveTools::suggestedName(const QStringList &inputs) const
{
    if (inputs.isEmpty())
        return tr("Archive");
    const QFileInfo first(inputs.first());
    if (inputs.size() == 1)
        return first.isDir() ? first.fileName() : first.completeBaseName().isEmpty() ? first.fileName() : first.completeBaseName();
    const QString folder = QFileInfo(first.absolutePath()).fileName();
    return folder.isEmpty() ? tr("Archive") : folder;
}

QString ArchiveTools::homePath() const { return QDir::homePath(); }

QString ArchiveTools::parentPath(const QString &path) const
{
    QDir dir(path);
    return dir.cdUp() ? dir.absolutePath() : QStringLiteral("/");
}

QString ArchiveTools::joinPath(const QString &folder, const QString &name) const
{
    return QDir::cleanPath(QDir(folder).filePath(name));
}

QString ArchiveTools::fileName(const QString &path) const { return QFileInfo(path).fileName(); }
bool ArchiveTools::isDirectory(const QString &path) const { return !path.isEmpty() && QFileInfo(path).isDir(); }
bool ArchiveTools::exists(const QString &path) const { return !path.isEmpty() && QFileInfo::exists(path); }
QString ArchiveTools::localPath(const QUrl &url) const { return url.isLocalFile() ? url.toLocalFile() : QString(); }

QVariantList ArchiveTools::listDirectory(const QString &path, bool showHidden) const
{
    QVariantList out;
    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot;
    if (showHidden)
        filters |= QDir::Hidden;
    const QFileInfoList entries = QDir(path).entryInfoList(filters, QDir::DirsFirst | QDir::Name | QDir::IgnoreCase | QDir::LocaleAware);
    for (const QFileInfo &info : entries) {
        out.append(QVariantMap{
            {QStringLiteral("name"), info.fileName()},
            {QStringLiteral("path"), info.absoluteFilePath()},
            {QStringLiteral("directory"), info.isDir()},
            {QStringLiteral("size"), info.isDir() ? qint64(0) : info.size()},
        });
    }
    return out;
}
