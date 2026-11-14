// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <KService>
#include <QAbstractListModel>
#include <QFileSystemWatcher>
#include <QHash>
#include <QQmlEngine>
#include <QTimer>
#include <QWindow>

class QProcess;

struct AppEntry
{
    QString desktopId;      // storage id, e.g. "org.shadow.files.desktop"
    QString name, genericName, comment, icon, group;
    QStringList keywords;
    int launches = 0;
};

// Installed applications, from the freedesktop menu/desktop-entry database
// (KService), with launching and launch counting.
//
// One instance is shared by the launcher, the dock and the task model; the
// launcher view filters it through 'filter' and 'group'.
class Applications : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the shell as the 'applications' context property")
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(QString group READ group WRITE setGroup NOTIFY groupChanged)
    Q_PROPERTY(QStringList groups READ groups NOTIFY reloaded)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int totalCount READ totalCount NOTIFY reloaded)

public:
    enum Roles { DesktopIdRole = Qt::UserRole + 1, NameRole, GenericNameRole, CommentRole, IconRole, GroupRole, LaunchesRole };

    explicit Applications(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString filter() const { return m_filter; }
    void setFilter(const QString &filter);
    QString group() const { return m_group; }
    void setGroup(const QString &group);
    QStringList groups() const { return m_groups; }
    int count() const { return int(m_visible.size()); }
    int totalCount() const { return int(m_all.size()); }

    // Launch an application by desktop id. 'origin' is the shell window the
    // request came from, used to hand the new window focus (xdg-activation).
    Q_INVOKABLE bool launch(const QString &desktopId, QWindow *origin = nullptr);
    // Launch by position in the filtered view (Enter in the launcher).
    Q_INVOKABLE bool launchAt(int row, QWindow *origin = nullptr);
    Q_INVOKABLE QVariantMap get(const QString &desktopId) const;
    // Most launched applications, for the launcher's first screen.
    Q_INVOKABLE QVariantList frequent(int limit) const;

    // Map a window's app id (Wayland app_id / X11 class) to a desktop entry.
    // Returns an empty id if none matches.
    QString desktopIdForAppId(const QString &appId, const QString &resourceName = {}) const;
    const AppEntry *entry(const QString &desktopId) const;

Q_SIGNALS:
    void filterChanged();
    void groupChanged();
    void countChanged();
    void reloaded();
    void launchFailed(const QString &name, const QString &error);

private:
    void reload();
    void reloadModel();
    void refilter();
    static QString groupFor(const KService::Ptr &service);

    // The desktop-entry database (KSycoca) is only rebuilt when something runs
    // kbuildsycoca6; in a Plasma session that is kded6, which this session
    // does not run. So the application directories are watched here, and a
    // change (a package installed or removed) rebuilds the database and
    // reloads the model, without a new login.
    void watchDirectories();
    void rebuildDatabase();
    QFileSystemWatcher m_dirWatcher;
    QTimer m_rebuildTimer;          // debounces bursts of changes (dpkg unpacking)
    QProcess *m_builder = nullptr;  // running kbuildsycoca6
    bool m_rebuildAgain = false;    // changes arrived while it was running

    QList<AppEntry> m_all;
    QList<int> m_visible;                       // indices into m_all
    QHash<QString, int> m_byId;                 // desktopId → index
    mutable QHash<QString, QString> m_appIdCache;
    QStringList m_groups;
    QString m_filter, m_group;
};
