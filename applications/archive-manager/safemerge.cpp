// SPDX-License-Identifier: GPL-3.0-or-later
#include "safemerge.h"

#include <QCoreApplication>
#include <QFile>

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace SafeMerge {

namespace {

QString tr(const char *s) { return QCoreApplication::translate("SafeMerge", s); }
QString show(const QByteArray &path) { return QString::fromLocal8Bit(path); }

struct Context
{
    Conflict conflict;
    const std::atomic_bool &cancel;
    Report report;
};

bool lexists(const QByteArray &path, struct stat *st = nullptr)
{
    struct stat local;
    return ::lstat(path.constData(), st ? st : &local) == 0;
}

std::vector<QByteArray> listDirectory(const QByteArray &path)
{
    std::vector<QByteArray> names;
    DIR *dir = ::opendir(path.constData());
    if (!dir)
        return names;
    while (const dirent *entry = ::readdir(dir)) {
        const QByteArray name(entry->d_name);
        if (name != "." && name != "..")
            names.push_back(name);
    }
    ::closedir(dir);
    std::sort(names.begin(), names.end());
    return names;
}

// True if a relative link target, seen from a link that sits 'depth'
// directories below the extraction root, never leaves that root.
bool linkStaysInside(const QByteArray &target, int depth)
{
    if (target.isEmpty() || target.startsWith('/'))
        return false;
    const QList<QByteArray> components = target.split('/');
    for (const QByteArray &component : components) {
        if (component.isEmpty() || component == ".")
            continue;
        if (component == "..") {
            if (--depth < 0)
                return false;
        } else {
            ++depth;
        }
    }
    return true;
}

// "name.tar.gz" → "name (2).tar.gz", the first that does not exist.
QByteArray freeName(const QByteArray &directory, const QByteArray &name)
{
    qsizetype dot = name.indexOf('.', 1);
    const QByteArray stem = dot > 0 ? name.left(dot) : name;
    const QByteArray suffix = dot > 0 ? name.mid(dot) : QByteArray();
    for (int i = 2; i < 100000; ++i) {
        const QByteArray candidate = stem + " (" + QByteArray::number(i) + ")" + suffix;
        if (!lexists(directory + '/' + candidate))
            return candidate;
    }
    return {};
}

bool copyRegular(const QByteArray &from, const QByteArray &to, mode_t mode, QString *error)
{
    const int in = ::open(from.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (in < 0) {
        *error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    const int out = ::open(to.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode & 0777);
    if (out < 0) {
        *error = QString::fromLocal8Bit(std::strerror(errno));
        ::close(in);
        return false;
    }
    std::vector<char> buffer(1 << 16);
    bool ok = true;
    for (;;) {
        const ssize_t n = ::read(in, buffer.data(), buffer.size());
        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        ssize_t written = 0;
        while (written < n) {
            const ssize_t w = ::write(out, buffer.data() + written, size_t(n - written));
            if (w < 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
            written += w;
        }
        if (!ok)
            break;
    }
    if (!ok)
        *error = QString::fromLocal8Bit(std::strerror(errno));
    ::close(in);
    if (::close(out) != 0 && ok) {
        *error = QString::fromLocal8Bit(std::strerror(errno));
        ok = false;
    }
    if (!ok)
        ::unlink(to.constData());
    return ok;
}

// Decide the final path for 'name' in 'directory' according to the
// conflict policy. Returns an empty array when the entry is to be skipped.
QByteArray resolveTarget(Context &c, const QByteArray &directory, const QByteArray &name, const QString &relative)
{
    QByteArray target = directory + '/' + name;
    struct stat existing;
    if (!lexists(target, &existing))
        return target;
    switch (c.conflict) {
    case Conflict::Skip:
        ++c.report.skippedExisting;
        return {};
    case Conflict::Overwrite:
        if (S_ISDIR(existing.st_mode)) {
            c.report.refused << tr("%1: a folder with this name already exists in the destination").arg(relative);
            return {};
        }
        // unlink() removes the name itself — a symbolic link is removed, not followed.
        if (::unlink(target.constData()) != 0) {
            c.report.refused << tr("%1: the existing file cannot be replaced (%2)").arg(relative, QString::fromLocal8Bit(std::strerror(errno)));
            return {};
        }
        return target;
    case Conflict::Rename: {
        const QByteArray free = freeName(directory, name);
        if (free.isEmpty()) {
            c.report.refused << tr("%1: no free name left in the destination").arg(relative);
            return {};
        }
        ++c.report.renamed;
        return directory + '/' + free;
    }
    }
    return {};
}

bool moveInto(const QByteArray &source, const QByteArray &target, const struct stat &st, QString *error)
{
    if (::rename(source.constData(), target.constData()) == 0)
        return true;
    if (errno != EXDEV) {
        *error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    // A mount point inside the destination: copy across file systems.
    if (S_ISLNK(st.st_mode)) {
        QByteArray link(qsizetype(st.st_size) + 1, '\0');
        const ssize_t n = ::readlink(source.constData(), link.data(), size_t(link.size()));
        if (n < 0 || ::symlink(link.left(n).constData(), target.constData()) != 0) {
            *error = QString::fromLocal8Bit(std::strerror(errno));
            return false;
        }
    } else if (!copyRegular(source, target, st.st_mode, error)) {
        return false;
    }
    ::unlink(source.constData());
    return true;
}

void mergeDirectory(Context &c, const QByteArray &source, const QByteArray &target, const QString &relative, int depth)
{
    if (depth > 512) {
        c.report.refused << tr("%1: folders nested too deeply").arg(relative);
        return;
    }
    const std::vector<QByteArray> names = listDirectory(source);
    for (const QByteArray &name : names) {
        if (c.cancel.load()) {
            c.report.cancelled = true;
            return;
        }
        const QByteArray from = source + '/' + name;
        const QString rel = relative.isEmpty() ? show(name) : relative + QLatin1Char('/') + show(name);
        struct stat st;
        if (::lstat(from.constData(), &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            const QByteArray to = target + '/' + name;
            struct stat existing;
            bool created = false;
            if (!lexists(to, &existing)) {
                if (::mkdir(to.constData(), 0777) != 0) {
                    c.report.refused << tr("%1: the folder cannot be created (%2)").arg(rel, QString::fromLocal8Bit(std::strerror(errno)));
                    continue;
                }
                created = true;
                ++c.report.directories;
            } else if (!S_ISDIR(existing.st_mode)) {
                // Includes a symbolic link to a folder: never extract through it.
                c.report.refused << tr("%1: the destination already has something with this name that is not a real folder").arg(rel);
                continue;
            }
            if (depth == 0 && created)
                c.report.topLevel << show(name);
            // Make sure we can read what the tool created, whatever its mode.
            ::chmod(from.constData(), 0700);
            mergeDirectory(c, from, to, rel, depth + 1);
            if (c.report.cancelled || !c.report.error.isEmpty())
                return;
            if (created)
                ::chmod(to.constData(), (st.st_mode & 0777) | 0700);
            ::rmdir(from.constData());
        } else if (S_ISREG(st.st_mode)) {
            const mode_t mode = st.st_mode & 0777;
            QByteArray movable = from;
            if (st.st_nlink > 1) {
                // Never trust a hard link made by a tool: detach it.
                const QByteArray copy = source + "/.shadow-copy-" + QByteArray::number(c.report.files);
                QString error;
                if (!copyRegular(from, copy, mode, &error)) {
                    c.report.refused << tr("%1: cannot be copied (%2)").arg(rel, error);
                    continue;
                }
                ::unlink(from.constData());
                movable = copy;
            } else if ((st.st_mode & 07777) != mode) {
                ::chmod(from.constData(), mode);
            }
            const QByteArray to = resolveTarget(c, target, name, rel);
            if (to.isEmpty()) {
                ::unlink(movable.constData());
                continue;
            }
            QString error;
            if (!moveInto(movable, to, st, &error)) {
                c.report.refused << tr("%1: cannot be written (%2)").arg(rel, error);
                ::unlink(movable.constData());
                continue;
            }
            ++c.report.files;
            c.report.bytes += st.st_size;
            if (depth == 0)
                c.report.topLevel << show(to.mid(target.size() + 1));
        } else if (S_ISLNK(st.st_mode)) {
            QByteArray link(4096, '\0');
            const ssize_t n = ::readlink(from.constData(), link.data(), size_t(link.size()));
            const QByteArray linkTarget = n > 0 ? link.left(n) : QByteArray();
            if (!linkStaysInside(linkTarget, depth)) {
                c.report.refused << tr("%1: symbolic link to “%2” points outside the extraction folder — not created").arg(rel, show(linkTarget));
                ::unlink(from.constData());
                continue;
            }
            const QByteArray to = resolveTarget(c, target, name, rel);
            if (to.isEmpty()) {
                ::unlink(from.constData());
                continue;
            }
            QString error;
            if (!moveInto(from, to, st, &error)) {
                c.report.refused << tr("%1: cannot be written (%2)").arg(rel, error);
                ::unlink(from.constData());
                continue;
            }
            ++c.report.links;
            if (depth == 0)
                c.report.topLevel << show(to.mid(target.size() + 1));
        } else {
            const QString what = S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode) ? tr("device node")
                               : S_ISFIFO(st.st_mode) ? tr("named pipe") : tr("socket or other special file");
            c.report.refused << tr("%1: %2 — not created").arg(rel, what);
            ::unlink(from.constData());
        }
    }
}

} // namespace

Report merge(const QString &stageRoot, const QString &subPath, const QString &destination,
             Conflict conflict, const std::atomic_bool &cancel)
{
    Context c{conflict, cancel, {}};
    QByteArray source = QFile::encodeName(stageRoot);
    // Descend to the requested sub-folder one real directory at a time.
    const QStringList parts = subPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (part == QLatin1String(".") || part == QLatin1String("..")) {
            c.report.error = tr("Invalid folder inside the archive.");
            return c.report;
        }
        source += '/' + QFile::encodeName(part);
        struct stat st;
        if (::lstat(source.constData(), &st) != 0)
            return c.report;        // nothing was extracted there
        if (!S_ISDIR(st.st_mode)) {
            c.report.error = tr("“%1” inside the archive is not a folder.").arg(subPath);
            return c.report;
        }
    }
    const QByteArray target = QFile::encodeName(destination);
    struct stat st;
    if (::stat(target.constData(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        c.report.error = tr("The destination “%1” is not a folder.").arg(destination);
        return c.report;
    }
    mergeDirectory(c, source, target, QString(), 0);
    return c.report;
}

qint64 treeSize(const QString &path)
{
    qint64 total = 0;
    std::vector<QByteArray> pending{QFile::encodeName(path)};
    while (!pending.empty()) {
        const QByteArray directory = pending.back();
        pending.pop_back();
        for (const QByteArray &name : listDirectory(directory)) {
            const QByteArray full = directory + '/' + name;
            struct stat st;
            if (::lstat(full.constData(), &st) != 0)
                continue;
            if (S_ISDIR(st.st_mode))
                pending.push_back(full);
            else if (S_ISREG(st.st_mode))
                total += st.st_size;
        }
    }
    return total;
}

} // namespace SafeMerge
