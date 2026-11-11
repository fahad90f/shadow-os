// SPDX-License-Identifier: GPL-3.0-or-later
#include "searchmodel.h"

#include "util.h"

#include <KIO/ListJob>
#include <KIO/UDSEntry>

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QIcon>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QThreadPool>
#include <QtConcurrentRun>

#include <deque>
#include <filesystem>
#include <sys/stat.h>

using namespace Qt::StringLiterals;

namespace {

// Matches a file name against the query (substring, or wildcard pattern).
class Matcher
{
public:
    explicit Matcher(const QString &query)
        : m_text(query)
    {
        if (query.contains(u'*') || query.contains(u'?')) {
            m_wildcard = true;
            m_re = QRegularExpression(QRegularExpression::wildcardToRegularExpression(query),
                                      QRegularExpression::CaseInsensitiveOption);
        }
    }
    bool matches(const QString &name) const
    {
        return m_wildcard ? m_re.match(name).hasMatch() : name.contains(m_text, Qt::CaseInsensitive);
    }

private:
    QString m_text;
    bool m_wildcard = false;
    QRegularExpression m_re;
};

// Kernel pseudo file systems: huge, synthetic, and never what a file search is after.
bool skipDirectory(const std::string &path)
{
    return path == "/proc" || path == "/sys" || path == "/dev";
}

QString iconFor(const QString &name, bool dir)
{
    static QHash<QString, QString> cache;
    const QMimeType type = dir ? QMimeDatabase().mimeTypeForName(u"inode/directory"_s)
                               : QMimeDatabase().mimeTypeForFile(name, QMimeDatabase::MatchExtension);
    const auto it = cache.constFind(type.name());
    if (it != cache.constEnd())
        return *it;
    QString icon = type.iconName();
    if (!QIcon::hasThemeIcon(icon)) {
        icon = type.genericIconName();
        if (!QIcon::hasThemeIcon(icon))
            icon = dir ? u"folder"_s : u"unknown"_s;
    }
    cache.insert(type.name(), icon);
    return icon;
}

} // namespace

SearchModel::SearchModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

SearchModel::~SearchModel()
{
    stopWorkers();
}

int SearchModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_hits.size());
}

QHash<int, QByteArray> SearchModel::roleNames() const
{
    return {
        {NameRole, "name"}, {UrlRole, "url"}, {IsDirRole, "isDir"}, {FolderRole, "folder"},
        {IconNameRole, "iconName"}, {SizeRole, "size"}, {ModifiedTextRole, "modifiedText"},
    };
}

QVariant SearchModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_hits.size())
        return {};
    const Hit &hit = m_hits.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case NameRole: return hit.name;
    case UrlRole: return hit.url;
    case IsDirRole: return hit.dir;
    case FolderRole: return hit.folder;
    case IconNameRole: return iconFor(hit.name, hit.dir);
    case SizeRole: return hit.dir ? -1.0 : double(hit.size);
    case ModifiedTextRole:
        return hit.mtime > 0 ? Util::formatDateTime(QDateTime::fromSecsSinceEpoch(hit.mtime)) : QString();
    }
    return {};
}

QVariantMap SearchModel::get(int row) const
{
    if (row < 0 || row >= m_hits.size())
        return {};
    const Hit &hit = m_hits.at(row);
    return {{u"name"_s, hit.name}, {u"url"_s, hit.url}, {u"isDir"_s, hit.dir}, {u"folder"_s, hit.folder}};
}

void SearchModel::stopWorkers()
{
    ++m_generation;         // results of earlier searches are ignored from now on
    if (m_cancel)
        m_cancel->store(true);
    m_cancel.reset();
    if (m_job)
        m_job->kill(KJob::Quietly);
    m_job.clear();
}

void SearchModel::clear()
{
    stopWorkers();
    beginResetModel();
    m_hits.clear();
    endResetModel();
    m_scanned = 0;
    m_truncated = false;
    m_error.clear();
    m_query.clear();
    m_running = false;
    Q_EMIT countChanged();
    Q_EMIT scannedChanged();
    Q_EMIT queryChanged();
    Q_EMIT runningChanged();
}

void SearchModel::cancel()
{
    if (!m_running)
        return;
    stopWorkers();
    m_running = false;
    Q_EMIT runningChanged();
}

void SearchModel::start(const QUrl &root, const QString &query, bool includeHidden)
{
    clear();
    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty() || !root.isValid())
        return;
    m_root = root;
    m_query = trimmed;
    m_running = true;
    Q_EMIT queryChanged();
    Q_EMIT runningChanged();
    if (root.isLocalFile())
        startLocal(root.toLocalFile(), trimmed, includeHidden);
    else
        startRemote(root, trimmed, includeHidden);
}

void SearchModel::append(const QList<Hit> &hits, int scanned)
{
    if (!hits.isEmpty()) {
        const int room = MaxResults - int(m_hits.size());
        const QList<Hit> accepted = hits.size() > room ? hits.first(qMax(0, room)) : hits;
        if (!accepted.isEmpty()) {
            beginInsertRows({}, int(m_hits.size()), int(m_hits.size() + accepted.size()) - 1);
            m_hits += accepted;
            endInsertRows();
            Q_EMIT countChanged();
        }
    }
    if (scanned != m_scanned) {
        m_scanned = scanned;
        Q_EMIT scannedChanged();
    }
}

void SearchModel::finish(bool truncated, const QString &error)
{
    m_truncated = truncated;
    m_error = error;
    m_running = false;
    m_cancel.reset();
    m_job.clear();
    Q_EMIT runningChanged();
}

void SearchModel::startLocal(const QString &rootPath, const QString &query, bool includeHidden)
{
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_cancel = cancel;
    const int generation = m_generation;
    const QPointer<SearchModel> self(this);

    (void)QtConcurrent::run(QThreadPool::globalInstance(), [self, generation, cancel, rootPath, query, includeHidden] {
        namespace fs = std::filesystem;
        const Matcher matcher(query);
        const QByteArray rootBytes = QFile::encodeName(rootPath);
        const std::string rootString(rootBytes.constData(), size_t(rootBytes.size()));

        QList<Hit> batch;
        int scanned = 0;
        int found = 0;
        bool truncated = false;
        QElapsedTimer sinceFlush;
        sinceFlush.start();

        const auto flush = [&] {
            const QList<Hit> out = batch;
            batch.clear();
            sinceFlush.restart();
            QMetaObject::invokeMethod(qApp, [self, generation, out, scanned] {
                if (self && self->m_generation == generation)
                    self->append(out, scanned);
            }, Qt::QueuedConnection);
        };

        // Breadth first, so results near the top of the tree come first.
        std::deque<std::string> pending{rootString};
        while (!pending.empty() && !cancel->load() && !truncated) {
            const std::string dir = std::move(pending.front());
            pending.pop_front();
            std::error_code ec;
            fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
            if (ec)
                continue;
            for (const fs::directory_iterator end; it != end && !cancel->load(); it.increment(ec)) {
                if (ec)
                    break;
                const std::string nameBytes = it->path().filename().string();
                if (nameBytes.empty() || (!includeHidden && nameBytes.front() == '.'))
                    continue;
                ++scanned;
                std::error_code typeError;
                const fs::file_status own = it->symlink_status(typeError);
                const bool realDir = !typeError && fs::is_directory(own);
                const std::string full = it->path().string();

                const QString name = QFile::decodeName(nameBytes.c_str());
                if (matcher.matches(name)) {
                    Hit hit;
                    hit.name = name;
                    const QString path = QFile::decodeName(full.c_str());
                    hit.url = QUrl::fromLocalFile(path);
                    const QString parent = QFile::decodeName(dir.c_str());
                    hit.folder = parent.size() > rootPath.size() ? parent.mid(rootPath.size()) : QString();
                    while (hit.folder.startsWith(u'/'))
                        hit.folder.remove(0, 1);
                    struct stat st {};
                    if (::stat(full.c_str(), &st) == 0) {     // follows links: a link to a folder opens as one
                        hit.dir = S_ISDIR(st.st_mode);
                        hit.size = qint64(st.st_size);
                        hit.mtime = qint64(st.st_mtime);
                    } else {
                        hit.dir = realDir;
                    }
                    batch.append(hit);
                    if (++found >= MaxResults) {
                        truncated = true;
                        break;
                    }
                }
                // Never follow links into other trees (and around loops).
                if (realDir && !skipDirectory(full))
                    pending.push_back(full);
                if (sinceFlush.elapsed() > 120)
                    flush();
            }
        }
        flush();
        const bool cancelled = cancel->load();
        QMetaObject::invokeMethod(qApp, [self, generation, truncated, cancelled] {
            if (self && self->m_generation == generation && !cancelled)
                self->finish(truncated, QString());
        }, Qt::QueuedConnection);
    });
}

void SearchModel::startRemote(const QUrl &root, const QString &query, bool includeHidden)
{
    const int generation = m_generation;
    const auto matcher = std::make_shared<Matcher>(query);
    KIO::ListJob *job = KIO::listRecursive(root, KIO::HideProgressInfo,
                                           includeHidden ? KIO::ListJob::ListFlags(KIO::ListJob::ListFlag::IncludeHidden)
                                                         : KIO::ListJob::ListFlags());
    m_job = job;
    connect(job, &KIO::ListJob::entries, this, [this, generation, matcher, root](KIO::Job *j, const KIO::UDSEntryList &list) {
        if (generation != m_generation)
            return;
        QList<Hit> hits;
        int scanned = m_scanned;
        for (const KIO::UDSEntry &entry : list) {
            // Relative path below the root, e.g. "docs/notes/todo.txt".
            const QString relative = entry.stringValue(KIO::UDSEntry::UDS_NAME);
            const qsizetype slash = relative.lastIndexOf(u'/');
            const QString name = slash < 0 ? relative : relative.mid(slash + 1);
            if (name.isEmpty() || name == "."_L1 || name == ".."_L1)
                continue;
            ++scanned;
            if (!matcher->matches(name))
                continue;
            Hit hit;
            hit.name = name;
            hit.url = Util::childUrl(root, relative);
            hit.folder = slash < 0 ? QString() : relative.left(slash);
            hit.dir = entry.isDir();
            hit.size = entry.numberValue(KIO::UDSEntry::UDS_SIZE, -1);
            hit.mtime = entry.numberValue(KIO::UDSEntry::UDS_MODIFICATION_TIME, 0);
            hits.append(hit);
        }
        append(hits, scanned);
        if (m_hits.size() >= MaxResults) {
            j->kill(KJob::Quietly);
            finish(true, QString());
        }
    });
    connect(job, &KJob::result, this, [this, generation](KJob *j) {
        if (generation != m_generation)
            return;
        const bool failed = j->error() != KJob::NoError && j->error() != KJob::KilledJobError;
        finish(false, failed ? j->errorString() : QString());
    });
}
