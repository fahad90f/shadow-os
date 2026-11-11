// SPDX-License-Identifier: GPL-3.0-or-later
#include "archiver.h"

#include "interaction.h"
#include "operations.h"
#include "util.h"

#include <command.h>

#include <KFileUtils>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <QtConcurrentRun>

#include <memory>

using namespace Qt::StringLiterals;

namespace {

const QStringList &archiveSuffixes()
{
    // Longest first, so ".tar.gz" wins over ".gz"-style partial matches.
    static const QStringList suffixes = {
        u".tar.gz"_s, u".tar.xz"_s, u".tar.zst"_s, u".tar.bz2"_s, u".tar.lz4"_s, u".tar.lzma"_s, u".tar.lz"_s,
        u".tar.Z"_s, u".tgz"_s, u".txz"_s, u".tzst"_s, u".tbz2"_s, u".tbz"_s, u".tar"_s, u".zip"_s, u".7z"_s,
        u".rar"_s, u".cpio"_s, u".iso"_s, u".jar"_s, u".cab"_s, u".lha"_s, u".lzh"_s, u".xar"_s,
    };
    return suffixes;
}

const QSet<QString> &archiveMimeTypes()
{
    static const QSet<QString> types = {
        u"application/x-tar"_s,
        u"application/x-compressed-tar"_s,
        u"application/x-bzip-compressed-tar"_s,
        u"application/x-bzip2-compressed-tar"_s,
        u"application/x-xz-compressed-tar"_s,
        u"application/x-zstd-compressed-tar"_s,
        u"application/x-lzma-compressed-tar"_s,
        u"application/x-lz4-compressed-tar"_s,
        u"application/x-lzip-compressed-tar"_s,
        u"application/x-tarz"_s,
        u"application/zip"_s,
        u"application/x-7z-compressed"_s,
        u"application/vnd.rar"_s,
        u"application/x-rar"_s,
        u"application/x-cpio"_s,
        u"application/x-cd-image"_s,
        u"application/x-iso9660-image"_s,
        u"application/java-archive"_s,
        u"application/x-java-archive"_s,
        u"application/vnd.ms-cab-compressed"_s,
        u"application/x-lha"_s,
        u"application/x-xar"_s,
    };
    return types;
}

// The last lines of bsdtar's diagnostics — the actual reason is at the end.
QString tail(const QString &text)
{
    QStringList lines = text.split(u'\n', Qt::SkipEmptyParts);
    if (lines.size() > 3)
        lines = lines.last(3);
    return lines.join(u'\n');
}

// File arguments that bsdtar would read as an option ("-C…") or as an
// archive to merge ("@…") are passed as "./name".
QString safeName(const QString &name)
{
    return name.startsWith(u'-') || name.startsWith(u'@') ? u"./"_s + name : name;
}

void removeTreeLater(const QString &path)
{
    (void)QtConcurrent::run([path] { QDir(path).removeRecursively(); });
}

} // namespace

Archiver::Archiver(QObject *parent)
    : QObject(parent)
{
}

Archiver *Archiver::instance()
{
    static Archiver *self = new Archiver(qApp);
    return self;
}

Archiver *Archiver::create(QQmlEngine *, QJSEngine *)
{
    Archiver *self = instance();
    QQmlEngine::setObjectOwnership(self, QQmlEngine::CppOwnership);
    return self;
}

bool Archiver::isArchive(const QString &mimeType, const QString &fileName)
{
    if (archiveMimeTypes().contains(mimeType))
        return true;
    const QStringList &suffixes = archiveSuffixes();
    return std::any_of(suffixes.cbegin(), suffixes.cend(), [&fileName](const QString &s) {
        return fileName.size() > s.size() && fileName.endsWith(s, Qt::CaseInsensitive);
    });
}

bool Archiver::available() const
{
    static const bool found = Shadow::Command::available(u"bsdtar"_s);
    return found;
}

bool Archiver::managerAvailable() const
{
    static const bool found = Shadow::Command::available(u"shadow-archive-manager"_s);
    return found;
}

QVariantList Archiver::formats() const
{
    return {
        QVariantMap{{u"suffix"_s, u".tar.gz"_s}, {u"label"_s, tr("tar.gz — gzip, opens everywhere")}},
        QVariantMap{{u"suffix"_s, u".tar.xz"_s}, {u"label"_s, tr("tar.xz — smallest, slower")}},
        QVariantMap{{u"suffix"_s, u".tar.zst"_s}, {u"label"_s, tr("tar.zst — Zstandard, fast")}},
        QVariantMap{{u"suffix"_s, u".zip"_s}, {u"label"_s, tr("zip — for Windows and macOS")}},
    };
}

QString Archiver::baseName(const QString &fileName) const
{
    for (const QString &suffix : archiveSuffixes()) {
        if (fileName.size() > suffix.size() && fileName.endsWith(suffix, Qt::CaseInsensitive))
            return fileName.left(fileName.size() - suffix.size());
    }
    const QString base = QFileInfo(fileName).completeBaseName();
    return base.isEmpty() ? fileName : base;
}

QString Archiver::suggestedName(const QList<QUrl> &urls) const
{
    if (urls.isEmpty())
        return {};
    if (urls.size() == 1) {
        const QString name = Util::displayName(urls.first());
        const QFileInfo info(urls.first().toLocalFile());
        return info.isDir() ? name : (info.completeBaseName().isEmpty() ? name : info.completeBaseName());
    }
    const QString parent = Util::displayName(Util::parentUrl(urls.first()));
    return parent == "/"_L1 ? tr("Archive") : parent;
}

void Archiver::runTar(const QString &title, const QString &failureTitle, const QStringList &arguments,
                      std::function<QString(bool ok)> finish)
{
    OperationsModel *jobs = FileOperations::instance()->jobs();
    auto *context = new QObject(this);          // owns the QProcess Shadow::Command creates
    auto cancelled = std::make_shared<bool>(false);
    const int id = jobs->addExternal(title, [context, cancelled] {
        *cancelled = true;
        const QList<QProcess *> processes = context->findChildren<QProcess *>();
        for (QProcess *process : processes)
            process->kill();
    });
    Shadow::Command::run(context, u"bsdtar"_s, arguments,
                        [jobs, context, cancelled, id, finish, failureTitle](const Shadow::CommandResult &result) {
        context->deleteLater();
        QString error;
        if (!result.ok() && !*cancelled) {
            error = tail(result.errText());
            if (error.isEmpty()) {
                error = result.exitCode == -1 ? tr("bsdtar could not be started.")
                                              : tr("bsdtar exited with status %1.").arg(result.exitCode);
            }
        }
        // Clean up or move the result into place; may report its own error.
        const QString followUp = finish(result.ok());
        if (error.isEmpty())
            error = followUp;
        jobs->finishExternal(id, error.isEmpty(), error);
        if (!error.isEmpty())
            Interaction::instance()->reportError(failureTitle, error);
    }, 0);
}

void Archiver::extract(const QUrl &archive, const QUrl &destDir)
{
    auto *ui = Interaction::instance();
    const QString archiveName = Util::displayName(archive);
    if (!available()) {
        ui->reportError(tr("Could not extract “%1”").arg(archiveName),
                        tr("bsdtar is not installed (package “libarchive-tools”)."));
        return;
    }
    if (!archive.isLocalFile() || !destDir.isLocalFile()) {
        ui->reportError(tr("Could not extract “%1”").arg(archiveName),
                        tr("Archives can only be extracted from and to local folders. Copy the archive to this computer first."));
        return;
    }
    const QString destPath = destDir.toLocalFile();
    QTemporaryDir staging(QDir(destPath).filePath(u".extract-XXXXXX"_s));
    staging.setAutoRemove(false);
    if (!staging.isValid()) {
        ui->reportError(tr("Could not extract “%1”").arg(archiveName),
                        tr("“%1” is not writable: %2").arg(destPath, staging.errorString()));
        return;
    }
    const QString stagingPath = staging.path();
    const QString wantedFolder = baseName(archiveName);

    const QStringList arguments{u"-x"_s, u"-o"_s, u"-f"_s, archive.toLocalFile(), u"-C"_s, stagingPath};
    runTar(tr("Extracting “%1”").arg(archiveName), tr("Could not extract “%1”").arg(archiveName), arguments,
           [stagingPath, destPath, destDir, wantedFolder, archiveName](bool ok) -> QString {
        if (!ok) {
            removeTreeLater(stagingPath);
            return {};
        }
        QDir stagingDir(stagingPath);
        const QStringList entries = stagingDir.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        if (entries.isEmpty()) {
            stagingDir.removeRecursively();
            return tr("“%1” contains no files.").arg(archiveName);
        }
        const QUrl base = QUrl::fromLocalFile(destPath + u'/');
        QString finalName;
        bool moved = false;
        if (entries.size() == 1) {
            // A single top-level entry is moved out as it is.
            finalName = entries.first();
            if (QFileInfo::exists(destPath + u'/' + finalName) || QFileInfo(destPath + u'/' + finalName).isSymLink())
                finalName = KFileUtils::suggestName(base, finalName);
            moved = QDir().rename(stagingPath + u'/' + entries.first(), destPath + u'/' + finalName);
            if (moved)
                QDir().rmdir(stagingPath);
        } else {
            finalName = wantedFolder;
            if (QFileInfo::exists(destPath + u'/' + finalName))
                finalName = KFileUtils::suggestName(base, finalName);
            moved = QDir().rename(stagingPath, destPath + u'/' + finalName);
        }
        if (!moved) {
            removeTreeLater(stagingPath);
            return tr("The extracted files could not be moved into “%1”.").arg(destPath);
        }
        Interaction::instance()->reportNotice(tr("Extracted “%1” to “%2”").arg(archiveName, finalName), u"success"_s);
        Q_EMIT FileOperations::instance()->itemsCreated(destDir, {QUrl::fromLocalFile(destPath + u'/' + finalName)});
        return {};
    });
}

void Archiver::compress(const QList<QUrl> &urls, const QUrl &destDir, const QString &name, const QString &suffix)
{
    auto *ui = Interaction::instance();
    const QString fileName = name.trimmed() + suffix;
    const QString failed = tr("Could not create “%1”").arg(fileName);
    if (!available()) {
        ui->reportError(failed, tr("bsdtar is not installed (package “libarchive-tools”)."));
        return;
    }
    const bool allLocal = destDir.isLocalFile()
        && std::all_of(urls.cbegin(), urls.cend(), [](const QUrl &u) { return u.isLocalFile(); });
    if (urls.isEmpty() || !allLocal) {
        ui->reportError(failed, tr("Only local files and folders can be compressed."));
        return;
    }
    QStringList format;
    if (suffix == ".tar.gz"_L1)
        format = {u"-z"_s};
    else if (suffix == ".tar.xz"_L1)
        format = {u"-J"_s};
    else if (suffix == ".tar.zst"_L1)
        format = {u"--zstd"_s};
    else if (suffix == ".zip"_L1)
        format = {u"--format"_s, u"zip"_s};
    else {
        ui->reportError(failed, tr("Unknown archive format “%1”.").arg(suffix));
        return;
    }
    const QString error = FileOperations::instance()->validateName(fileName);
    if (!error.isEmpty()) {
        ui->reportError(failed, error);
        return;
    }
    const QString destPath = destDir.toLocalFile();
    const QString target = destPath + u'/' + fileName;
    if (QFileInfo::exists(target)) {
        ui->reportError(failed, tr("A file with this name already exists in “%1”.").arg(destPath));
        return;
    }
    // Written under a temporary name and renamed when complete, so a failed or
    // cancelled run never leaves a truncated archive under the final name.
    const QString partial = destPath + u"/."_s + fileName + u".part"_s;
    QFile::remove(partial);

    QStringList arguments{u"-c"_s, u"-f"_s, partial};
    arguments += format;
    arguments << u"--"_s;
    QString currentDir;
    for (const QUrl &url : urls) {
        const QFileInfo info(url.toLocalFile());
        const QString dir = info.absolutePath();
        if (dir != currentDir) {
            arguments << u"-C"_s << dir;
            currentDir = dir;
        }
        arguments << safeName(info.fileName());
    }

    const QString what = urls.size() == 1 ? tr("“%1”").arg(Util::displayName(urls.first()))
                                          : tr("%1 items").arg(urls.size());
    runTar(tr("Compressing %1 to “%2”").arg(what, fileName), failed, arguments,
           [partial, target, destDir, fileName](bool ok) -> QString {
        if (!ok) {
            QFile::remove(partial);
            return {};
        }
        if (!QFile::rename(partial, target)) {
            QFile::remove(partial);
            return tr("The archive could not be saved as “%1”.").arg(target);
        }
        Interaction::instance()->reportNotice(tr("Created “%1”").arg(fileName), u"success"_s);
        Q_EMIT FileOperations::instance()->itemsCreated(destDir, {QUrl::fromLocalFile(target)});
        return {};
    });
}

void Archiver::openInManager(const QUrl &archive)
{
    if (!archive.isLocalFile())
        return;
    if (!QProcess::startDetached(u"shadow-archive-manager"_s, {archive.toLocalFile()})) {
        Interaction::instance()->reportError(tr("Could not start the Archive Manager"),
                                             tr("shadow-archive-manager failed to start."));
    }
}
