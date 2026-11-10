// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDBusArgument>
#include <QObject>
#include <QQmlEngine>
#include <QVariantList>

// Virtual desktops ("workspaces") of the compositor, through KWin's D-Bus
// interface org.kde.KWin.VirtualDesktopManager.
class Workspaces : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the shell as the 'workspaces' context property")
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(QVariantList list READ list NOTIFY listChanged)      // [{id, name, position}]
    Q_PROPERTY(QString current READ current NOTIFY currentChanged)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY currentChanged)
    Q_PROPERTY(int count READ count NOTIFY listChanged)

public:
    explicit Workspaces(QObject *parent = nullptr);

    bool available() const { return m_available; }
    QVariantList list() const { return m_list; }
    QString current() const { return m_current; }
    int currentIndex() const;
    int count() const { return int(m_list.size()); }

    Q_INVOKABLE void switchTo(const QString &id);
    Q_INVOKABLE void switchToIndex(int index);
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    Q_INVOKABLE void add();
    Q_INVOKABLE void remove(const QString &id);

Q_SIGNALS:
    void availableChanged();
    void listChanged();
    void currentChanged();

private Q_SLOTS:
    void refresh();
    void onCurrentChanged(const QString &id);

private:
    bool m_available = false;
    QVariantList m_list;
    QString m_current;
};
