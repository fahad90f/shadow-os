// SPDX-License-Identifier: GPL-3.0-or-later
//
// The system tray's back-end: StatusNotifierItem host and, when nobody else
// in the session is, the StatusNotifierWatcher items register with.
#pragma once

#include <QAbstractListModel>
#include <QColor>
#include <QDBusAbstractAdaptor>
#include <QDBusMessage>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>

class QDBusServiceWatcher;
class QWindow;
class TrayItem;
class TrayMenu;

// "tray" in QML: the list of items (role "trayItem": a TrayItem) and the
// state of the service.
class TrayHost : public QAbstractListModel
{
    Q_OBJECT
    // There is a session bus and a StatusNotifierWatcher (this shell's or
    // somebody else's) the tray is a host of.
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    // Who the watcher is: "own" (this shell) | "foreign" (another program;
    // the tray shows its items) | "none" (no session bus, or not known yet)
    Q_PROPERTY(QString watcher READ watcher NOTIFY stateChanged)
    // Items that are shown (their status is not Passive).
    Q_PROPERTY(int shownCount READ shownCount NOTIFY shownCountChanged)
    // The item whose menu is showing (set by the menu overlay), for the
    // "open" look of its icon. Null when none is.
    Q_PROPERTY(QObject *menuItem READ menuItem WRITE setMenuItem NOTIFY menuItemChanged)

public:
    enum Roles { ItemRole = Qt::UserRole + 1 };
    enum Mode {
        Shell,      // own the watcher name if it is free, else be a host of the existing watcher
        Observe,    // diagnostics: list the items of the existing watcher, register nothing
    };

    explicit TrayHost(QObject *parent = nullptr);
    ~TrayHost() override;

    // Asynchronous throughout; returns at once.
    void start(Mode mode);

    bool available() const { return m_watcherMode != NoWatcher; }
    QString watcher() const;
    int shownCount() const { return m_shown; }
    QObject *menuItem() const { return m_menuItem; }
    void setMenuItem(QObject *item);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // ---- for QML ----------------------------------------------------------------
    // Asks the menu overlay to show the menu of 'item' under the screen
    // position x (the centre of its icon).
    Q_INVOKABLE void showMenu(QObject *item, int x);
    Q_INVOKABLE void showToolTip(QObject *item, int x);
    Q_INVOKABLE void hideToolTip();

    // ---- for the image provider ---------------------------------------------------
    QImage itemImage(int serial, int side, const QColor &foreground, const QColor &background) const;
    QImage menuImage(int serial, int entryId, int side, const QColor &foreground, const QColor &background) const;

    // ---- for items and menus --------------------------------------------------------
    // Gets an xdg-activation token made from the last input in 'origin' and
    // calls 'then' with it — with an empty string if there is none within
    // TrayLimits::TokenTimeoutMs.
    void requestToken(QWindow *origin, const QString &appId, const std::function<void(const QString &)> &then);

    // ---- org.kde.StatusNotifierWatcher (called by the adaptor) -----------------------
    void registerItem(const QString &serviceOrPath, const QDBusMessage &message);
    void registerHost(const QString &service, const QDBusMessage &message);
    QStringList registeredItems() const;

    // Diagnostics.
    QJsonObject toJson() const;
    // Every known item has read its properties (or failed to).
    bool settled() const;
    const QList<TrayItem *> &items() const { return m_items; }

Q_SIGNALS:
    void stateChanged();
    void shownCountChanged();
    void menuItemChanged();
    // 'item' is a TrayItem.
    void menuRequested(QObject *item, int x);
    void toolTipRequested(QObject *item, int x);
    void toolTipDismissed();
    // The item is about to be destroyed: whatever shows it must let go.
    void itemRemoved(QObject *item);
    // The list of the foreign watcher has been read (Observe mode).
    void listed();

    // Signals of org.kde.StatusNotifierWatcher, relayed by the adaptor.
    void itemRegistered(const QString &service);
    void itemUnregistered(const QString &service);
    void hostRegistered();

private Q_SLOTS:
    void itemSignal(const QDBusMessage &message);
    void foreignItemRegistered(const QString &service);
    void foreignItemUnregistered(const QString &service);

private:
    enum WatcherMode { NoWatcher, OwnWatcher, ForeignWatcher };

    void requestWatcherName();
    void watcherOwnerChanged(const QString &owner);
    void becomeWatcher();
    void attachToForeignWatcher(const QString &owner);
    void addItem(const QString &service, const QString &path, const QString &client);
    void removeItem(TrayItem *item, bool announce);
    void removeAll();
    void serviceGone(const QString &service);
    void itemReady(TrayItem *item);
    void recount();
    TrayItem *itemBySerial(int serial) const;
    static bool splitRegistration(const QString &registration, QString *service, QString *path);

    Mode m_mode = Shell;
    bool m_started = false;
    WatcherMode m_watcherMode = NoWatcher;
    QString m_watcherOwner;
    QString m_hostName;                     // org.kde.StatusNotifierHost-<pid>
    QDBusServiceWatcher *m_nameWatcher = nullptr;
    QDBusServiceWatcher *m_serviceWatcher = nullptr;    // registered services and hosts

    QList<TrayItem *> m_items;              // every registered item, in order of registration
    QList<TrayItem *> m_rows;               // those that have read their properties: the model
    QHash<QString, TrayItem *> m_byRegistration;        // "<service><path>"
    QHash<QString, TrayItem *> m_byOwner;               // "<unique name><path>", for signals
    QHash<TrayItem *, QString> m_clients;               // who registered the item (unique name)
    QStringList m_hosts;
    int m_nextSerial = 1;
    int m_shown = 0;
    QPointer<QObject> m_menuItem;

    struct TokenRequest { std::function<void(const QString &)> then; };
    QHash<int, TokenRequest> m_tokenRequests;           // by input serial
    bool m_tokenConnected = false;
};

class TrayWatcherAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.StatusNotifierWatcher")
    Q_CLASSINFO("D-Bus Introspection",
                "  <interface name=\"org.kde.StatusNotifierWatcher\">\n"
                "    <method name=\"RegisterStatusNotifierItem\"><arg name=\"service\" type=\"s\" direction=\"in\"/></method>\n"
                "    <method name=\"RegisterStatusNotifierHost\"><arg name=\"service\" type=\"s\" direction=\"in\"/></method>\n"
                "    <property name=\"RegisteredStatusNotifierItems\" type=\"as\" access=\"read\"/>\n"
                "    <property name=\"IsStatusNotifierHostRegistered\" type=\"b\" access=\"read\"/>\n"
                "    <property name=\"ProtocolVersion\" type=\"i\" access=\"read\"/>\n"
                "    <signal name=\"StatusNotifierItemRegistered\"><arg type=\"s\"/></signal>\n"
                "    <signal name=\"StatusNotifierItemUnregistered\"><arg type=\"s\"/></signal>\n"
                "    <signal name=\"StatusNotifierHostRegistered\"/>\n"
                "    <signal name=\"StatusNotifierHostUnregistered\"/>\n"
                "  </interface>\n")
    Q_PROPERTY(QStringList RegisteredStatusNotifierItems READ registeredItems)
    Q_PROPERTY(bool IsStatusNotifierHostRegistered READ hostRegistered)
    Q_PROPERTY(int ProtocolVersion READ protocolVersion)

public:
    explicit TrayWatcherAdaptor(TrayHost *host);

    QStringList registeredItems() const;
    bool hostRegistered() const { return true; }       // this shell is one
    int protocolVersion() const { return 0; }

public Q_SLOTS:
    void RegisterStatusNotifierItem(const QString &service, const QDBusMessage &message);
    void RegisterStatusNotifierHost(const QString &service, const QDBusMessage &message);

Q_SIGNALS:
    void StatusNotifierItemRegistered(const QString &service);
    void StatusNotifierItemUnregistered(const QString &service);
    void StatusNotifierHostRegistered();
    void StatusNotifierHostUnregistered();

private:
    TrayHost *m_host;
};
