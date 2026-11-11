// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "statusbackend.h"

#include <QDBusConnection>
#include <QHash>
#include <QUrl>

namespace StatusModule {

class DBusProperties;

// The media player the media keys and the media card act on.
//
// Source: MPRIS2 — every org.mpris.MediaPlayer2.* name on the session bus is
// a player; its /org/mpris/MediaPlayer2 object describes itself
// (org.mpris.MediaPlayer2: Identity) and what it plays
// (org.mpris.MediaPlayer2.Player: PlaybackStatus, Metadata, Can*). Names are
// followed with NameOwnerChanged, properties with PropertiesChanged.
//
// With several players the current one is a player that is playing — the
// one that started most recently — and otherwise the one that played last.
//
// Cover art is offered only when the player points at a local file; the
// shell does not fetch anything from the network on a player's behalf.
class MediaBackend : public Backend
{
    Q_OBJECT
    Q_PROPERTY(bool hasPlayer READ hasPlayer NOTIFY changed)
    Q_PROPERTY(int playerCount READ playerCount NOTIFY changed)
    Q_PROPERTY(QString playerName READ playerName NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString artist READ artist NOTIFY changed)
    Q_PROPERTY(QString album READ album NOTIFY changed)
    Q_PROPERTY(QUrl artUrl READ artUrl NOTIFY changed)
    // "Playing" | "Paused" | "Stopped"
    Q_PROPERTY(QString playbackStatus READ playbackStatus NOTIFY changed)
    Q_PROPERTY(bool playing READ playing NOTIFY changed)
    Q_PROPERTY(bool canPlayPause READ canPlayPause NOTIFY changed)
    Q_PROPERTY(bool canGoNext READ canGoNext NOTIFY changed)
    Q_PROPERTY(bool canGoPrevious READ canGoPrevious NOTIFY changed)

public:
    explicit MediaBackend(QObject *parent = nullptr);

    bool hasPlayer() const { return !m_current.isEmpty(); }
    int playerCount() const { return m_playerCount; }
    QString playerName() const { return m_now.playerName; }
    QString title() const { return m_now.title; }
    QString artist() const { return m_now.artist; }
    QString album() const { return m_now.album; }
    QUrl artUrl() const { return m_now.artUrl; }
    QString playbackStatus() const { return m_now.playbackStatus; }
    bool playing() const { return m_now.playbackStatus == QLatin1String("Playing"); }
    bool canPlayPause() const { return m_now.canPlayPause; }
    bool canGoNext() const { return m_now.canGoNext; }
    bool canGoPrevious() const { return m_now.canGoPrevious; }

    Q_INVOKABLE void playPause();
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();

Q_SIGNALS:
    void changed();

protected:
    QJsonObject details() const override;

private:
    struct Player {
        DBusProperties *root = nullptr;         // org.mpris.MediaPlayer2
        DBusProperties *player = nullptr;       // org.mpris.MediaPlayer2.Player
        quint64 lastActive = 0;
        bool wasPlaying = false;
    };
    struct Snapshot {
        QString service;
        QString playerName;
        QString title;
        QString artist;
        QString album;
        QUrl artUrl;
        QString playbackStatus;
        bool canPlayPause = false;
        bool canGoNext = false;
        bool canGoPrevious = false;
        bool operator==(const Snapshot &other) const = default;
    };

    void addPlayer(const QString &service);
    void removePlayer(const QString &service);
    void evaluate();
    void call(const QString &method);

    QDBusConnection m_bus;
    QHash<QString, Player> m_players;
    QString m_current;
    Snapshot m_now;
    quint64 m_clock = 0;
    int m_playerCount = 0;
    bool m_listed = false;
};

} // namespace StatusModule
