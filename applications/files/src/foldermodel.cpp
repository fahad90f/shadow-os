// SPDX-License-Identifier: GPL-3.0-or-later
#include "foldermodel.h"

#include "archiver.h"
#include "interaction.h"
#include "operations.h"
#include "thumbnails.h"
#include "util.h"

#include <KCoreDirLister>
#include <KIO/FileSystemFreeSpaceJob>
#include <KIO/Global>
#include <KIO/Job>
#include <KProtocolInfo>
#include <KProtocolManager>

#include <QDBusConnection>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QIcon>
#include <QJsonObject>
#include <QLocale>
#include <QMimeType>
#include <QRegularExpression>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

namespace {

constexpr int kThumbnailSize = 256;
constexpr int kIncrementalLimit = 64;

QLocale sortingLocale()
{
    // QCollator ignores numeric mode in the "C" locale; file names should
    // sort naturally ("file2" before "file10") in every locale.
    const QLocale locale;
    return locale.language() == QLocale::C ? QLocale(QLocale::English, QLocale::UnitedStates) : locale;
}

// Icons of the XDG user folders, by absolute path.
const QHash<QString, QString> &standardFolderIcons()
{
    static const QHash<QString, QString> icons = [] {
        QHash<QString, QString> map;
        const auto add = [&map](QStandardPaths::StandardLocation location, const QString &icon) {
            const QString path = QStandardPaths::writableLocation(location);
            if (!path.isEmpty() && path != QDir::homePath())
                map.insert(QDir::cleanPath(path), icon);
        };
        add(QStandardPaths::DesktopLocation, u"user-desktop"_s);
        add(QStandardPaths::DocumentsLocation, u"folder-documents"_s);
        add(QStandardPaths::DownloadLocation, u"folder-download"_s);
        add(QStandardPaths::MusicLocation, u"folder-music"_s);
        add(QStandardPaths::PicturesLocation, u"folder-pictures"_s);
        add(QStandardPaths::MoviesLocation, u"folder-videos"_s);
        map.insert(QDir::homePath(), u"user-home"_s);
        return map;
    }();
    return icons;
}

QString protocolHint(const QString &scheme)
{
    static const QSet<QString> extras = {
        u"sftp"_s, u"smb"_s, u"nfs"_s, u"fish"_s, u"mtp"_s, u"afc"_s, u"man"_s, u"info"_s, u"recentlyused"_s,
        u"tar"_s, u"zip"_s, u"ar"_s, u"7z"_s, u"network"_s,
    };
    if (extras.contains(scheme))
        return FolderModel::tr("Support for %1:// is provided by the “kio-extras” package, which is not installed.").arg(scheme);
    return FolderModel::tr("No KIO worker for %1:// is installed on this system.").arg(scheme);
}

QString errorTitle(int code)
{
    switch (code) {
    case KIO::ERR_DOES_NOT_EXIST:
        return FolderModel::tr("This folder does not exist");
    case KIO::ERR_ACCESS_DENIED:
    case KIO::ERR_CANNOT_ENTER_DIRECTORY:
        return FolderModel::tr("You do not have permission to open this folder");
    case KIO::ERR_UNKNOWN_HOST:
    case KIO::ERR_CANNOT_CONNECT:
    case KIO::ERR_SERVER_TIMEOUT:
    case KIO::ERR_CONNECTION_BROKEN:
    case KIO::ERR_CANNOT_CREATE_SOCKET:
        return FolderModel::tr("Could not connect to the server");
    case KIO::ERR_CANNOT_AUTHENTICATE:
    case KIO::ERR_CANNOT_LOGIN:
    case KIO::ERR_NEED_PASSWD:
        return FolderModel::tr("Signing in to the server failed");
    case KIO::ERR_UNSUPPORTED_PROTOCOL:
    case KIO::ERR_CANNOT_CREATE_WORKER:
        return FolderModel::tr("This kind of location is not supported");
    default:
        return FolderModel::tr("This location could not be opened");
    }
}

QUrl normalized(const QUrl &requested)
{
    QUrl url = requested.adjusted(QUrl::NormalizePathSegments);
    if (url.scheme().isEmpty())
        url.setScheme(u"file"_s);
    if (url.path().isEmpty())
        url.setPath(u"/"_s);
    if (url.path().size() > 1)
        url = url.adjusted(QUrl::StripTrailingSlash);
    return url;
}

} // namespace

FolderModel::FolderModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_lister(new KCoreDirLister(this))
    , m_collator(sortingLocale())
{
    m_collator.setNumericMode(true);
    m_collator.setCaseSensitivity(Qt::CaseInsensitive);

    m_lister->setAutoErrorHandlingEnabled(false);
    m_lister->setShowHiddenFiles(true);       // hidden files are filtered here, so toggling is instant
    m_lister->setDelayedMimeTypes(true);

    connect(m_lister, &KCoreDirLister::started, this, [this] { setLoading(true); });
    connect(m_lister, &KCoreDirLister::completed, this, [this] {
        setLoading(false);
        updateWritable();
        refreshSpace();
        applyPendingSelection();
        Q_EMIT loaded();
    });
    connect(m_lister, &KCoreDirLister::canceled, this, [this] {
        if (m_restarting)
            return;     // stopped by load() / reload(), which continue themselves
        setLoading(false);
        // The listing was cancelled from outside — typically the password
        // prompt of a server was dismissed. Nothing was listed, so this must
        // not look like an empty folder.
        if (m_error.isEmpty() && m_rows.isEmpty() && m_totalCount == 0) {
            m_listed = false;
            setError(tr("Opening this location was cancelled"),
                     tr("“%1” was not read. Reload to try again.").arg(Util::displayPath(m_url)));
        }
        Q_EMIT loaded();
    });
    connect(m_lister, &KCoreDirLister::clear, this, [this] {
        clearRows();
        m_existsTimer.start();
    });
    connect(m_lister, &KCoreDirLister::itemsAdded, this, [this](const QUrl &, const KFileItemList &items) {
        addItems(items);
    });
    connect(m_lister, &KCoreDirLister::itemsDeleted, this, &FolderModel::removeItems);
    connect(m_lister, &KCoreDirLister::refreshItems, this, &FolderModel::refreshItems);
    connect(m_lister, &KCoreDirLister::redirection, this, [this](const QUrl &, const QUrl &newUrl) {
        m_url = normalized(newUrl);
        Q_EMIT urlChanged();
    });
    connect(m_lister, &KCoreDirLister::jobError, this, [this](KIO::Job *job) {
        if (job->error() == KIO::ERR_IS_FILE) {
            // The location is a file: show it in its folder instead.
            const QUrl file = m_url;
            QTimer::singleShot(0, this, [this, file] {
                m_pendingSelection = {file};
                m_pendingFresh = true;
                m_pendingTimer.start();
                load(Util::parentUrl(file));
            });
            return;
        }
        setError(errorTitle(job->error()), job->errorString());
    });

    connect(ThumbnailStore::instance(), &ThumbnailStore::ready, this, &FolderModel::onThumbnailReady);

    m_pendingTimer.setSingleShot(true);
    m_pendingTimer.setInterval(4000);
    connect(&m_pendingTimer, &QTimer::timeout, this, [this] { m_pendingSelection.clear(); });

    m_spaceTimer.setSingleShot(true);
    m_spaceTimer.setInterval(1500);
    connect(&m_spaceTimer, &QTimer::timeout, this, &FolderModel::refreshSpace);

    m_existsTimer.setSingleShot(true);
    m_existsTimer.setInterval(400);
    connect(&m_existsTimer, &QTimer::timeout, this, &FolderModel::checkStillExists);

    // Local folders are watched by the kernel (KDirWatch). Remote folders and
    // the Trash only learn about changes through KDirNotify signals on the
    // session bus; so that the view is also right without a session bus, the
    // listing is re-read whenever one of our own file operations has ended.
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(300);
    connect(&m_refreshTimer, &QTimer::timeout, this, [this] {
        if (m_listed && !m_loading && m_error.isEmpty() && !m_url.isLocalFile())
            m_lister->updateDirectory(m_url);
    });
    connect(FileOperations::instance(), &FileOperations::operationFinished, this, [this] {
        if (m_listed && !m_url.isLocalFile() && !QDBusConnection::sessionBus().isConnected())
            m_refreshTimer.start();
    });
}

FolderModel::~FolderModel() = default;

// ---- model ---------------------------------------------------------------------

int FolderModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QHash<int, QByteArray> FolderModel::roleNames() const
{
    return {
        {NameRole, "name"},
        {UrlRole, "url"},
        {IsDirRole, "isDir"},
        {IsLinkRole, "isLink"},
        {IsHiddenRole, "isHidden"},
        {SizeRole, "size"},
        {MimeTypeRole, "mimeType"},
        {TypeNameRole, "typeName"},
        {IconNameRole, "iconName"},
        {ModifiedRole, "modified"},
        {ModifiedTextRole, "modifiedText"},
        {PermissionsRole, "permissions"},
        {OwnerRole, "owner"},
        {GroupRole, "group"},
        {SelectedRole, "selected"},
        {ThumbnailRole, "thumbnail"},
        {ReadableRole, "readable"},
    };
}

QString FolderModel::resolvedIconName(const KFileItem &item)
{
    if (item.isDir() && item.isLocalFile()) {
        const QString special = standardFolderIcons().value(item.localPath());
        if (!special.isEmpty() && QIcon::hasThemeIcon(special))
            return special;
    }
    static QHash<QString, QString> cache;
    const QString wanted = item.iconName();
    const auto it = cache.constFind(wanted);
    if (it != cache.constEnd())
        return *it;
    QString resolved = wanted;
    if (!QIcon::hasThemeIcon(wanted)) {
        const QString generic = item.currentMimeType().genericIconName();
        if (QIcon::hasThemeIcon(generic))
            resolved = generic;
        else
            resolved = item.isDir() ? u"folder"_s : u"unknown"_s;
    }
    cache.insert(wanted, resolved);
    return resolved;
}

QVariant FolderModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Row &row = m_rows.at(index.row());
    const KFileItem &item = row.item;

    // The final MIME type of an extension-less local file needs a look at its
    // content; only do that for files that are actually shown, and never on
    // slow (network) file systems.
    const auto finalType = [&item] {
        if (item.isLocalFile() && !item.isSlow())
            item.determineMimeType();
        return item.currentMimeType();
    };

    switch (role) {
    case Qt::DisplayRole:
    case NameRole: return item.text();
    case UrlRole: return item.url();
    case IsDirRole: return row.dir;
    case IsLinkRole: return item.isLink();
    case IsHiddenRole: return item.isHidden();
    case SizeRole: return row.dir ? QVariant(-1.0) : QVariant(double(row.size));
    case MimeTypeRole: return finalType().name();
    case TypeNameRole: return finalType().comment();
    case IconNameRole:
        finalType();
        return resolvedIconName(item);
    case ModifiedRole: return item.time(KFileItem::ModificationTime);
    case ModifiedTextRole: return Util::formatDateTime(item.time(KFileItem::ModificationTime));
    case PermissionsRole: return item.permissionsString();
    case OwnerRole: return item.user();
    case GroupRole: return item.group();
    case SelectedRole: return m_selected.contains(item.url());
    case ThumbnailRole:
        if (m_thumbnails && ThumbnailStore::canPreview(item))
            return ThumbnailStore::instance()->lookup(item, kThumbnailSize);
        return QString();
    case ReadableRole: return item.isReadable();
    }
    return {};
}

// ---- location --------------------------------------------------------------------

QString FolderModel::location() const
{
    return Util::displayPath(m_url);
}

namespace {

// Top-level entries of trash:/ are addressed as "<trash id>-<name>".
QString trashEntryName(const QString &segment)
{
    static const QRegularExpression prefix(u"^\\d+-"_s);
    QString name(segment);
    return name.remove(prefix);
}

} // namespace

QString FolderModel::title() const
{
    if (m_url.isLocalFile() && m_url.toLocalFile() == QDir::homePath())
        return tr("Home");
    if (isTrash() && m_url.path().count(u'/') == 1 && m_url.path().size() > 1)
        return trashEntryName(m_url.path().mid(1));
    return Util::displayName(m_url);
}

QString FolderModel::symbol() const
{
    if (isTrash())
        return u"trash"_s;
    if (!m_url.isLocalFile())
        return u"globe"_s;
    const QString path = m_url.toLocalFile();
    if (path == QDir::homePath())
        return u"home"_s;
    if (path == "/"_L1)
        return u"harddrive"_s;
    return u"folder"_s;
}

QVariantList FolderModel::crumbs() const
{
    QVariantList list;
    const auto add = [&list](const QString &label, const QUrl &url, const QString &icon = QString()) {
        list.append(QVariantMap{{u"label"_s, label}, {u"url"_s, url}, {u"icon"_s, icon}});
    };

    QUrl base(m_url);
    QString rest;       // path below `base`
    if (m_url.isLocalFile()) {
        const QString path = m_url.toLocalFile();
        const QString home = QDir::homePath();
        if (home != "/"_L1 && (path == home || path.startsWith(home + u'/'))) {
            base = QUrl::fromLocalFile(home);
            rest = path.mid(home.size());
            add(tr("Home"), base, u"home"_s);
        } else {
            base = QUrl::fromLocalFile(u"/"_s);
            rest = path;
            add(u"/"_s, base, u"harddrive"_s);
        }
    } else {
        rest = m_url.path();
        base.setPath(u"/"_s);
        if (isTrash()) {
            add(tr("Trash"), base, u"trash"_s);
        } else {
            QString label = m_url.scheme() + u"://"_s;
            if (!m_url.userName().isEmpty())
                label += m_url.userName() + u'@';
            label += m_url.host();
            if (m_url.port() > 0)
                label += u':' + QString::number(m_url.port());
            add(label, base, u"globe"_s);
        }
    }

    QUrl current(base);
    const QStringList parts = rest.split(u'/', Qt::SkipEmptyParts);
    for (qsizetype i = 0; i < parts.size(); ++i) {
        current = Util::childUrl(current, parts.at(i));
        add(isTrash() && i == 0 ? trashEntryName(parts.at(i)) : parts.at(i), current);
    }
    return list;
}

bool FolderModel::canGoUp() const
{
    return m_url.path().size() > 1;
}

void FolderModel::open(const QUrl &url)
{
    const QUrl target = normalized(url);
    if (!target.isValid())
        return;
    if (target == m_url) {
        if (!m_error.isEmpty())
            reload();
        return;
    }
    if (m_url.isValid() && !m_url.isEmpty()) {
        m_back.append(m_url);
        m_forward.clear();
    }
    load(target);
}

void FolderModel::openLocation(const QString &text)
{
    const QString base = m_url.isLocalFile() ? m_url.toLocalFile() : QDir::homePath();
    const QUrl url = Util::urlFromUserInput(text, base);
    if (!url.isValid() || url.isEmpty())
        return;
    if (url.isLocalFile()) {
        const QFileInfo info(url.toLocalFile());
        if (info.exists() && !info.isDir()) {
            reveal(url);
            return;
        }
    }
    open(url);
}

void FolderModel::reveal(const QUrl &url)
{
    const QUrl item = normalized(url);
    m_pendingSelection = {item};
    m_pendingFresh = true;
    m_pendingTimer.start();
    const QUrl dir = Util::parentUrl(item);
    if (normalized(dir) == m_url)
        applyPendingSelection();
    else
        open(dir);
}

void FolderModel::goBack()
{
    if (m_back.isEmpty())
        return;
    const QUrl target = m_back.takeLast();
    m_forward.append(m_url);
    load(target);
}

void FolderModel::goForward()
{
    if (m_forward.isEmpty())
        return;
    const QUrl target = m_forward.takeLast();
    m_back.append(m_url);
    load(target);
}

void FolderModel::goUp()
{
    if (!canGoUp())
        return;
    // Coming back up, the folder that was left is selected.
    const QUrl child = m_url;
    m_pendingSelection = {child};
    m_pendingFresh = true;
    m_pendingTimer.start();
    open(Util::parentUrl(m_url));
}

void FolderModel::reload()
{
    if (!m_url.isValid() || m_url.isEmpty())
        return;
    const QUrl url = m_url;
    setError({});
    if (!KProtocolInfo::isKnownProtocol(url.scheme()) || (url.isLocalFile() && !QFileInfo::exists(url.toLocalFile()))) {
        load(url);
        return;
    }
    setLoading(true);
    m_listed = true;
    m_restarting = true;
    const bool opened = m_lister->openUrl(url, KCoreDirLister::Reload);
    m_restarting = false;
    if (!opened) {
        m_listed = false;
        setLoading(false);
        setError(tr("This location could not be opened"), Util::displayPath(url));
        Q_EMIT loaded();
    }
}

void FolderModel::load(const QUrl &url)
{
    m_restarting = true;
    m_lister->stop();
    m_restarting = false;
    m_listed = false;
    m_url = url;
    setError({});
    clearRows();
    if (m_spaceKnown) {
        m_spaceKnown = false;
        Q_EMIT spaceChanged();
    }
    Q_EMIT urlChanged();
    Q_EMIT historyChanged();

    const auto fail = [this](const QString &error, const QString &detail) {
        setLoading(false);
        setError(error, detail);
        updateWritable();
        Q_EMIT loaded();
    };

    const QString scheme = url.scheme();
    if (!KProtocolInfo::isKnownProtocol(scheme)) {
        fail(tr("The “%1” protocol is not available").arg(scheme), protocolHint(scheme));
        return;
    }
    if (!KProtocolManager::supportsListing(url)) {
        fail(tr("This location cannot be browsed"),
             tr("%1:// addresses do not provide folders that can be listed.").arg(scheme));
        return;
    }
    if (url.isLocalFile()) {
        const QFileInfo info(url.toLocalFile());
        if (!info.exists()) {
            fail(tr("This folder does not exist"), url.toLocalFile());
            return;
        }
        if (!info.isDir()) {
            fail(tr("This is not a folder"), url.toLocalFile());
            return;
        }
    }

    setLoading(true);
    m_listed = true;
    if (!m_lister->openUrl(url)) {
        m_listed = false;
        fail(tr("This location could not be opened"), Util::displayPath(url));
        return;
    }
    updateWritable();
}

void FolderModel::clearRows()
{
    beginResetModel();
    m_rows.clear();
    m_folderCount = 0;
    m_hiddenCount = 0;
    m_totalCount = 0;
    endResetModel();
    if (!m_selected.isEmpty()) {
        m_selected.clear();
        Q_EMIT selectionChanged();
    }
    Q_EMIT countChanged();
}

bool FolderModel::loadBlocking(const QUrl &url, int timeoutMs)
{
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(this, &FolderModel::loaded, &loop, &QEventLoop::quit);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    open(url);
    if (m_loading) {
        timer.start(timeoutMs);
        loop.exec();
    }
    return !m_loading && m_error.isEmpty();
}

void FolderModel::setError(const QString &error, const QString &detail)
{
    if (m_error == error && m_errorDetail == detail)
        return;
    m_error = error;
    m_errorDetail = detail;
    Q_EMIT errorChanged();
}

void FolderModel::setLoading(bool loading)
{
    if (m_loading == loading)
        return;
    m_loading = loading;
    Q_EMIT loadingChanged();
}

void FolderModel::updateWritable()
{
    bool writable = false;
    if (m_error.isEmpty() && !isTrash()) {
        if (m_url.isLocalFile()) {
            writable = QFileInfo(m_url.toLocalFile()).isWritable();
        } else {
            writable = KProtocolManager::supportsWriting(m_url);
            const KFileItem root = m_lister->rootItem();
            if (writable && !root.isNull())
                writable = root.isWritable();
        }
    }
    if (writable != m_writable) {
        m_writable = writable;
        Q_EMIT writableChanged();
    }
}

void FolderModel::refreshSpace()
{
    if (!m_error.isEmpty() || isTrash())
        return;
    const QUrl url = m_url;
    KIO::FileSystemFreeSpaceJob *job = KIO::fileSystemFreeSpace(url);
    connect(job, &KJob::result, this, [this, job, url] {
        if (url != m_url)
            return;
        const bool known = job->error() == KJob::NoError && job->size() > 0;
        const qulonglong total = known ? job->size() : 0;
        const qulonglong free = known ? job->availableSize() : 0;
        if (known != m_spaceKnown || total != m_totalSpace || free != m_freeSpace) {
            m_spaceKnown = known;
            m_totalSpace = total;
            m_freeSpace = free;
            Q_EMIT spaceChanged();
        }
    });
}

void FolderModel::checkStillExists()
{
    if (!m_url.isLocalFile() || !m_error.isEmpty() || m_loading)
        return;
    const QString path = m_url.toLocalFile();
    if (QFileInfo::exists(path))
        return;
    // The folder being shown was deleted or unmounted: fall back to the
    // nearest ancestor that still exists.
    QString ancestor = path;
    while (ancestor.size() > 1 && !QFileInfo(ancestor).isDir())
        ancestor = QFileInfo(ancestor).path();
    Interaction::instance()->reportNotice(tr("“%1” no longer exists.").arg(Util::displayName(m_url)), u"warning"_s);
    load(QUrl::fromLocalFile(ancestor));
}

// ---- rows --------------------------------------------------------------------------

bool FolderModel::accepts(const KFileItem &item) const
{
    if (!m_showHidden && item.isHidden())
        return false;
    if (m_foldersOnly && !item.isDir())
        return false;
    if (!m_filter.isEmpty()) {
        const QString name = item.text();
        if (m_filter.contains(u'*') || m_filter.contains(u'?')) {
            const QRegularExpression re(QRegularExpression::wildcardToRegularExpression(m_filter),
                                        QRegularExpression::CaseInsensitiveOption);
            if (!re.match(name).hasMatch())
                return false;
        } else if (!name.contains(m_filter, Qt::CaseInsensitive)) {
            return false;
        }
    }
    return true;
}

FolderModel::Row FolderModel::makeRow(const KFileItem &item) const
{
    Row row{item, m_collator.sortKey(item.text()), item.size(),
            item.time(KFileItem::ModificationTime).toSecsSinceEpoch(), item.isDir(), QString()};
    if (m_sort == ByType)
        row.type = item.mimeComment();
    return row;
}

bool FolderModel::lessThan(const Row &a, const Row &b) const
{
    if (m_foldersFirst && a.dir != b.dir)
        return a.dir;
    int c = 0;
    switch (m_sort) {
    case BySize:
        if (!(a.dir && b.dir))
            c = a.size < b.size ? -1 : a.size > b.size ? 1 : 0;
        break;
    case ByModified:
        c = a.mtime < b.mtime ? -1 : a.mtime > b.mtime ? 1 : 0;
        break;
    case ByType:
        c = QString::localeAwareCompare(a.type, b.type);
        break;
    case ByName:
        break;
    }
    if (c == 0) {
        c = a.key.compare(b.key);
        if (c == 0)
            c = a.item.name().compare(b.item.name());
        // Ties of the other sort orders are always resolved A → Z.
        if (m_sort != ByName)
            return c < 0;
    }
    return m_ascending ? c < 0 : c > 0;
}

void FolderModel::recount()
{
    m_folderCount = int(std::count_if(m_rows.cbegin(), m_rows.cend(), [](const Row &r) { return r.dir; }));
}

void FolderModel::pruneSelection()
{
    if (m_selected.isEmpty())
        return;
    QSet<QUrl> present;
    for (const Row &row : std::as_const(m_rows)) {
        const QUrl url = row.item.url();
        if (m_selected.contains(url))
            present.insert(url);
    }
    if (present.size() != m_selected.size()) {
        m_selected = present;
        Q_EMIT selectionChanged();
    }
}

void FolderModel::rebuild()
{
    beginResetModel();
    m_rows.clear();
    m_hiddenCount = 0;
    // Only while the lister holds the folder being shown: after a failed
    // navigation it still has the items of the previous one.
    const KFileItemList items = m_listed ? m_lister->items(KCoreDirLister::AllItems) : KFileItemList();
    m_totalCount = int(items.size());
    m_rows.reserve(items.size());
    for (const KFileItem &item : items) {
        if (!m_showHidden && item.isHidden())
            ++m_hiddenCount;
        if (accepts(item))
            m_rows.append(makeRow(item));
    }
    std::sort(m_rows.begin(), m_rows.end(), [this](const Row &a, const Row &b) { return lessThan(a, b); });
    recount();
    endResetModel();
    pruneSelection();
    Q_EMIT countChanged();
}

void FolderModel::insertRow(const Row &row)
{
    const auto pos = std::upper_bound(m_rows.cbegin(), m_rows.cend(), row,
                                      [this](const Row &a, const Row &b) { return lessThan(a, b); });
    const int at = int(pos - m_rows.cbegin());
    beginInsertRows({}, at, at);
    m_rows.insert(at, row);
    endInsertRows();
    if (row.dir)
        ++m_folderCount;
}

void FolderModel::addItems(const KFileItemList &items)
{
    m_totalCount += int(items.size());
    QList<Row> fresh;
    for (const KFileItem &item : items) {
        if (!m_showHidden && item.isHidden())
            ++m_hiddenCount;
        if (accepts(item))
            fresh.append(makeRow(item));
    }
    if (!fresh.isEmpty()) {
        if (m_loading || m_rows.isEmpty() || fresh.size() > kIncrementalLimit) {
            beginResetModel();
            m_rows += fresh;
            std::sort(m_rows.begin(), m_rows.end(), [this](const Row &a, const Row &b) { return lessThan(a, b); });
            recount();
            endResetModel();
        } else {
            for (const Row &row : std::as_const(fresh))
                insertRow(row);
        }
    }
    Q_EMIT countChanged();
    applyPendingSelection();
    if (!m_loading)
        m_spaceTimer.start();
}

void FolderModel::removeItems(const KFileItemList &items)
{
    if (items.size() > kIncrementalLimit) {
        QSet<QUrl> gone;
        for (const KFileItem &item : items) {
            gone.insert(item.url());
            --m_totalCount;
            if (!m_showHidden && item.isHidden())
                --m_hiddenCount;
        }
        beginResetModel();
        m_rows.removeIf([&gone](const Row &row) { return gone.contains(row.item.url()); });
        recount();
        endResetModel();
        m_totalCount = qMax(0, m_totalCount);
        m_hiddenCount = qMax(0, m_hiddenCount);
        pruneSelection();
        Q_EMIT countChanged();
    } else {
        bool selectionTouched = false;
        for (const KFileItem &item : items) {
            --m_totalCount;
            if (!m_showHidden && item.isHidden())
                --m_hiddenCount;
            const int row = indexOfUrl(item.url());
            if (row < 0)
                continue;
            selectionTouched = m_selected.remove(item.url()) || selectionTouched;
            beginRemoveRows({}, row, row);
            if (m_rows.at(row).dir)
                --m_folderCount;
            m_rows.removeAt(row);
            endRemoveRows();
        }
        m_totalCount = qMax(0, m_totalCount);
        m_hiddenCount = qMax(0, m_hiddenCount);
        if (selectionTouched)
            Q_EMIT selectionChanged();
        Q_EMIT countChanged();
    }
    m_spaceTimer.start();
    m_existsTimer.start();
}

void FolderModel::refreshItems(const QList<QPair<KFileItem, KFileItem>> &items)
{
    bool selectionTouched = false;
    for (const auto &[oldItem, newItem] : items) {
        // The lister also reports changes of the folder being listed itself
        // (its modification time changes whenever an entry is added). That
        // item is not one of the rows.
        if (Util::sameUrl(oldItem.url(), m_url) || !Util::sameUrl(Util::parentUrl(newItem.url()), m_url)) {
            updateWritable();
            continue;
        }
        if (!m_showHidden) {
            if (oldItem.isHidden())
                --m_hiddenCount;
            if (newItem.isHidden())
                ++m_hiddenCount;
        }
        const QUrl oldUrl = oldItem.url();
        const QUrl newUrl = newItem.url();
        const bool wasSelected = m_selected.remove(oldUrl);
        const int row = indexOfUrl(oldUrl);
        const bool keep = accepts(newItem);
        if (wasSelected && keep)
            m_selected.insert(newUrl);
        selectionTouched = selectionTouched || (wasSelected && (!keep || oldUrl != newUrl));

        if (row >= 0) {
            const Row updated = keep ? makeRow(newItem) : m_rows.at(row);
            // In place if the row still sorts between its neighbours.
            const bool inPlace = keep
                && (row == 0 || !lessThan(updated, m_rows.at(row - 1)))
                && (row == m_rows.size() - 1 || !lessThan(m_rows.at(row + 1), updated));
            if (inPlace) {
                if (m_rows.at(row).dir != updated.dir)
                    m_folderCount += updated.dir ? 1 : -1;
                m_rows[row] = updated;
                const QModelIndex idx = index(row);
                Q_EMIT dataChanged(idx, idx);
            } else {
                beginRemoveRows({}, row, row);
                if (m_rows.at(row).dir)
                    --m_folderCount;
                m_rows.removeAt(row);
                endRemoveRows();
                if (keep)
                    insertRow(updated);
            }
        } else if (keep) {
            insertRow(makeRow(newItem));
        }
    }
    m_hiddenCount = qMax(0, m_hiddenCount);
    if (selectionTouched)
        Q_EMIT selectionChanged();
    else if (!m_selected.isEmpty())
        Q_EMIT selectionChanged();      // sizes of selected items may have changed
    Q_EMIT countChanged();
    applyPendingSelection();
    m_spaceTimer.start();
}

void FolderModel::onThumbnailReady(const QUrl &url, int size)
{
    if (!m_thumbnails || size != kThumbnailSize)
        return;
    const int row = indexOfUrl(url);
    if (row < 0)
        return;
    const QModelIndex idx = index(row);
    Q_EMIT dataChanged(idx, idx, {ThumbnailRole});
}

// ---- view options ------------------------------------------------------------------

void FolderModel::setShowHidden(bool show)
{
    if (m_showHidden == show)
        return;
    m_showHidden = show;
    rebuild();
    Q_EMIT showHiddenChanged();
}

void FolderModel::setFilter(const QString &filter)
{
    if (m_filter == filter)
        return;
    m_filter = filter;
    rebuild();
    Q_EMIT filterChanged();
}

QString FolderModel::sortRole() const
{
    switch (m_sort) {
    case BySize: return u"size"_s;
    case ByModified: return u"modified"_s;
    case ByType: return u"type"_s;
    case ByName: break;
    }
    return u"name"_s;
}

void FolderModel::setSortRole(const QString &role)
{
    const SortRole sort = role == "size"_L1 ? BySize : role == "modified"_L1 ? ByModified
        : role == "type"_L1 ? ByType : ByName;
    if (sort == m_sort)
        return;
    m_sort = sort;
    rebuild();
    Q_EMIT sortChanged();
}

void FolderModel::setSortAscending(bool ascending)
{
    if (m_ascending == ascending)
        return;
    m_ascending = ascending;
    rebuild();
    Q_EMIT sortChanged();
}

void FolderModel::setFoldersFirst(bool first)
{
    if (m_foldersFirst == first)
        return;
    m_foldersFirst = first;
    rebuild();
    Q_EMIT sortChanged();
}

void FolderModel::setFoldersOnly(bool only)
{
    if (m_foldersOnly == only)
        return;
    m_foldersOnly = only;
    rebuild();
    Q_EMIT foldersOnlyChanged();
}

void FolderModel::setThumbnails(bool enabled)
{
    if (m_thumbnails == enabled)
        return;
    m_thumbnails = enabled;
    if (!m_rows.isEmpty())
        Q_EMIT dataChanged(index(0), index(int(m_rows.size()) - 1), {ThumbnailRole});
    Q_EMIT thumbnailsChanged();
}

// ---- selection ---------------------------------------------------------------------

int FolderModel::selectedFolders() const
{
    int n = 0;
    for (const Row &row : m_rows) {
        if (row.dir && m_selected.contains(row.item.url()))
            ++n;
    }
    return n;
}

double FolderModel::selectedSize() const
{
    qulonglong sum = 0;
    for (const Row &row : m_rows) {
        if (!row.dir && m_selected.contains(row.item.url()))
            sum += row.size;
    }
    return double(sum);
}

QList<QUrl> FolderModel::selectedUrls() const
{
    QList<QUrl> urls;
    urls.reserve(m_selected.size());
    for (const Row &row : m_rows) {
        const QUrl url = row.item.url();
        if (m_selected.contains(url))
            urls.append(url);
    }
    return urls;
}

void FolderModel::emitSelection(int first, int last)
{
    if (first >= 0 && last >= first && last < m_rows.size())
        Q_EMIT dataChanged(index(first), index(last), {SelectedRole});
    Q_EMIT selectionChanged();
}

bool FolderModel::isSelected(int row) const
{
    return row >= 0 && row < m_rows.size() && m_selected.contains(m_rows.at(row).item.url());
}

void FolderModel::selectOnly(int row)
{
    if (row < 0 || row >= m_rows.size())
        return;
    const QUrl url = m_rows.at(row).item.url();
    if (m_selected.size() == 1 && m_selected.contains(url))
        return;
    m_selected.clear();
    m_selected.insert(url);
    emitSelection(0, int(m_rows.size()) - 1);
}

void FolderModel::toggle(int row)
{
    if (row < 0 || row >= m_rows.size())
        return;
    const QUrl url = m_rows.at(row).item.url();
    if (!m_selected.remove(url))
        m_selected.insert(url);
    emitSelection(row, row);
}

void FolderModel::setSelected(int row, bool selected)
{
    if (row < 0 || row >= m_rows.size())
        return;
    const QUrl url = m_rows.at(row).item.url();
    if (selected == m_selected.contains(url))
        return;
    if (selected)
        m_selected.insert(url);
    else
        m_selected.remove(url);
    emitSelection(row, row);
}

void FolderModel::selectRange(int from, int to, bool keepOthers)
{
    if (m_rows.isEmpty())
        return;
    const int last = int(m_rows.size()) - 1;
    const int a = qBound(0, qMin(from, to), last);
    const int b = qBound(0, qMax(from, to), last);
    if (!keepOthers)
        m_selected.clear();
    for (int row = a; row <= b; ++row)
        m_selected.insert(m_rows.at(row).item.url());
    emitSelection(0, last);
}

void FolderModel::selectRows(const QList<int> &rows)
{
    QSet<QUrl> wanted;
    for (int row : rows) {
        if (row >= 0 && row < m_rows.size())
            wanted.insert(m_rows.at(row).item.url());
    }
    if (wanted == m_selected)
        return;
    m_selected = wanted;
    emitSelection(0, int(m_rows.size()) - 1);
}

QList<int> FolderModel::selectedRows() const
{
    QList<int> rows;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_selected.contains(m_rows.at(i).item.url()))
            rows.append(i);
    }
    return rows;
}

void FolderModel::selectAll()
{
    if (m_rows.isEmpty())
        return;
    for (const Row &row : std::as_const(m_rows))
        m_selected.insert(row.item.url());
    emitSelection(0, int(m_rows.size()) - 1);
}

void FolderModel::clearSelection()
{
    if (m_selected.isEmpty())
        return;
    m_selected.clear();
    emitSelection(0, int(m_rows.size()) - 1);
}

void FolderModel::selectWhenAvailable(const QList<QUrl> &urls)
{
    if (urls.isEmpty())
        return;
    m_pendingSelection = urls;
    m_pendingFresh = true;
    m_pendingTimer.start();
    applyPendingSelection();
}

void FolderModel::applyPendingSelection()
{
    if (m_pendingSelection.isEmpty())
        return;
    int firstRow = -1;
    bool changed = false;
    for (auto it = m_pendingSelection.begin(); it != m_pendingSelection.end();) {
        const int row = indexOfUrl(*it);
        if (row < 0) {
            ++it;
            continue;
        }
        if (m_pendingFresh) {
            m_pendingFresh = false;
            m_selected.clear();
            firstRow = row;
        }
        m_selected.insert(m_rows.at(row).item.url());
        changed = true;
        it = m_pendingSelection.erase(it);
    }
    if (m_pendingSelection.isEmpty())
        m_pendingTimer.stop();
    if (changed)
        emitSelection(0, int(m_rows.size()) - 1);
    if (firstRow >= 0)
        Q_EMIT revealRow(firstRow);
}

// ---- lookups -----------------------------------------------------------------------

QVariantMap FolderModel::get(int row) const
{
    if (row < 0 || row >= m_rows.size())
        return {};
    const KFileItem &item = m_rows.at(row).item;
    if (item.isLocalFile() && !item.isSlow())
        item.determineMimeType();
    const QString mime = item.currentMimeType().name();
    return {
        {u"name"_s, item.text()},
        {u"url"_s, item.url()},
        {u"isDir"_s, item.isDir()},
        {u"isLink"_s, item.isLink()},
        {u"linkTarget"_s, item.linkDest()},
        {u"mimeType"_s, mime},
        {u"typeName"_s, item.currentMimeType().comment()},
        {u"iconName"_s, resolvedIconName(item)},
        {u"size"_s, item.isDir() ? -1.0 : double(item.size())},
        {u"localPath"_s, item.isLocalFile() ? item.localPath() : QString()},
        {u"isArchive"_s, !item.isDir() && item.isLocalFile() && Archiver::isArchive(mime, item.name())},
        {u"readable"_s, item.isReadable()},
        {u"writable"_s, item.isWritable()},
    };
}

QUrl FolderModel::urlAt(int row) const
{
    return row >= 0 && row < m_rows.size() ? m_rows.at(row).item.url() : QUrl();
}

bool FolderModel::isDirAt(int row) const
{
    return row >= 0 && row < m_rows.size() && m_rows.at(row).dir;
}

int FolderModel::indexOfUrl(const QUrl &url) const
{
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).item.url().matches(url, QUrl::StripTrailingSlash))
            return i;
    }
    return -1;
}

int FolderModel::findByPrefix(const QString &prefix, int from) const
{
    const int n = int(m_rows.size());
    if (n == 0 || prefix.isEmpty())
        return -1;
    const int start = qBound(0, from, n - 1);
    for (int i = 0; i < n; ++i) {
        const int row = (start + i) % n;
        if (m_rows.at(row).item.text().startsWith(prefix, Qt::CaseInsensitive))
            return row;
    }
    return -1;
}

bool FolderModel::nameExists(const QString &name) const
{
    return !m_lister->findByName(name).isNull();
}

QJsonArray FolderModel::toJson() const
{
    QJsonArray array;
    for (const Row &row : m_rows) {
        const KFileItem &item = row.item;
        if (item.isLocalFile() && !item.isSlow())
            item.determineMimeType();
        array.append(QJsonObject{
            {u"name"_s, item.name()},
            {u"isDir"_s, item.isDir()},
            {u"isLink"_s, item.isLink()},
            {u"size"_s, double(item.size())},
            {u"mimeType"_s, item.currentMimeType().name()},
            {u"modified"_s, item.time(KFileItem::ModificationTime).toUTC().toString(Qt::ISODate)},
            {u"permissions"_s, item.permissionsString()},
            {u"mode"_s, QString::number(item.permissions() & 07777, 8)},
            {u"owner"_s, item.user()},
            {u"group"_s, item.group()},
            {u"hidden"_s, item.isHidden()},
        });
    }
    return array;
}
