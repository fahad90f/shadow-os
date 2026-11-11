// SPDX-License-Identifier: GPL-3.0-or-later
#include "statusmedia.h"

#include "statusdbus.h"

#include <QDBusArgument>
#include <QDBusServiceWatcher>
#include <QFileInfo>
#include <QJsonArray>

namespace StatusModule {

namespace {

const QString kPrefix = QStringLiteral("org.mpris.MediaPlayer2.");
const QString kPath = QStringLiteral("/org/mpris/MediaPlayer2");
const QString kRoot = QStringLiteral("org.mpris.MediaPlayer2");
const QString kPlayer = QStringLiteral("org.mpris.MediaPlayer2.Player");

bool isPlayerName(const QString &service)
{
    // playerctld only mirrors the other players.
    return service.startsWith(kPrefix) && service != kPrefix + QStringLiteral("playerctld");
}

// xesam:artist is a list of strings; some players send a single string.
QString textOf(const QVariant &value)
{
    if (value.metaType() == QMetaType::fromType<QDBusArgument>())
        return qdbus_cast<QStringList>(value).join(QStringLiteral(", "));
    if (value.metaType() == QMetaType::fromType<QStringList>())
        return value.toStringList().join(QStringLiteral(", "));
    return value.toString();
}

QUrl localArt(const QString &location)
{
    const QUrl url(location);
    if (!url.isLocalFile())
        return {};
    const QFileInfo info(url.toLocalFile());
    constexpr qint64 limit = 16 * 1024 * 1024;
    if (!info.isFile() || !info.isReadable() || info.size() > limit)
        return {};
    return url;
}

QString replyError(const QDBusMessage &reply)
{
    return reply.errorMessage().isEmpty() ? reply.errorName() : reply.errorMessage();
}

} // namespace

MediaBackend::MediaBackend(QObject *parent)
    : Backend(parent), m_bus(QDBusConnection::sessionBus())
{
    // A trailing "*" watches the whole org.mpris.MediaPlayer2 namespace.
    auto *watcher = new QDBusServiceWatcher(QStringLiteral("org.mpris.MediaPlayer2*"), m_bus,
                                            QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &service, const QString &, const QString &newOwner) {
        if (!isPlayerName(service))
            return;
        if (newOwner.isEmpty())
            removePlayer(service);
        else
            addPlayer(service);
    });

    const QDBusMessage list = dbusMethod(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                                         QStringLiteral("org.freedesktop.DBus"), QStringLiteral("ListNames"));
    dbusCall(m_bus, list, this, [this](const QDBusMessage &reply) {
        if (reply.type() != QDBusMessage::ReplyMessage) {
            setAvailable(false, tr("The session bus is not available: %1").arg(replyError(reply)));
            return;
        }
        m_listed = true;
        const QStringList names = reply.arguments().value(0).toStringList();
        for (const QString &name : names) {
            if (isPlayerName(name))
                addPlayer(name);
        }
        evaluate();
    });
}

void MediaBackend::addPlayer(const QString &service)
{
    Player &entry = m_players[service];
    if (!entry.root) {
        entry.root = new DBusProperties(m_bus, service, kPath, kRoot, this);
        entry.player = new DBusProperties(m_bus, service, kPath, kPlayer, this);
        entry.lastActive = ++m_clock;
        connect(entry.root, &DBusProperties::changed, this, &MediaBackend::evaluate);
        connect(entry.player, &DBusProperties::changed, this, &MediaBackend::evaluate);
    }
    entry.root->load();
    entry.player->load();
}

void MediaBackend::removePlayer(const QString &service)
{
    const Player entry = m_players.take(service);
    for (DBusProperties *properties : {entry.root, entry.player}) {
        if (!properties)
            continue;
        properties->disconnect(this);
        properties->deleteLater();
    }
    evaluate();
}

void MediaBackend::evaluate()
{
    if (!m_listed)
        return;

    QString best;
    bool bestPlaying = false;
    quint64 bestActive = 0;
    int count = 0;
    bool settled = true;
    for (auto it = m_players.begin(); it != m_players.end(); ++it) {
        Player &entry = it.value();
        settled = settled && entry.root->isLoaded() && entry.player->isLoaded();
        if (!entry.player->isValid())
            continue;       // not answering as an MPRIS player (yet)
        ++count;
        const bool playing = entry.player->string(QStringLiteral("PlaybackStatus")) == QLatin1String("Playing");
        if (playing && !entry.wasPlaying)
            entry.lastActive = ++m_clock;
        entry.wasPlaying = playing;
        if (best.isEmpty() || (playing && !bestPlaying) || (playing == bestPlaying && entry.lastActive > bestActive)) {
            best = it.key();
            bestPlaying = playing;
            bestActive = entry.lastActive;
        }
    }

    Snapshot s;
    if (!best.isEmpty()) {
        const Player &entry = m_players[best];
        s.service = best;
        if (entry.root->isValid())
            s.playerName = entry.root->string(QStringLiteral("Identity"));
        if (s.playerName.isEmpty())
            s.playerName = best.mid(kPrefix.size()).section(QLatin1Char('.'), 0, 0);
        const QVariantMap metadata = dbusMap(entry.player->value(QStringLiteral("Metadata")));
        s.title = metadata.value(QStringLiteral("xesam:title")).toString();
        if (s.title.isEmpty())
            s.title = QUrl(metadata.value(QStringLiteral("xesam:url")).toString()).fileName();
        s.artist = textOf(metadata.value(QStringLiteral("xesam:artist")));
        s.album = metadata.value(QStringLiteral("xesam:album")).toString();
        s.artUrl = localArt(metadata.value(QStringLiteral("mpris:artUrl")).toString());
        s.playbackStatus = entry.player->string(QStringLiteral("PlaybackStatus"));
        const bool control = entry.player->boolean(QStringLiteral("CanControl"), true);
        s.canPlayPause = control && (entry.player->boolean(QStringLiteral("CanPause"))
                                     || entry.player->boolean(QStringLiteral("CanPlay")));
        s.canGoNext = control && entry.player->boolean(QStringLiteral("CanGoNext"));
        s.canGoPrevious = control && entry.player->boolean(QStringLiteral("CanGoPrevious"));
    }

    if (s != m_now || best != m_current || count != m_playerCount) {
        m_now = s;
        m_current = best;
        m_playerCount = count;
        Q_EMIT changed();
    }
    if (settled || isReady())
        setAvailable(true);
}

void MediaBackend::call(const QString &method)
{
    if (m_current.isEmpty())
        return;
    dbusCall(m_bus, dbusMethod(m_current, kPath, kPlayer, method), this, [this](const QDBusMessage &reply) {
        if (reply.type() == QDBusMessage::ErrorMessage)
            Q_EMIT failed(tr("The media player did not accept the command: %1").arg(replyError(reply)));
    });
}

void MediaBackend::playPause()
{
    if (!hasPlayer() || !m_now.canPlayPause)
        return;
    // PlayPause is an error for a player that cannot pause while it plays.
    const Player &entry = m_players[m_current];
    if (playing() && !entry.player->boolean(QStringLiteral("CanPause")))
        call(QStringLiteral("Stop"));
    else
        call(QStringLiteral("PlayPause"));
}

void MediaBackend::next()
{
    if (hasPlayer() && m_now.canGoNext)
        call(QStringLiteral("Next"));
}

void MediaBackend::previous()
{
    if (hasPlayer() && m_now.canGoPrevious)
        call(QStringLiteral("Previous"));
}

QJsonObject MediaBackend::details() const
{
    QJsonObject json;
    if (!isAvailable())
        return json;
    QJsonArray players;
    for (auto it = m_players.cbegin(); it != m_players.cend(); ++it) {
        if (!it->player->isValid())
            continue;
        players.append(QJsonObject{
            {QStringLiteral("service"), it.key()},
            {QStringLiteral("identity"), it->root->string(QStringLiteral("Identity"))},
            {QStringLiteral("playbackStatus"), it->player->string(QStringLiteral("PlaybackStatus"))},
        });
    }
    json.insert(QStringLiteral("players"), players);
    json.insert(QStringLiteral("hasPlayer"), hasPlayer());
    if (hasPlayer()) {
        json.insert(QStringLiteral("current"), QJsonObject{
            {QStringLiteral("service"), m_now.service},
            {QStringLiteral("name"), m_now.playerName},
            {QStringLiteral("title"), m_now.title},
            {QStringLiteral("artist"), m_now.artist},
            {QStringLiteral("album"), m_now.album},
            {QStringLiteral("artUrl"), m_now.artUrl.toString()},
            {QStringLiteral("playbackStatus"), m_now.playbackStatus},
            {QStringLiteral("canPlayPause"), m_now.canPlayPause},
            {QStringLiteral("canGoNext"), m_now.canGoNext},
            {QStringLiteral("canGoPrevious"), m_now.canGoPrevious},
        });
    }
    return json;
}

} // namespace StatusModule
