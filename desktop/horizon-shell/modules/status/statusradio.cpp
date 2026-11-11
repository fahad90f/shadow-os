// SPDX-License-Identifier: GPL-3.0-or-later
#include "statusradio.h"

#include <QJsonArray>
#include <QSocketNotifier>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <linux/rfkill.h>
#include <unistd.h>

namespace StatusModule {

namespace {

const char *const kDevice = "/dev/rfkill";

QString typeName(quint8 type)
{
    switch (type) {
    case RFKILL_TYPE_WLAN: return QStringLiteral("wlan");
    case RFKILL_TYPE_BLUETOOTH: return QStringLiteral("bluetooth");
    case RFKILL_TYPE_UWB: return QStringLiteral("uwb");
    case RFKILL_TYPE_WIMAX: return QStringLiteral("wimax");
    case RFKILL_TYPE_WWAN: return QStringLiteral("wwan");
    case RFKILL_TYPE_GPS: return QStringLiteral("gps");
    case RFKILL_TYPE_FM: return QStringLiteral("fm");
    case RFKILL_TYPE_NFC: return QStringLiteral("nfc");
    default: return QStringLiteral("type-%1").arg(type);
    }
}

QString errnoText(int error)
{
    return QString::fromLocal8Bit(std::strerror(error));
}

} // namespace

RadioBackend::RadioBackend(QObject *parent)
    : Backend(parent)
{
    m_fd = ::open(kDevice, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (m_fd < 0) {
        setAvailable(false, errno == ENOENT
                                ? tr("The kernel offers no radio switches here (/dev/rfkill does not exist).")
                                : tr("/dev/rfkill cannot be read: %1").arg(errnoText(errno)));
        return;
    }
    m_writable = ::access(kDevice, W_OK) == 0;
    m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &RadioBackend::readEvents);
    // The kernel queues an "add" event for every existing switch on open.
    readEvents();
}

RadioBackend::~RadioBackend()
{
    if (m_fd >= 0)
        ::close(m_fd);
}

void RadioBackend::readEvents()
{
    for (;;) {
        rfkill_event event{};
        const ssize_t size = ::read(m_fd, &event, sizeof(event));
        if (size < 0 && errno == EINTR)
            continue;
        if (size < ssize_t(RFKILL_EVENT_SIZE_V1))
            break;      // EAGAIN: nothing more queued
        // Copies: the kernel structure is packed, its fields cannot be
        // passed by reference.
        const quint32 index = event.idx;
        const quint8 type = event.type;
        switch (event.op) {
        case RFKILL_OP_ADD:
        case RFKILL_OP_CHANGE:
            m_radios.insert(index, Radio{type, event.soft != 0, event.hard != 0});
            break;
        case RFKILL_OP_DEL:
            m_radios.remove(index);
            break;
        default:
            break;
        }
    }
    evaluate();
}

void RadioBackend::evaluate()
{
    bool allBlocked = !m_radios.isEmpty();
    for (const Radio &radio : std::as_const(m_radios))
        allBlocked = allBlocked && (radio.soft || radio.hard);
    if (allBlocked != m_airplaneMode) {
        m_airplaneMode = allBlocked;
        Q_EMIT changed();
    }

    if (m_radios.isEmpty())
        setAvailable(false, tr("This computer has no radio (Wi-Fi, Bluetooth or mobile broadband) to switch."));
    else if (!m_writable)
        setAvailable(false, tr("This session is not allowed to switch radios (/dev/rfkill is read-only for it)."));
    else
        setAvailable(true);
}

void RadioBackend::setAirplaneMode(bool on)
{
    if (!isAvailable())
        return;
    if (!on) {
        bool allHard = true;
        for (const Radio &radio : std::as_const(m_radios))
            allHard = allHard && radio.hard;
        if (allHard) {
            Q_EMIT failed(tr("The radios are switched off by a hardware switch."));
            return;
        }
    }
    const int fd = ::open(kDevice, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        Q_EMIT failed(tr("The radios could not be switched: %1").arg(errnoText(errno)));
        return;
    }
    rfkill_event event{};
    event.type = RFKILL_TYPE_ALL;
    event.op = RFKILL_OP_CHANGE_ALL;
    event.soft = on ? 1 : 0;
    const ssize_t written = ::write(fd, &event, sizeof(event));
    const int error = errno;
    ::close(fd);
    if (written != ssize_t(sizeof(event)))
        Q_EMIT failed(tr("The radios could not be switched: %1").arg(errnoText(error)));
    // The new state is not assumed: the kernel reports each switch that
    // changed as an event on the descriptor this back-end reads.
}

QJsonObject RadioBackend::details() const
{
    QJsonObject json{
        {QStringLiteral("device"), QString::fromLatin1(kDevice)},
        {QStringLiteral("readable"), m_fd >= 0},
        {QStringLiteral("writable"), m_writable},
        {QStringLiteral("airplaneMode"), m_airplaneMode},
    };
    QJsonArray radios;
    for (auto it = m_radios.cbegin(); it != m_radios.cend(); ++it) {
        radios.append(QJsonObject{
            {QStringLiteral("index"), qint64(it.key())},
            {QStringLiteral("type"), typeName(it->type)},
            {QStringLiteral("softBlocked"), it->soft},
            {QStringLiteral("hardBlocked"), it->hard},
        });
    }
    json.insert(QStringLiteral("radios"), radios);
    return json;
}

} // namespace StatusModule
