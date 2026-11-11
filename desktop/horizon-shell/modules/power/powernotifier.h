// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QStringList>

namespace PowerModule {

// Desktop notifications through the standard org.freedesktop.Notifications
// D-Bus API (whatever daemon owns the name — the shell's own notifications
// module included, which is why every call is asynchronous).
//
// A notification is identified by a 'key' chosen by the caller; sending the
// same key again replaces the notification that is still showing.
class Notifier : public QObject
{
    Q_OBJECT

public:
    enum Urgency { Low = 0, Normal = 1, Critical = 2 };

    explicit Notifier(QObject *parent = nullptr);

    struct Message {
        QString key;
        QString appName;
        QString icon;           // icon-theme name
        QString desktopEntry;   // "desktop-entry" hint, may be empty
        QString summary;
        QString body;
        Urgency urgency = Normal;
        // key, label, key, label, ... ("default" = activating the notification itself)
        QStringList actions;
        bool resident = false;  // stays until closed (expire_timeout 0)
    };
    void send(const Message &message);
    void close(const QString &key);

    QJsonObject toJson() const;

Q_SIGNALS:
    void actionInvoked(const QString &key, const QString &action);

private Q_SLOTS:
    void onActionInvoked(uint id, const QString &action);
    void onClosed(uint id, uint reason);

private:
    struct Shown { uint id = 0; quint64 serial = 0; };
    QHash<QString, Shown> m_shown;
    quint64 m_serial = 0;
    int m_sent = 0;
    QString m_lastError;
    QString m_lastSummary;
};

} // namespace PowerModule
