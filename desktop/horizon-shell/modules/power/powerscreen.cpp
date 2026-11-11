// SPDX-License-Identifier: GPL-3.0-or-later
#include "powerscreen.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QScreen>

namespace PowerModule {

DpmsManager::DpmsManager()
    : QWaylandClientExtensionTemplate<DpmsManager>(1)
{
    initialize();
}

DpmsOutput::DpmsOutput(struct ::org_kde_kwin_dpms *object, QObject *parent)
    : QObject(parent), QtWayland::org_kde_kwin_dpms(object)
{
}

DpmsOutput::~DpmsOutput()
{
    if (isInitialized())
        release();
}

void DpmsOutput::org_kde_kwin_dpms_supported(uint32_t supported) { m_pendingSupported = supported != 0; }
void DpmsOutput::org_kde_kwin_dpms_mode(uint32_t mode) { m_pendingMode = int(mode); }

void DpmsOutput::org_kde_kwin_dpms_done()
{
    if (m_supported == m_pendingSupported && m_mode == m_pendingMode)
        return;
    m_supported = m_pendingSupported;
    m_mode = m_pendingMode;
    Q_EMIT changed();
}

Screens::Screens(QObject *parent)
    : QObject(parent)
{
    if (QGuiApplication::platformName() != QLatin1String("wayland"))
        return;
    m_manager = new DpmsManager;
    m_manager->setParent(this);
    connect(m_manager, &DpmsManager::activeChanged, this, &Screens::sync);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, &Screens::sync);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, &Screens::sync);
    sync();
}

void Screens::sync()
{
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (auto it = m_outputs.begin(); it != m_outputs.end();) {
        if (!m_manager->isActive() || !screens.contains(it.key())) {
            delete it.value();
            it = m_outputs.erase(it);
        } else {
            ++it;
        }
    }
    if (m_manager->isActive()) {
        for (QScreen *screen : screens) {
            if (m_outputs.contains(screen))
                continue;
            const auto *native = screen->nativeInterface<QNativeInterface::QWaylandScreen>();
            if (!native || !native->output())
                continue;       // Qt's placeholder screen while no output exists
            auto *output = new DpmsOutput(m_manager->get(native->output()), this);
            connect(output, &DpmsOutput::changed, this, &Screens::changed);
            m_outputs.insert(screen, output);
        }
    }
    Q_EMIT changed();
}

bool Screens::available() const
{
    return m_manager && m_manager->isActive();
}

bool Screens::supported() const
{
    for (const DpmsOutput *output : m_outputs) {
        if (output->supported())
            return true;
    }
    return false;
}

bool Screens::off() const
{
    bool any = false;
    for (const DpmsOutput *output : m_outputs) {
        if (!output->supported())
            continue;
        if (output->mode() != 3)
            return false;
        any = true;
    }
    return any;
}

bool Screens::setOff(bool off)
{
    bool any = false;
    for (DpmsOutput *output : std::as_const(m_outputs)) {
        if (!output->supported())
            continue;
        output->set(off ? 3 : 0);
        any = true;
    }
    return any;
}

QJsonObject Screens::toJson() const
{
    static const char *const names[] = {"on", "standby", "suspend", "off"};
    QJsonArray outputs;
    for (auto it = m_outputs.constBegin(); it != m_outputs.constEnd(); ++it) {
        const int mode = it.value()->mode();
        outputs.append(QJsonObject{{QStringLiteral("name"), it.key()->name()},
                                   {QStringLiteral("dpmsSupported"), it.value()->supported()},
                                   {QStringLiteral("mode"), mode >= 0 && mode <= 3 ? QLatin1String(names[mode]) : QLatin1String("unknown")}});
    }
    QJsonObject o{{QStringLiteral("protocolAvailable"), available()},
                  {QStringLiteral("canSwitchOff"), supported()},
                  {QStringLiteral("off"), off()},
                  {QStringLiteral("outputs"), outputs}};
    if (!available())
        o.insert(QStringLiteral("unavailableReason"),
                 m_manager ? QStringLiteral("The compositor does not offer org_kde_kwin_dpms_manager.")
                           : QStringLiteral("Not a Wayland session."));
    else if (!supported())
        o.insert(QStringLiteral("unavailableReason"), QStringLiteral("No output supports DPMS."));
    return o;
}

} // namespace PowerModule
