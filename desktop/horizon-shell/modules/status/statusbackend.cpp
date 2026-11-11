// SPDX-License-Identifier: GPL-3.0-or-later
#include "statusbackend.h"

namespace StatusModule {

Backend::Backend(QObject *parent)
    : QObject(parent)
{
}

QJsonObject Backend::toJson() const
{
    QJsonObject json = details();
    json.insert(QStringLiteral("available"), m_available);
    json.insert(QStringLiteral("ready"), m_ready);
    if (!m_available)
        json.insert(QStringLiteral("reason"), m_ready ? m_reason : QStringLiteral("still being queried"));
    return json;
}

void Backend::setAvailable(bool available, const QString &reason)
{
    const bool wasReady = m_ready;
    const bool changed = available != m_available || (!available && reason != m_reason);
    m_available = available;
    m_reason = available ? QString() : reason;
    m_ready = true;
    if (changed)
        Q_EMIT availabilityChanged();
    if (!wasReady)
        Q_EMIT readyChanged();
}

} // namespace StatusModule
