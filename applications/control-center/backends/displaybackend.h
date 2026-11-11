// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractItemModel>
#include <QJsonObject>
#include <QObject>
#include <QPoint>
#include <QQmlEngine>
#include <QRect>
#include <QSize>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

namespace NetHw {
class RowModel;
}

// Displays page: outputs of the KWin Wayland session and Night Light.
//
// Outputs
//   kscreen-doctor -j        the output configuration as JSON (name, enabled,
//                            connected, modes, currentModeId, scale, rotation,
//                            pos, priority; vrrPolicy / hdr / wcg only for
//                            outputs that have the capability)
//   kscreen-doctor output.<name>.<setting>…
//                            one invocation with the complete wanted state of
//                            every output — mode, scale, rotation, position,
//                            priority, enable/disable (+ vrrpolicy, hdr, wcg
//                            when changed) — so the change is applied atomically
// The tool only gives real data inside a KWin Wayland session (its other
// back-ends would describe an X server or this process's own offscreen
// surface), so everything is "not-kwin-wayland" unless WAYLAND_DISPLAY is
// set and org.kde.KWin is on the session bus.
//
// Edits are staged: the setters change a pending copy, apply() sends it, and
// the page then asks "Keep these settings?" — revert() (or 15 seconds
// without an answer, or closing the page) restores the previous state.
//
// Night Light (KWin's nightlight plugin; only with KConfig support)
//   ~/.config/kwinrc [NightColor]   Active, Mode (Constant | Times | Location |
//                            Automatic), NightTemperature, MorningBeginFixed and
//                            EveningBeginFixed ("HHmm"), TransitionTime (min),
//                            LatitudeFixed, LongitudeFixed — written with
//                            KConfig::Notify, which KWin watches
//   org.kde.KWin /org/kde/KWin/NightLight (org.kde.KWin.NightLight)
//                            available, running, currentTemperature;
//                            preview(uint) / stopPreview() while dragging
class DisplayBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

    Q_PROPERTY(QAbstractItemModel *outputs READ outputs CONSTANT)
    Q_PROPERTY(int outputCount READ outputCount NOTIFY outputsChanged)
    Q_PROPERTY(int enabledCount READ enabledCount NOTIFY outputsChanged)
    Q_PROPERTY(QRect layoutBounds READ layoutBounds NOTIFY outputsChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY outputsChanged)
    Q_PROPERTY(QStringList pendingArguments READ pendingArguments NOTIFY outputsChanged)

    Q_PROPERTY(bool confirming READ confirming NOTIFY confirmingChanged)
    Q_PROPERTY(int confirmSecondsLeft READ confirmSecondsLeft NOTIFY confirmSecondsLeftChanged)

    Q_PROPERTY(bool nightLightAvailable READ nightLightAvailable NOTIFY nightLightChanged)
    Q_PROPERTY(QString nightLightStatus READ nightLightStatus NOTIFY nightLightChanged)
    Q_PROPERTY(bool nightLightSupported READ nightLightSupported NOTIFY nightLightChanged)
    Q_PROPERTY(bool nightLightEnabled READ nightLightEnabled NOTIFY nightLightChanged)
    Q_PROPERTY(QString nightLightMode READ nightLightMode NOTIFY nightLightChanged)
    Q_PROPERTY(int nightTemperature READ nightTemperature NOTIFY nightLightChanged)
    Q_PROPERTY(QString nightLightEvening READ nightLightEvening NOTIFY nightLightChanged)
    Q_PROPERTY(QString nightLightMorning READ nightLightMorning NOTIFY nightLightChanged)
    Q_PROPERTY(int nightLightTransitionMinutes READ nightLightTransitionMinutes NOTIFY nightLightChanged)
    Q_PROPERTY(double nightLightLatitude READ nightLightLatitude NOTIFY nightLightChanged)
    Q_PROPERTY(double nightLightLongitude READ nightLightLongitude NOTIFY nightLightChanged)
    Q_PROPERTY(bool nightLightRunning READ nightLightRunning NOTIFY nightLightChanged)
    Q_PROPERTY(int nightLightCurrentTemperature READ nightLightCurrentTemperature NOTIFY nightLightChanged)

public:
    explicit DisplayBackend(QObject *parent = nullptr);
    ~DisplayBackend() override;

    bool ready() const { return m_ready; }
    // "ok", "not-kwin-wayland", "no-tool", "query-failed" or "no-outputs".
    QString state() const { return m_state; }
    bool available() const { return m_state == QLatin1String("ok"); }
    QString statusMessage() const { return m_statusMessage; }
    bool busy() const { return m_busy; }

    QAbstractItemModel *outputs() const;
    int outputCount() const { return int(m_outputs.size()); }
    int enabledCount() const;
    // Bounding box of the enabled outputs in the pending arrangement.
    QRect layoutBounds() const;
    bool dirty() const;
    // The kscreen-doctor arguments apply() would use right now.
    QStringList pendingArguments() const;

    bool confirming() const { return m_confirming; }
    int confirmSecondsLeft() const { return m_confirmSecondsLeft; }

    bool nightLightAvailable() const { return m_nightAvailable; }
    QString nightLightStatus() const { return m_nightStatus; }
    bool nightLightSupported() const { return m_nightSupported; }
    bool nightLightEnabled() const { return m_nightEnabled; }
    QString nightLightMode() const { return m_nightMode; }
    int nightTemperature() const { return m_nightTemperature; }
    QString nightLightEvening() const { return m_nightEvening; }
    QString nightLightMorning() const { return m_nightMorning; }
    int nightLightTransitionMinutes() const { return m_nightTransition; }
    double nightLightLatitude() const { return m_nightLatitude; }
    double nightLightLongitude() const { return m_nightLongitude; }
    bool nightLightRunning() const { return m_nightRunning; }
    int nightLightCurrentTemperature() const { return m_nightCurrentTemperature; }

    // ---- staged edits (by output name) -----------------------------------
    Q_INVOKABLE void setOutputEnabled(const QString &name, bool enabled);
    Q_INVOKABLE void setResolution(const QString &name, int width, int height);
    Q_INVOKABLE void setMode(const QString &name, const QString &modeId);
    Q_INVOKABLE void setScale(const QString &name, double scale);
    // "none", "left", "inverted" or "right" (kscreen-doctor's names).
    Q_INVOKABLE void setRotation(const QString &name, const QString &rotation);
    Q_INVOKABLE void setPrimary(const QString &name);
    // Drop an output at (x, y) in compositor coordinates; it snaps to the
    // nearest free position that touches another output.
    Q_INVOKABLE void moveOutput(const QString &name, int x, int y);
    Q_INVOKABLE void setVrrPolicy(const QString &name, const QString &policy);
    Q_INVOKABLE void setHdr(const QString &name, bool enabled);

    Q_INVOKABLE void apply();
    Q_INVOKABLE void discard();
    Q_INVOKABLE void keep();
    Q_INVOKABLE void revert();

    // ---- Night Light ---------------------------------------------------------
    Q_INVOKABLE void setNightLightEnabled(bool enabled);
    // "constant", "times" or "location".
    Q_INVOKABLE void setNightLightMode(const QString &mode);
    Q_INVOKABLE void setNightTemperature(int kelvin);
    // "HH:mm" each: when the night colour begins and when it ends.
    Q_INVOKABLE bool setNightLightTimes(const QString &evening, const QString &morning);
    Q_INVOKABLE bool setNightLightTransitionMinutes(int minutes);
    Q_INVOKABLE bool setNightLightLocation(double latitude, double longitude);
    Q_INVOKABLE void previewNightTemperature(int kelvin);
    Q_INVOKABLE void stopNightPreview();

    QJsonObject toJson() const;

public Q_SLOTS:
    void refresh();

Q_SIGNALS:
    void readyChanged();
    void stateChanged();
    void busyChanged();
    void outputsChanged();
    void confirmingChanged();
    void confirmSecondsLeftChanged();
    void nightLightChanged();
    void failed(const QString &message);
    void applied();
    void reverted(bool automatically);

private Q_SLOTS:
    void queryNightLight();

private:
    struct Mode
    {
        QString id;
        int width = 0;
        int height = 0;
        double refresh = 0;
    };
    struct Settings
    {
        bool enabled = false;
        QString modeId;
        double scale = 1;
        int rotation = 1;       // KScreen: 1 none, 2 left, 4 inverted, 8 right
        QPoint pos;
        int priority = 0;       // 1 = primary; 0 = disabled
        int vrrPolicy = -1;     // -1 not capable; 0 never, 1 always, 2 automatic
        int hdr = -1;           // -1 not capable; 0 off, 1 on
        int wcg = -1;
        bool operator==(const Settings &other) const;
    };
    struct Output
    {
        int id = 0;
        QString name;
        int type = 0;
        bool connected = false;
        QList<Mode> modes;
        QStringList preferredModes;
        QSize sizeMm;
        Settings current;
        Settings pending;
    };

    void setState(const QString &state, const QString &message);
    void setBusy(bool busy);
    void parseConfiguration(const QByteArray &json);
    void publish();
    Output *find(const QString &name);
    const Mode *modeOf(const Output &output, const QString &modeId) const;
    QSize logicalSize(const Output &output, const Settings &settings) const;
    QRect geometry(const Output &output) const;
    void settle(const QString &movedName = QString(), const QPoint &wanted = QPoint());
    QPoint nearestFreePosition(const QSize &size, const QPoint &wanted, const QList<QRect> &placed) const;
    void normalisePriorities();
    QStringList argumentsFor(bool pending) const;
    static QString outputArgument(const Output &output);
    void runApply(const QStringList &arguments, bool reverting, bool automatically);
    void finishConfirmation();
    void loadNightLightConfig();
    void nightLightWritten();
    void scheduleRefresh();
    void updateReady();

    bool m_ready = false;
    bool m_outputsLoaded = false;
    bool m_nightLoaded = false;
    QString m_state;
    QString m_statusMessage;
    bool m_busy = false;
    bool m_kwinRunning = false;
    quint64 m_generation = 0;

    QList<Output> m_outputs;
    NetHw::RowModel *m_model = nullptr;

    bool m_confirming = false;
    int m_confirmSecondsLeft = 0;
    QTimer m_confirmTimer;
    QTimer m_refreshTimer;
    QStringList m_revertArguments;
    QByteArray m_lastJson;

    bool m_nightAvailable = false;
    QString m_nightStatus;
    bool m_nightSupported = false;
    bool m_nightEnabled = false;
    QString m_nightMode = QStringLiteral("automatic");
    int m_nightTemperature = 4500;
    QString m_nightEvening = QStringLiteral("18:00");
    QString m_nightMorning = QStringLiteral("06:00");
    int m_nightTransition = 30;
    double m_nightLatitude = 0;
    double m_nightLongitude = 0;
    bool m_nightRunning = false;
    int m_nightCurrentTemperature = 0;
    bool m_nightPreviewing = false;
};
