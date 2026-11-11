// SPDX-License-Identifier: GPL-3.0-or-later
//
// The session's notification service: org.freedesktop.Notifications
// (Desktop Notifications Specification 1.2) and the state behind the popups
// and the notification centre.
#pragma once

#include "notificationtext.h"

#include <QAbstractListModel>
#include <QDBusAbstractAdaptor>
#include <QDBusMessage>
#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include <memory>
#include <unordered_map>

class Applications;
class QDBusServiceWatcher;
class QWindow;
class NotificationServer;

namespace NotificationLimits {
inline constexpr int MaxVisiblePopups = 4;      // popups on screen at a time
inline constexpr int MaxQueuedPopups = 8;       // waiting for a free place; the rest go to the centre only
inline constexpr int MaxHistory = 250;          // notifications kept in the centre
inline constexpr int MaxHistoryPerApp = 50;
inline constexpr int MaxActions = 6;            // buttons of one notification
inline constexpr int DefaultTimeoutLowMs = 5000;
inline constexpr int DefaultTimeoutMs = 7000;
inline constexpr int MinTimeoutMs = 1500;
inline constexpr int MaxTimeoutMs = 120000;
// Popups per client: a burst of this many, then one more every RefillMs.
inline constexpr int PopupBurst = 10;
inline constexpr int PopupRefillMs = 1000;
inline constexpr int MaxTrackedClients = 256;
} // namespace NotificationLimits

struct NotificationAction
{
    QString key;
    QString label;
};

// One notification as the shell keeps it. Every string has been cleaned and
// bounded (NotificationText), every image decoded and scaled down.
struct Notification
{
    uint id = 0;
    QString sender;             // unique bus name of the client
    bool clientGone = false;    // the client has left the bus: nobody would hear an action
    QString appKey;             // what notifications are grouped by
    QString appName;
    QString desktopEntry;
    QString summary;
    NotificationText::Body body;
    QString category;
    int urgency = 1;            // 0 low, 1 normal, 2 critical
    QList<NotificationAction> actions;  // without the default action
    bool hasDefaultAction = false;
    QImage image;               // "image-data" / "image-path"
    QImage appIcon;             // app_icon or the desktop entry's icon
    int revision = 0;           // changes when the pictures do
    bool transient = false;
    bool resident = false;
    int progress = -1;          // the "value" hint, 0–100; -1 none
    int timeoutMs = 0;          // of the popup; 0: stays until dismissed
    int remainingMs = 0;
    qint64 timestamp = 0;       // ms since the epoch
    bool unread = true;
    bool inHistory = false;
    enum Popup { NoPopup, Queued, Visible };
    Popup popup = NoPopup;
};

// A list of notifications for QML: the popups on screen, or the centre's
// history (grouped by application; see the "appKey" role).
class NotificationModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        IdRole = Qt::UserRole + 1, AppKeyRole, AppNameRole, SummaryRole, BodyRole, BodyPlainRole, UrgencyRole,
        CriticalRole, ActionsRole, HasDefaultActionRole, ImageSourceRole, IconSourceRole, ProgressRole,
        TimestampRole, UnreadRole, ResidentRole, InteractiveRole,
    };

    explicit NotificationModel(NotificationServer *server);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return int(m_ids.size()); }

    const QList<uint> &ids() const { return m_ids; }
    // Makes the model show 'ids', with the smallest change that does it: a
    // removal or an insertion where possible (so views keep their place),
    // a reset otherwise.
    void setIds(const QList<uint> &ids);
    void notifyChanged(uint id);

Q_SIGNALS:
    void countChanged();

private:
    NotificationServer *m_server;
    QList<uint> m_ids;
};

class NotificationServer : public QObject
{
    Q_OBJECT
    // This shell is the session's notification service. False while another
    // program owns org.freedesktop.Notifications (it is never taken away
    // from it) or when there is no session bus.
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    // Why not: "" | "no-bus" | "other-service"
    Q_PROPERTY(QString unavailableReason READ unavailableReason NOTIFY availableChanged)
    // Popups are held back, except those of critical urgency. Persisted.
    Q_PROPERTY(bool doNotDisturb READ doNotDisturb WRITE setDoNotDisturb NOTIFY doNotDisturbChanged)
    Q_PROPERTY(int unreadCount READ unreadCount NOTIFY unreadCountChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(NotificationModel *popups READ popups CONSTANT)
    Q_PROPERTY(NotificationModel *history READ history CONSTANT)
    // The pointer is over the popups: their timeouts stand still.
    Q_PROPERTY(bool popupsHovered READ popupsHovered WRITE setPopupsHovered NOTIFY popupsHoveredChanged)
    // The notification centre is showing: no popups meanwhile.
    Q_PROPERTY(bool centerOpen READ centerOpen WRITE setCenterOpen NOTIFY centerOpenChanged)
    // Changes whenever the groups of the history do (see group()).
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)

public:
    enum CloseReason { Expired = 1, Dismissed = 2, ClosedByClient = 3, Undefined = 4 };

    // 'applications' may be null (diagnostics); desktop entries are then not resolved.
    explicit NotificationServer(Applications *applications, QObject *parent = nullptr);
    ~NotificationServer() override;

    // Exports the service and asks for the bus name, without blocking.
    void start();

    bool available() const { return m_available; }
    QString unavailableReason() const;
    bool doNotDisturb() const { return m_doNotDisturb; }
    void setDoNotDisturb(bool on);
    int unreadCount() const { return m_unread; }
    int count() const { return m_historyCount; }
    NotificationModel *popups() { return &m_popupModel; }
    NotificationModel *history() { return &m_historyModel; }
    bool popupsHovered() const { return m_popupsHovered; }
    void setPopupsHovered(bool hovered);
    bool centerOpen() const { return m_centerOpen; }
    void setCenterOpen(bool open);
    int revision() const { return m_revision; }

    const Notification *notification(uint id) const;
    QImage image(uint id) const;
    QImage appIcon(uint id) const;

    // ---- for QML ------------------------------------------------------------
    // The user clicked a notification: its default action if it has one,
    // and the notification is closed. 'origin' is the window clicked in.
    Q_INVOKABLE void activate(uint id, QWindow *origin = nullptr);
    Q_INVOKABLE void invokeAction(uint id, const QString &key, QWindow *origin = nullptr);
    // Closes a notification for good ("dismissed by the user").
    Q_INVOKABLE void dismiss(uint id);
    Q_INVOKABLE void dismissApp(const QString &appKey);
    Q_INVOKABLE void clearAll();
    Q_INVOKABLE void markAllRead();
    // Opens hyperlink number 'link' of the notification's body.
    Q_INVOKABLE void openLink(uint id, const QString &link);
    // { name, count, iconSource } of a group of the history.
    Q_INVOKABLE QVariantMap group(const QString &appKey) const;

    // ---- org.freedesktop.Notifications (called by the adaptor) -----------------
    uint notify(const QString &sender, const QString &appName, uint replacesId, const QString &appIcon,
                const QString &summary, const QString &body, const QStringList &actions, const QVariantMap &hints,
                int timeout);
    void closeByClient(const QString &sender, uint id);
    static QStringList capabilities();

    // Diagnostics: everything the service holds.
    QJsonObject state() const;
    static QString configPath();
    static bool readDoNotDisturb();

Q_SIGNALS:
    void availableChanged();
    void doNotDisturbChanged();
    void unreadCountChanged();
    void countChanged();
    void popupsHoveredChanged();
    void centerOpenChanged();
    void revisionChanged();

private:
    void requestName();
    void setOwner(const QString &owner);
    void loadConfig();
    void watchConfig();

    void fill(Notification &n, const QString &appName, const QString &appIcon, const QString &summary,
              const QString &body, const QStringList &actions, const QVariantMap &hints, int timeout);
    bool mayPopup(const Notification &n);
    void present(Notification &n);
    void withdrawPopup(Notification &n);
    void promoteQueued();
    void close(uint id, CloseReason reason);
    void emitClosed(const Notification &n, CloseReason reason);
    void deliverAction(uint id, const QString &key, const QString &token);
    void trimHistory();
    void syncPopups();
    void scheduleHistorySync();
    void syncHistory();
    void recount();
    void tick();
    void updateTicking();
    void trackClient(const QString &sender);
    void untrackClient(const QString &sender);
    void clientGone(const QString &sender);
    uint nextId();

    Applications *m_applications;
    bool m_started = false;
    bool m_busConnected = false;
    bool m_available = false;
    QString m_owner;                    // unique name of the owner of the bus name
    QDBusServiceWatcher *m_nameWatcher = nullptr;
    QDBusServiceWatcher *m_clientWatcher = nullptr;
    QHash<QString, int> m_clients;      // sender → number of its notifications

    bool m_doNotDisturb = false;
    QFileSystemWatcher m_configWatcher;

    std::unordered_map<uint, std::unique_ptr<Notification>> m_notifications;
    uint m_lastId = 0;
    QList<uint> m_visible;              // popups on screen, newest first
    QList<uint> m_queue;                // popups waiting for a place
    NotificationModel m_popupModel;
    NotificationModel m_historyModel;
    QTimer m_historySync;
    QTimer m_tick;
    QElapsedTimer m_tickClock;
    bool m_popupsHovered = false;
    bool m_centerOpen = false;
    int m_unread = 0;
    int m_historyCount = 0;
    int m_revision = 0;

    struct Bucket { double tokens = 0; qint64 stamp = 0; };
    QHash<QString, Bucket> m_buckets;   // popup allowance per client
    QElapsedTimer m_clock;

    struct TokenRequest { uint id; QString key; };
    QHash<int, TokenRequest> m_tokenRequests;   // by input serial

    // Counters for diagnostics.
    struct {
        quint64 received = 0, replaced = 0, popupsShown = 0, heldByDoNotDisturb = 0, heldByRateLimit = 0,
                heldByQueue = 0, closedExpired = 0, closedDismissed = 0, closedByClient = 0, closedUndefined = 0,
                actionsInvoked = 0, slowestNotifyUs = 0;
    } m_stats;
};

// The D-Bus face of the server.
class NotificationAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
    Q_CLASSINFO("D-Bus Introspection",
                "  <interface name=\"org.freedesktop.Notifications\">\n"
                "    <method name=\"GetCapabilities\"><arg name=\"capabilities\" type=\"as\" direction=\"out\"/></method>\n"
                "    <method name=\"Notify\">\n"
                "      <arg name=\"app_name\" type=\"s\" direction=\"in\"/>\n"
                "      <arg name=\"replaces_id\" type=\"u\" direction=\"in\"/>\n"
                "      <arg name=\"app_icon\" type=\"s\" direction=\"in\"/>\n"
                "      <arg name=\"summary\" type=\"s\" direction=\"in\"/>\n"
                "      <arg name=\"body\" type=\"s\" direction=\"in\"/>\n"
                "      <arg name=\"actions\" type=\"as\" direction=\"in\"/>\n"
                "      <arg name=\"hints\" type=\"a{sv}\" direction=\"in\"/>\n"
                "      <annotation name=\"org.qtproject.QtDBus.QtTypeName.In6\" value=\"QVariantMap\"/>\n"
                "      <arg name=\"expire_timeout\" type=\"i\" direction=\"in\"/>\n"
                "      <arg name=\"id\" type=\"u\" direction=\"out\"/>\n"
                "    </method>\n"
                "    <method name=\"CloseNotification\"><arg name=\"id\" type=\"u\" direction=\"in\"/></method>\n"
                "    <method name=\"GetServerInformation\">\n"
                "      <arg name=\"name\" type=\"s\" direction=\"out\"/>\n"
                "      <arg name=\"vendor\" type=\"s\" direction=\"out\"/>\n"
                "      <arg name=\"version\" type=\"s\" direction=\"out\"/>\n"
                "      <arg name=\"spec_version\" type=\"s\" direction=\"out\"/>\n"
                "    </method>\n"
                "    <signal name=\"NotificationClosed\"><arg name=\"id\" type=\"u\"/><arg name=\"reason\" type=\"u\"/></signal>\n"
                "    <signal name=\"ActionInvoked\"><arg name=\"id\" type=\"u\"/><arg name=\"action_key\" type=\"s\"/></signal>\n"
                "    <signal name=\"ActivationToken\"><arg name=\"id\" type=\"u\"/><arg name=\"activation_token\" type=\"s\"/></signal>\n"
                "  </interface>\n")

public:
    explicit NotificationAdaptor(NotificationServer *server);

public Q_SLOTS:
    QStringList GetCapabilities();
    uint Notify(const QString &appName, uint replacesId, const QString &appIcon, const QString &summary,
                const QString &body, const QStringList &actions, const QVariantMap &hints, int expireTimeout,
                const QDBusMessage &message);
    void CloseNotification(uint id, const QDBusMessage &message);
    QString GetServerInformation(QString &vendor, QString &version, QString &specVersion);

private:
    NotificationServer *m_server;
};

// org.nexus.horizon.Notifications on the same object: read-only diagnostics
// for "horizon-shell --dump notifications" and the tests.
class NotificationDebugAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.nexus.horizon.Notifications")

public:
    explicit NotificationDebugAdaptor(NotificationServer *server);

public Q_SLOTS:
    // The server's state as a JSON document.
    QString GetState();

private:
    NotificationServer *m_server;
};
