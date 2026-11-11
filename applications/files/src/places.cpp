// SPDX-License-Identifier: GPL-3.0-or-later
#include "places.h"

#include "util.h"

#include <KConfig>
#include <KConfigGroup>
#include <KDirWatch>
#include <KProtocolInfo>

#include <QCoreApplication>
#include <QDir>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

namespace {

QVariantMap entry(const QString &key, const QString &title, const QString &icon, const QUrl &url, const QString &kind)
{
    return {{u"key"_s, key}, {u"title"_s, title}, {u"icon"_s, icon}, {u"url"_s, url}, {u"kind"_s, kind}};
}

QVariantList readEntries(QSettings &settings, const QString &group, const QString &kind, const QString &icon)
{
    QVariantList list;
    const int n = settings.beginReadArray(group);
    for (int i = 0; i < n; ++i) {
        settings.setArrayIndex(i);
        const QUrl url(settings.value(u"url"_s).toString());
        if (!url.isValid() || url.isEmpty())
            continue;
        QString title = settings.value(u"title"_s).toString();
        if (title.isEmpty())
            title = Util::displayName(url);
        list.append(entry(kind + u':' + url.toString(), title, icon, url, kind));
    }
    settings.endArray();
    return list;
}

int indexOfUrl(const QVariantList &list, const QUrl &url)
{
    for (int i = 0; i < list.size(); ++i) {
        if (Util::sameUrl(list.at(i).toMap().value(u"url"_s).toUrl(), url))
            return i;
    }
    return -1;
}

QString trashConfigPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + u"/trashrc"_s;
}

} // namespace

Places::Places(QObject *parent)
    : QObject(parent)
{
    refresh();
    loadUserEntries();

    // kio_trash records whether the trash is empty in trashrc.
    m_trashWatch = new KDirWatch(this);
    m_trashWatch->addFile(trashConfigPath());
    connect(m_trashWatch, &KDirWatch::dirty, this, &Places::readTrashState);
    connect(m_trashWatch, &KDirWatch::created, this, &Places::readTrashState);
    connect(m_trashWatch, &KDirWatch::deleted, this, &Places::readTrashState);
    readTrashState();
}

Places *Places::instance()
{
    static Places *self = new Places(qApp);
    return self;
}

Places *Places::create(QQmlEngine *, QJSEngine *)
{
    Places *self = instance();
    QQmlEngine::setObjectOwnership(self, QQmlEngine::CppOwnership);
    return self;
}

QUrl Places::homeUrl() const
{
    return QUrl::fromLocalFile(QDir::homePath());
}

void Places::refresh()
{
    QVariantList list;
    const QString home = QDir::homePath();
    list.append(entry(u"home"_s, tr("Home"), u"home"_s, QUrl::fromLocalFile(home), u"place"_s));

    struct Standard {
        QStandardPaths::StandardLocation location;
        const char *key;
        const char *title;
        const char *icon;
    };
    static const Standard standards[] = {
        {QStandardPaths::DesktopLocation, "desktop", QT_TR_NOOP("Desktop"), "monitor"},
        {QStandardPaths::DocumentsLocation, "documents", QT_TR_NOOP("Documents"), "file-text"},
        {QStandardPaths::DownloadLocation, "downloads", QT_TR_NOOP("Downloads"), "download"},
        {QStandardPaths::MusicLocation, "music", QT_TR_NOOP("Music"), "music"},
        {QStandardPaths::PicturesLocation, "pictures", QT_TR_NOOP("Pictures"), "image"},
        {QStandardPaths::MoviesLocation, "videos", QT_TR_NOOP("Videos"), "video"},
    };
    QStringList seen{home};
    for (const Standard &s : standards) {
        const QString path = QDir::cleanPath(QStandardPaths::writableLocation(s.location));
        // Unconfigured XDG folders resolve to the home directory itself.
        if (path.isEmpty() || seen.contains(path) || !QDir(path).exists())
            continue;
        seen << path;
        list.append(entry(QString::fromLatin1(s.key), tr(s.title), QString::fromLatin1(s.icon),
                          QUrl::fromLocalFile(path), u"place"_s));
    }
    if (KProtocolInfo::isKnownProtocol(u"trash"_s))
        list.append(entry(u"trash"_s, tr("Trash"), u"trash"_s, QUrl(u"trash:/"_s), u"trash"_s));
    list.append(entry(u"root"_s, tr("File System"), u"harddrive"_s, QUrl::fromLocalFile(u"/"_s), u"root"_s));

    if (list != m_places) {
        m_places = list;
        Q_EMIT placesChanged();
    }
}

void Places::loadUserEntries()
{
    QSettings settings;
    m_bookmarks = readEntries(settings, u"Bookmarks"_s, u"bookmark"_s, u"star"_s);
    m_servers = readEntries(settings, u"Servers"_s, u"server"_s, u"server"_s);
}

void Places::save(const QString &group, const QVariantList &entries)
{
    QSettings settings;
    settings.remove(group);
    settings.beginWriteArray(group, int(entries.size()));
    for (int i = 0; i < entries.size(); ++i) {
        const QVariantMap map = entries.at(i).toMap();
        settings.setArrayIndex(i);
        settings.setValue(u"url"_s, map.value(u"url"_s).toUrl().toString());
        settings.setValue(u"title"_s, map.value(u"title"_s).toString());
    }
    settings.endArray();
    settings.sync();
}

bool Places::isBookmarked(const QUrl &url) const
{
    return indexOfUrl(m_bookmarks, url) >= 0;
}

void Places::addBookmark(const QUrl &url, const QString &title)
{
    if (!url.isValid() || url.isEmpty() || isBookmarked(url))
        return;
    m_bookmarks.append(entry(u"bookmark:"_s + url.toString(), title.isEmpty() ? Util::displayName(url) : title,
                             u"star"_s, url, u"bookmark"_s));
    save(u"Bookmarks"_s, m_bookmarks);
    Q_EMIT bookmarksChanged();
}

void Places::removeBookmark(const QUrl &url)
{
    const int i = indexOfUrl(m_bookmarks, url);
    if (i < 0)
        return;
    m_bookmarks.removeAt(i);
    save(u"Bookmarks"_s, m_bookmarks);
    Q_EMIT bookmarksChanged();
}

void Places::renameBookmark(const QUrl &url, const QString &title)
{
    const int i = indexOfUrl(m_bookmarks, url);
    const QString trimmed = title.trimmed();
    if (i < 0 || trimmed.isEmpty())
        return;
    QVariantMap map = m_bookmarks.at(i).toMap();
    map.insert(u"title"_s, trimmed);
    m_bookmarks[i] = map;
    save(u"Bookmarks"_s, m_bookmarks);
    Q_EMIT bookmarksChanged();
}

void Places::addServer(const QUrl &url, const QString &title)
{
    if (!url.isValid() || url.isEmpty() || indexOfUrl(m_servers, url) >= 0)
        return;
    QString label = title.trimmed();
    if (label.isEmpty()) {
        label = url.host().isEmpty() ? url.toDisplayString() : url.host();
        if (url.path().size() > 1)
            label += url.path();
    }
    m_servers.append(entry(u"server:"_s + url.toString(), label, u"server"_s, url, u"server"_s));
    save(u"Servers"_s, m_servers);
    Q_EMIT serversChanged();
}

void Places::removeServer(const QUrl &url)
{
    const int i = indexOfUrl(m_servers, url);
    if (i < 0)
        return;
    m_servers.removeAt(i);
    save(u"Servers"_s, m_servers);
    Q_EMIT serversChanged();
}

void Places::readTrashState()
{
    const KConfig config(trashConfigPath(), KConfig::SimpleConfig);
    const bool empty = config.group(u"Status"_s).readEntry("Empty", true);
    if (empty != m_trashEmpty) {
        m_trashEmpty = empty;
        Q_EMIT trashChanged();
    }
}

QJsonArray Places::toJson() const
{
    QJsonArray array;
    const auto append = [&array](const QVariantList &list) {
        for (const QVariant &v : list) {
            const QVariantMap map = v.toMap();
            const QUrl url = map.value(u"url"_s).toUrl();
            array.append(QJsonObject{
                {u"key"_s, map.value(u"key"_s).toString()},
                {u"title"_s, map.value(u"title"_s).toString()},
                {u"kind"_s, map.value(u"kind"_s).toString()},
                {u"icon"_s, map.value(u"icon"_s).toString()},
                {u"url"_s, url.toString()},
                {u"path"_s, url.isLocalFile() ? url.toLocalFile() : QString()},
            });
        }
    };
    append(m_places);
    append(m_bookmarks);
    append(m_servers);
    return array;
}
