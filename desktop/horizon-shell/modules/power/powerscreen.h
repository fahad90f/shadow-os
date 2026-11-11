// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "qwayland-dpms.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QtWaylandClient/QWaylandClientExtension>

class QScreen;

namespace PowerModule {

class DpmsManager : public QWaylandClientExtensionTemplate<DpmsManager>, public QtWayland::org_kde_kwin_dpms_manager
{
    Q_OBJECT
public:
    DpmsManager();
};

// One output's DPMS state.
class DpmsOutput : public QObject, public QtWayland::org_kde_kwin_dpms
{
    Q_OBJECT
public:
    DpmsOutput(struct ::org_kde_kwin_dpms *object, QObject *parent);
    ~DpmsOutput() override;

    bool supported() const { return m_supported; }
    // org_kde_kwin_dpms.mode: 0 on, 1 standby, 2 suspend, 3 off; -1 until reported.
    int mode() const { return m_mode; }

Q_SIGNALS:
    void changed();

protected:
    void org_kde_kwin_dpms_supported(uint32_t supported) override;
    void org_kde_kwin_dpms_mode(uint32_t mode) override;
    void org_kde_kwin_dpms_done() override;

private:
    bool m_supported = false, m_pendingSupported = false;
    int m_mode = -1, m_pendingMode = -1;
};

// Switching the screens off and on.
//
// Through KWin's Wayland protocol org_kde_kwin_dpms (plasma-wayland-protocols,
// dpms.xml), the interface kscreen-doctor --dpms uses as well: one
// org_kde_kwin_dpms object per wl_output, "set" with mode off/on. KWin wakes
// the outputs by itself on the next input event. Outputs that do not support
// DPMS (KWin says so per output) are left alone, and without the protocol
// (another compositor, no Wayland) the screens are not switched off at all.
class Screens : public QObject
{
    Q_OBJECT

public:
    explicit Screens(QObject *parent = nullptr);

    // The compositor offers the protocol.
    bool available() const;
    // ... and at least one output can be switched.
    bool supported() const;
    // Every output that can be switched is off.
    bool off() const;

    // Returns false when nothing could be switched.
    bool setOff(bool off);

    QJsonObject toJson() const;

Q_SIGNALS:
    void changed();

private:
    void sync();

    DpmsManager *m_manager = nullptr;
    QHash<QScreen *, DpmsOutput *> m_outputs;
};

} // namespace PowerModule
