// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>

// Data for the About page: identity of the installed system.
// Sources: /etc/os-release, uname(2), /etc/hostname via gethostname(2).
class AboutBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString osName READ osName CONSTANT)
    Q_PROPERTY(QString osVersion READ osVersion CONSTANT)
    Q_PROPERTY(QString osCodename READ osCodename CONSTANT)
    Q_PROPERTY(QString osChannel READ osChannel CONSTANT)
    Q_PROPERTY(QString baseSystem READ baseSystem CONSTANT)
    Q_PROPERTY(QString homeUrl READ homeUrl CONSTANT)
    Q_PROPERTY(QString bugUrl READ bugUrl CONSTANT)
    Q_PROPERTY(QString kernel READ kernel CONSTANT)
    Q_PROPERTY(QString architecture READ architecture CONSTANT)
    Q_PROPERTY(QString hostname READ hostname CONSTANT)
    Q_PROPERTY(bool systemInformationAvailable READ systemInformationAvailable CONSTANT)

public:
    explicit AboutBackend(QObject *parent = nullptr);

    QString osName() const { return m_values.value(QStringLiteral("NAME")); }
    QString osVersion() const { return m_values.value(QStringLiteral("VERSION_ID")); }
    QString osCodename() const { return m_values.value(QStringLiteral("VERSION_CODENAME")); }
    QString osChannel() const { return m_values.value(QStringLiteral("SHADOW_CHANNEL")); }
    QString baseSystem() const { return m_values.value(QStringLiteral("SHADOW_BASE")); }
    QString homeUrl() const { return m_values.value(QStringLiteral("HOME_URL")); }
    QString bugUrl() const { return m_values.value(QStringLiteral("BUG_REPORT_URL")); }
    QString kernel() const { return m_kernel; }
    QString architecture() const { return m_architecture; }
    QString hostname() const { return m_hostname; }
    bool systemInformationAvailable() const;

    Q_INVOKABLE bool openSystemInformation() const;
    QJsonObject toJson() const;

private:
    QMap<QString, QString> m_values;
    QString m_kernel, m_architecture, m_hostname;
};
