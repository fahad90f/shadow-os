// SPDX-License-Identifier: GPL-3.0-or-later
#include "statusbrightness.h"

#include "statusdbus.h"

#include <QDir>
#include <QFile>

#include <cmath>

namespace StatusModule {

namespace {

const QString kLogin1 = QStringLiteral("org.freedesktop.login1");
// "auto": the caller's own session or, if the caller is not part of one (a
// shell started as a user service), the user's display session.
const QString kSessionPath = QStringLiteral("/org/freedesktop/login1/session/auto");
const QString kSession = QStringLiteral("org.freedesktop.login1.Session");

QString readText(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromLatin1(file.read(256)).trimmed();
}

int readInt(const QString &path, int fallback)
{
    bool ok = false;
    const int value = readText(path).toInt(&ok);
    return ok ? value : fallback;
}

int typeRank(const QString &type)
{
    if (type == QLatin1String("firmware"))
        return 3;
    if (type == QLatin1String("platform"))
        return 2;
    if (type == QLatin1String("raw"))
        return 1;
    return 0;
}

} // namespace

BrightnessBackend::BrightnessBackend(QObject *parent)
    : Backend(parent)
{
    // The override exists for tests with a fabricated device directory; the
    // name of the device is still what logind is asked to change.
    m_root = qEnvironmentVariable("NEXUS_STATUS_BACKLIGHT_DIR", QStringLiteral("/sys/class/backlight"));
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &BrightnessBackend::readValue);
    findDevice();
    if (m_device.isEmpty())
        updateAvailability();       // no hardware: logind need not be asked
    else
        probeSession();
}

void BrightnessBackend::findDevice()
{
    QString best, bestType;
    int bestMax = 0;
    int bestScore = -1;
    const QStringList names = QDir(m_root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : names) {
        const QString dir = m_root + QLatin1Char('/') + name;
        const int max = readInt(dir + QStringLiteral("/max_brightness"), 0);
        if (max <= 0)
            continue;
        const QString type = readText(dir + QStringLiteral("/type"));
        int score = typeRank(type) * 2;
        // Of several devices of one kind, the one whose connector is in use.
        if (readText(dir + QStringLiteral("/device/enabled")) == QLatin1String("enabled"))
            score += 1;
        if (score > bestScore) {
            bestScore = score;
            best = name;
            bestType = type;
            bestMax = max;
        }
    }

    if (best != m_device || bestMax != m_max) {
        if (!m_watcher.files().isEmpty())
            m_watcher.removePaths(m_watcher.files());
        m_device = best;
        m_type = bestType;
        m_max = bestMax;
        m_value = 0;
        if (!m_device.isEmpty()) {
            const QString dir = m_root + QLatin1Char('/') + m_device;
            for (const QString &file : {QStringLiteral("/brightness"), QStringLiteral("/actual_brightness")}) {
                if (QFile::exists(dir + file))
                    m_watcher.addPath(dir + file);
            }
        }
        Q_EMIT changed();
    }
    readValue();
}

void BrightnessBackend::probeSession()
{
    QDBusMessage call = dbusMethod(kLogin1, kSessionPath, QStringLiteral("org.freedesktop.DBus.Properties"),
                                   QStringLiteral("Get"));
    call << kSession << QStringLiteral("Id");
    dbusCall(QDBusConnection::systemBus(), call, this, [this](const QDBusMessage &reply) {
        m_sessionKnown = true;
        m_sessionOk = reply.type() == QDBusMessage::ReplyMessage;
        m_sessionError = m_sessionOk ? QString()
                                     : reply.errorMessage().isEmpty() ? reply.errorName() : reply.errorMessage();
        updateAvailability();
    });
}

void BrightnessBackend::updateAvailability()
{
    if (m_device.isEmpty())
        setAvailable(false, tr("This computer has no screen backlight that can be controlled (%1 has no device).").arg(m_root));
    else if (!m_sessionKnown)
        return;
    else if (!m_sessionOk)
        setAvailable(false, tr("Changing the brightness needs a logind session: %1").arg(m_sessionError));
    else
        setAvailable(true);
}

void BrightnessBackend::readValue()
{
    if (m_device.isEmpty())
        return;
    const int value = qBound(0, readInt(m_root + QLatin1Char('/') + m_device + QStringLiteral("/brightness"), m_value), m_max);
    if (value != m_value) {
        m_value = value;
        Q_EMIT changed();
    }
}

void BrightnessBackend::refresh()
{
    const bool hadDevice = !m_device.isEmpty();
    findDevice();
    if (!hadDevice && !m_device.isEmpty())
        probeSession();
    else if (hadDevice && m_device.isEmpty())
        updateAvailability();
}

// The lowest value this back-end sets: a "raw" device at 0 is a dark
// screen, while firmware and platform devices keep their lowest step visible.
int BrightnessBackend::lowest() const
{
    return m_type == QLatin1String("raw") ? qMax(1, int(std::ceil(m_max / 100.0))) : 0;
}

void BrightnessBackend::setLevel(qreal level)
{
    if (!isAvailable())
        return;
    write(qBound(lowest(), qRound(qBound(0.0, level, 1.0) * m_max), m_max));
}

void BrightnessBackend::step(int direction)
{
    if (!isAvailable() || direction == 0)
        return;
    // Steps add up while an earlier request is still on its way to logind.
    const int base = m_pending >= 0 ? m_pending : m_value;
    const int size = qMax(1, qRound(m_max * 0.05));
    write(qBound(lowest(), base + (direction > 0 ? size : -size), m_max));
}

// One request at a time: a value asked for while another is on its way
// replaces whatever was waiting, so dragging the slider never queues up.
void BrightnessBackend::write(int value)
{
    if (!m_inFlight && value == m_value) {
        m_pending = -1;
        return;
    }
    m_pending = value;
    if (m_inFlight)
        return;
    m_inFlight = true;
    QDBusMessage call = dbusMethod(kLogin1, kSessionPath, kSession, QStringLiteral("SetBrightness"));
    call << QStringLiteral("backlight") << m_device << quint32(value);
    dbusCall(QDBusConnection::systemBus(), call, this, [this, value](const QDBusMessage &reply) {
        m_inFlight = false;
        const int wanted = m_pending;
        m_pending = -1;
        readValue();        // what the kernel has now, not what was asked for
        if (reply.type() == QDBusMessage::ErrorMessage) {
            Q_EMIT failed(tr("The brightness could not be changed: %1")
                              .arg(reply.errorMessage().isEmpty() ? reply.errorName() : reply.errorMessage()));
            return;
        }
        if (wanted >= 0 && wanted != value)
            write(wanted);
    });
}

QJsonObject BrightnessBackend::details() const
{
    QJsonObject json{{QStringLiteral("directory"), m_root}};
    if (m_device.isEmpty())
        return json;
    json.insert(QStringLiteral("device"), m_device);
    json.insert(QStringLiteral("type"), m_type);
    json.insert(QStringLiteral("maxBrightness"), m_max);
    json.insert(QStringLiteral("brightness"), m_value);
    json.insert(QStringLiteral("percent"), qRound(level() * 100));
    json.insert(QStringLiteral("lowestSet"), lowest());
    if (m_sessionKnown) {
        json.insert(QStringLiteral("logindSession"), m_sessionOk);
        if (!m_sessionOk)
            json.insert(QStringLiteral("logindError"), m_sessionError);
    }
    return json;
}

} // namespace StatusModule
