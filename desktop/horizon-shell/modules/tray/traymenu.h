// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

class QWindow;
class TrayHost;
class TrayItem;

// The context menu of a tray item: a client-side view of the menu another
// program exports with the com.canonical.dbusmenu interface.
//
// The layout is read as a whole (GetLayout, recursive) each time the menu is
// about to be shown and is kept current while it is open (LayoutUpdated,
// ItemsPropertiesUpdated). Depth, entry count, label length and icon size
// are bounded by TrayLimits. All calls are asynchronous.
class TrayMenu : public QObject
{
    Q_OBJECT
    // Changes with every change of the entries; QML re-reads entries() then.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    // The entries are those of the menu as it is now (a layout arrived since open()).
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    // A layout has been received at some time; entries() may be out of date.
    Q_PROPERTY(bool loaded READ loaded NOTIFY stateChanged)
    // Waiting for the application's answer.
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    // Why there is no menu: "" | "timeout" (the application does not answer)
    // | "error" (it answered with an error or with something that is not a menu)
    Q_PROPERTY(QString failure READ failure NOTIFY stateChanged)

public:
    TrayMenu(TrayHost *host, TrayItem *item, const QString &owner, const QString &path);
    ~TrayMenu() override;

    QString owner() const { return m_owner; }
    QString path() const { return m_path; }
    bool isOpen() const { return m_open; }

    int revision() const { return m_revision; }
    bool ready() const { return m_ready; }
    bool loaded() const { return m_loaded; }
    bool loading() const { return m_loading; }
    QString failure() const { return m_failure; }

    // The visible entries below 'parentId' (0: the top level), in order:
    // [{ id, label, enabled, separator, toggle: "" | "check" | "radio",
    //    state: 0 off | 1 on | 2 indeterminate, submenu: bool, icon: url or "" }]
    // Separators at the start or the end and repeated separators are left out.
    Q_INVOKABLE QVariantList entries(int parentId) const;
    // 'id' is an entry that opens a submenu.
    Q_INVOKABLE bool isSubmenu(int id) const;

    // The menu is about to be shown / is no longer shown.
    Q_INVOKABLE void open();
    Q_INVOKABLE void close();
    Q_INVOKABLE void openSubmenu(int id);
    Q_INVOKABLE void closeSubmenu(int id);
    // The user chose entry 'id' in window 'origin'.
    Q_INVOKABLE void trigger(int id, QWindow *origin);

    // The icon of an entry as a square of 'side' device pixels; null if none.
    QImage image(int id, int side, const QColor &foreground, const QColor &background) const;

    // LayoutUpdated or ItemsPropertiesUpdated arrived from the menu's object.
    void handleSignal(const QDBusMessage &message);

    // Reads the layout without telling the application that the menu is
    // shown (--dump tray).
    void peek();
    QJsonArray toJson(int parentId = 0) const;

Q_SIGNALS:
    void revisionChanged();
    void stateChanged();

private Q_SLOTS:
    void dbusSignal(const QDBusMessage &message);

private:
    struct Entry {
        int id = 0;
        int parent = -1;
        QList<int> children;
        QString label;
        QString iconName;
        QImage iconImage;           // decoded "icon-data"
        bool enabled = true;
        bool visible = true;
        bool separator = false;
        bool submenu = false;
        int toggle = 0;             // 0 none, 1 checkmark, 2 radio
        int toggleState = 2;        // 0 off, 1 on, 2 indeterminate (the default by specification)
        mutable int hasIcon = -1;   // -1 not looked up yet, 0 no, 1 yes
    };

    void listen(bool on);
    void aboutToShow(int id);
    void fetchLayout();
    void scheduleFetch();
    bool layoutArrived(const QDBusMessage &reply);
    bool readEntry(const QDBusArgument &argument, int parent, int depth, QHash<int, Entry> &into, int *id) const;
    static void setProperty(Entry &entry, const QString &name, const QVariant &value);
    static void resetProperty(Entry &entry, const QString &name);
    QVariantMap row(const Entry &entry) const;
    bool entryHasIcon(const Entry &entry) const;
    void sendEvent(int id, const QString &event);
    void setState(bool loading, const QString &failure);
    void bump();

    TrayHost *m_host;
    TrayItem *m_item;
    const QString m_owner;
    const QString m_path;

    QHash<int, Entry> m_entries;    // by id; 0 is the root
    int m_revision = 0;
    bool m_open = false;
    bool m_ready = false;
    bool m_loaded = false;
    bool m_loading = false;
    QString m_failure;

    bool m_fetching = false;
    bool m_fetchAgain = false;
    int m_session = 0;              // counts open() calls; answers to earlier ones are stale
    QTimer m_fetchTimer;

    QList<int> m_openSubmenus;      // in the order they were opened
    bool m_triggering = false;      // a "clicked" event waits for its activation token
    QList<int> m_closedLater;       // "closed" events held back until that click is sent
};
