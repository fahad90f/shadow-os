// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>

namespace {
// Top-level keys end up in the [General] section of the file.
const char kCompactMode[] = "CompactMode";
const char kShowStatusBar[] = "ShowStatusBar";
const char kShowScrollBar[] = "ShowScrollBar";
const char kCopyOnSelect[] = "CopyOnSelect";
const char kConfirmPaste[] = "ConfirmMultilinePaste";
const char kTrimPaste[] = "TrimPastedNewlines";
const char kConfirmClose[] = "ConfirmCloseRunning";
const char kRestoreSession[] = "RestoreSession";
const char kSshTargets[] = "SshTargets";
const char kSessionGeometry[] = "Session/Geometry";
const char kSessionState[] = "Session/State";
} // namespace

QString SshTarget::destination() const
{
    QString text = user.isEmpty() ? host : user + QLatin1Char('@') + host;
    if (port != 22)
        text += QLatin1Char(':') + QString::number(port);
    return text;
}

QString SshTarget::label() const
{
    return name.isEmpty() ? destination() : name;
}

QJsonObject SshTarget::toJson() const
{
    return QJsonObject{
        {QStringLiteral("name"), name},
        {QStringLiteral("host"), host},
        {QStringLiteral("user"), user},
        {QStringLiteral("port"), port},
        {QStringLiteral("identityFile"), identityFile},
        {QStringLiteral("jumpHost"), jumpHost},
    };
}

Settings *Settings::instance()
{
    static Settings *self = new Settings(QCoreApplication::instance());
    return self;
}

Settings::Settings(QObject *parent)
    : QObject(parent)
    , m_settings(QSettings::NativeFormat, QSettings::UserScope, QStringLiteral("shadow"), QStringLiteral("terminal"))
{
}

QString Settings::filePath() const
{
    return m_settings.fileName();
}

bool Settings::flag(const char *key, bool fallback) const
{
    return m_settings.value(QLatin1String(key), fallback).toBool();
}

void Settings::setFlag(const char *key, bool on)
{
    if (flag(key, !on) == on && m_settings.contains(QLatin1String(key)))
        return;
    m_settings.setValue(QLatin1String(key), on);
    m_settings.sync();
    Q_EMIT changed();
}

bool Settings::compactMode() const { return flag(kCompactMode, false); }
void Settings::setCompactMode(bool on) { setFlag(kCompactMode, on); }
bool Settings::showStatusBar() const { return flag(kShowStatusBar, true); }
void Settings::setShowStatusBar(bool on) { setFlag(kShowStatusBar, on); }
bool Settings::showScrollBar() const { return flag(kShowScrollBar, true); }
void Settings::setShowScrollBar(bool on) { setFlag(kShowScrollBar, on); }
bool Settings::copyOnSelect() const { return flag(kCopyOnSelect, false); }
void Settings::setCopyOnSelect(bool on) { setFlag(kCopyOnSelect, on); }
bool Settings::confirmMultilinePaste() const { return flag(kConfirmPaste, true); }
void Settings::setConfirmMultilinePaste(bool on) { setFlag(kConfirmPaste, on); }
bool Settings::trimPastedNewlines() const { return flag(kTrimPaste, true); }
void Settings::setTrimPastedNewlines(bool on) { setFlag(kTrimPaste, on); }
bool Settings::confirmCloseRunning() const { return flag(kConfirmClose, true); }
void Settings::setConfirmCloseRunning(bool on) { setFlag(kConfirmClose, on); }

QString Settings::restoreSession() const
{
    const QString mode = m_settings.value(QLatin1String(kRestoreSession), QStringLiteral("ask")).toString();
    if (mode == QLatin1String("always") || mode == QLatin1String("never"))
        return mode;
    return QStringLiteral("ask");
}

void Settings::setRestoreSession(const QString &mode)
{
    if (mode == restoreSession() && m_settings.contains(QLatin1String(kRestoreSession)))
        return;
    m_settings.setValue(QLatin1String(kRestoreSession), mode);
    m_settings.sync();
    Q_EMIT changed();
}

QList<SshTarget> Settings::sshTargets() const
{
    QList<SshTarget> targets;
    // QSettings arrays are read through a non-const interface.
    QSettings &s = const_cast<QSettings &>(m_settings);
    const int count = s.beginReadArray(QLatin1String(kSshTargets));
    for (int i = 0; i < count; ++i) {
        s.setArrayIndex(i);
        SshTarget target;
        target.name = s.value(QStringLiteral("Name")).toString();
        target.host = s.value(QStringLiteral("Host")).toString();
        target.user = s.value(QStringLiteral("User")).toString();
        target.port = qBound(1, s.value(QStringLiteral("Port"), 22).toInt(), 65535);
        target.identityFile = s.value(QStringLiteral("IdentityFile")).toString();
        target.jumpHost = s.value(QStringLiteral("JumpHost")).toString();
        if (!target.host.isEmpty())
            targets.append(target);
    }
    s.endArray();
    return targets;
}

void Settings::setSshTargets(const QList<SshTarget> &targets)
{
    m_settings.remove(QLatin1String(kSshTargets));
    m_settings.beginWriteArray(QLatin1String(kSshTargets), int(targets.size()));
    for (int i = 0; i < targets.size(); ++i) {
        const SshTarget &target = targets.at(i);
        m_settings.setArrayIndex(i);
        m_settings.setValue(QStringLiteral("Name"), target.name);
        m_settings.setValue(QStringLiteral("Host"), target.host);
        m_settings.setValue(QStringLiteral("User"), target.user);
        m_settings.setValue(QStringLiteral("Port"), target.port);
        m_settings.setValue(QStringLiteral("IdentityFile"), target.identityFile);
        m_settings.setValue(QStringLiteral("JumpHost"), target.jumpHost);
    }
    m_settings.endArray();
    m_settings.sync();
    Q_EMIT changed();
}

QByteArray Settings::windowGeometry() const
{
    return m_settings.value(QLatin1String(kSessionGeometry)).toByteArray();
}

void Settings::setWindowGeometry(const QByteArray &geometry)
{
    m_settings.setValue(QLatin1String(kSessionGeometry), geometry);
    m_settings.sync();
}

QJsonObject Settings::savedSession() const
{
    const QByteArray json = m_settings.value(QLatin1String(kSessionState)).toString().toUtf8();
    return QJsonDocument::fromJson(json).object();
}

void Settings::setSavedSession(const QJsonObject &session)
{
    m_settings.setValue(QLatin1String(kSessionState),
                        QString::fromUtf8(QJsonDocument(session).toJson(QJsonDocument::Compact)));
    m_settings.sync();
}

void Settings::clearSavedSession()
{
    if (!m_settings.contains(QLatin1String(kSessionState)))
        return;
    m_settings.remove(QLatin1String(kSessionState));
    m_settings.sync();
}

QJsonObject Settings::toJson() const
{
    QJsonArray targets;
    const QList<SshTarget> ssh = sshTargets();
    for (const SshTarget &target : ssh)
        targets.append(target.toJson());
    const QJsonObject session = savedSession();
    return QJsonObject{
        {QStringLiteral("file"), filePath()},
        {QStringLiteral("compactMode"), compactMode()},
        {QStringLiteral("showStatusBar"), showStatusBar()},
        {QStringLiteral("showScrollBar"), showScrollBar()},
        {QStringLiteral("copyOnSelect"), copyOnSelect()},
        {QStringLiteral("confirmMultilinePaste"), confirmMultilinePaste()},
        {QStringLiteral("trimPastedNewlines"), trimPastedNewlines()},
        {QStringLiteral("confirmCloseRunning"), confirmCloseRunning()},
        {QStringLiteral("restoreSession"), restoreSession()},
        {QStringLiteral("sshTargets"), targets},
        {QStringLiteral("savedSession"), session.isEmpty() ? QJsonValue() : QJsonValue(session)},
    };
}
