// SPDX-License-Identifier: GPL-3.0-or-later
#include "displaybackend.h"

#include "dumpregistry.h"
#include "networkcommon.h"

#include <QDBusConnection>
#include <QDBusServiceWatcher>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTime>

#include <cmath>
#include <functional>

#ifdef NEXUS_HAVE_KCONFIG
#include <KConfig>
#include <KConfigGroup>
#endif

using namespace NetHw;

NEXUS_CC_DUMP("display", [] {
    DisplayBackend backend;
    waitUntilReady(&backend, 15000);
    return QJsonValue(backend.toJson());
})

namespace {
const QString kDoctor = QStringLiteral("kscreen-doctor");
const QString kKwin = QStringLiteral("org.kde.KWin");
const QString kNightPath = QStringLiteral("/org/kde/KWin/NightLight");
const QString kNightInterface = QStringLiteral("org.kde.KWin.NightLight");
const int kConfirmSeconds = 15;
const int kSnapDistance = 48;       // compositor pixels
const int kMinNightTemperature = 1000;
const int kMaxNightTemperature = 6500;

QString rotationName(int rotation)
{
    switch (rotation) {
    case 2: return QStringLiteral("left");
    case 4: return QStringLiteral("inverted");
    case 8: return QStringLiteral("right");
    default: return QStringLiteral("none");
    }
}

int rotationValue(const QString &name)
{
    if (name == QLatin1String("none")) return 1;
    if (name == QLatin1String("left")) return 2;
    if (name == QLatin1String("inverted")) return 4;
    if (name == QLatin1String("right")) return 8;
    return 0;
}

QString vrrName(int policy)
{
    switch (policy) {
    case 1: return QStringLiteral("always");
    case 2: return QStringLiteral("automatic");
    default: return QStringLiteral("never");
    }
}

int vrrValue(const QString &name)
{
    if (name == QLatin1String("never")) return 0;
    if (name == QLatin1String("always")) return 1;
    if (name == QLatin1String("automatic")) return 2;
    return -1;
}

// "1", "1.25", "1.5" — at most two decimals, no trailing zeros.
QString formatScale(double scale)
{
    QString text = QString::number(scale, 'f', 2);
    while (text.endsWith(QLatin1Char('0')))
        text.chop(1);
    if (text.endsWith(QLatin1Char('.')))
        text.chop(1);
    return text;
}

QString formatRefresh(double refresh)
{
    if (refresh <= 0)
        return QString();
    const double rounded = std::round(refresh);
    if (std::abs(refresh - rounded) < 0.01)
        return DisplayBackend::tr("%1 Hz").arg(int(rounded));
    return DisplayBackend::tr("%1 Hz").arg(QString::number(refresh, 'f', 2));
}

// KScreen::Output::Type
QString typeLabel(int type)
{
    switch (type) {
    case 1: return QStringLiteral("VGA");
    case 2: case 3: case 4: case 5: return QStringLiteral("DVI");
    case 6: return QStringLiteral("HDMI");
    case 7: return DisplayBackend::tr("Built-in display");
    case 8: case 9: case 10: case 11: case 12: case 13: return DisplayBackend::tr("TV");
    case 14: return QStringLiteral("DisplayPort");
    default: return QString();
    }
}

bool touches(const QRect &a, const QRect &b)
{
    const int aRight = a.x() + a.width(), aBottom = a.y() + a.height();
    const int bRight = b.x() + b.width(), bBottom = b.y() + b.height();
    const int sharedRows = qMin(aBottom, bBottom) - qMax(a.y(), b.y());
    const int sharedColumns = qMin(aRight, bRight) - qMax(a.x(), b.x());
    if ((aRight == b.x() || bRight == a.x()) && sharedRows > 0)
        return true;
    return (aBottom == b.y() || bBottom == a.y()) && sharedColumns > 0;
}

int gap(const QRect &a, const QRect &b)
{
    const int dx = qMax(0, qMax(a.x() - (b.x() + b.width()), b.x() - (a.x() + a.width())));
    const int dy = qMax(0, qMax(a.y() - (b.y() + b.height()), b.y() - (a.y() + a.height())));
    return dx + dy;
}

bool overlapsAny(const QRect &rect, const QList<QRect> &others)
{
    for (const QRect &other : others) {
        if (rect.intersects(other))
            return true;
    }
    return false;
}

bool touchesAny(const QRect &rect, const QList<QRect> &others)
{
    for (const QRect &other : others) {
        if (touches(rect, other))
            return true;
    }
    return false;
}

#ifdef NEXUS_HAVE_KCONFIG
// "HHmm" (kwinrc) ↔ "HH:mm" (UI)
QString timeFromConfig(const QString &value, const QString &fallback)
{
    const QTime time = QTime::fromString(value, QStringLiteral("hhmm"));
    return time.isValid() ? time.toString(QStringLiteral("HH:mm")) : fallback;
}

bool writeNightColor(const std::function<void(KConfigGroup &)> &write)
{
    KConfig config(QStringLiteral("kwinrc"), KConfig::NoGlobals);
    KConfigGroup group = config.group(QStringLiteral("NightColor"));
    write(group);
    return config.sync();
}
#endif
} // namespace

bool DisplayBackend::Settings::operator==(const Settings &other) const
{
    return enabled == other.enabled && modeId == other.modeId && qFuzzyCompare(scale, other.scale)
           && rotation == other.rotation && pos == other.pos && priority == other.priority
           && vrrPolicy == other.vrrPolicy && hdr == other.hdr && wcg == other.wcg;
}

DisplayBackend::DisplayBackend(QObject *parent)
    : QObject(parent)
{
    m_model = new RowModel({
        QStringLiteral("name"), QStringLiteral("outputTitle"), QStringLiteral("typeLabel"), QStringLiteral("outputEnabled"),
        QStringLiteral("modeText"), QStringLiteral("resolutions"), QStringLiteral("resolutionIndex"),
        QStringLiteral("refreshRates"), QStringLiteral("refreshIndex"), QStringLiteral("scales"),
        QStringLiteral("scaleIndex"), QStringLiteral("scaleFactor"), QStringLiteral("orientation"),
        QStringLiteral("posX"), QStringLiteral("posY"), QStringLiteral("logicalWidth"), QStringLiteral("logicalHeight"),
        QStringLiteral("primary"), QStringLiteral("vrrCapable"), QStringLiteral("vrrPolicy"),
        QStringLiteral("hdrCapable"), QStringLiteral("hdr"), QStringLiteral("physicalSize"), QStringLiteral("changed"),
    }, QStringLiteral("name"), this);

    m_confirmTimer.setInterval(1000);
    connect(&m_confirmTimer, &QTimer::timeout, this, [this] {
        if (!m_confirming)
            return;
        if (--m_confirmSecondsLeft <= 0) {
            Q_EMIT confirmSecondsLeftChanged();
            finishConfirmation();
            runApply(m_revertArguments, true, true);
            return;
        }
        Q_EMIT confirmSecondsLeftChanged();
    });

    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(600);
    connect(&m_refreshTimer, &QTimer::timeout, this, &DisplayBackend::refresh);

    // Hot-plug and mode changes reach this process as screen changes.
    if (auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        connect(app, &QGuiApplication::screenAdded, this, &DisplayBackend::scheduleRefresh);
        connect(app, &QGuiApplication::screenRemoved, this, &DisplayBackend::scheduleRefresh);
        connect(app, &QGuiApplication::primaryScreenChanged, this, &DisplayBackend::scheduleRefresh);
    }

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.isConnected()) {
        auto *watcher = new QDBusServiceWatcher(kKwin, bus, QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(watcher, &QDBusServiceWatcher::serviceRegistered, this, &DisplayBackend::refresh);
        connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, &DisplayBackend::refresh);
        bus.connect(kKwin, kNightPath, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"),
                    this, SLOT(queryNightLight()));
    }
    refresh();
}

DisplayBackend::~DisplayBackend()
{
    // Unconfirmed settings do not survive the page: put the previous ones back.
    if (m_confirming && !m_revertArguments.isEmpty()) {
        QProcess process;
        process.setProgram(kDoctor);
        process.setArguments(m_revertArguments);
        process.setProcessEnvironment(toolEnvironment());
        process.startDetached();
    }
    if (m_nightPreviewing)
        stopNightPreview();
}

QAbstractItemModel *DisplayBackend::outputs() const
{
    return m_model;
}

void DisplayBackend::setState(const QString &state, const QString &message)
{
    if (state == m_state && message == m_statusMessage)
        return;
    m_state = state;
    m_statusMessage = message;
    Q_EMIT stateChanged();
}

void DisplayBackend::setBusy(bool busy)
{
    if (busy == m_busy)
        return;
    m_busy = busy;
    Q_EMIT busyChanged();
}

void DisplayBackend::updateReady()
{
    if (!m_ready && m_outputsLoaded && m_nightLoaded) {
        m_ready = true;
        Q_EMIT readyChanged();
    }
}

void DisplayBackend::scheduleRefresh()
{
    if (!m_confirming && !m_busy)
        m_refreshTimer.start();
}

// ---- reading the configuration --------------------------------------------------

void DisplayBackend::refresh()
{
    const quint64 generation = ++m_generation;
    const auto unavailable = [this, generation](const QString &state, const QString &message) {
        if (generation != m_generation)
            return;
        m_outputs.clear();
        m_lastJson.clear();
        publish();
        setState(state, message);
        m_kwinRunning = false;
        queryNightLight();
        m_outputsLoaded = true;
        updateReady();
    };

    QDBusConnection bus = QDBusConnection::sessionBus();
    const bool wayland = !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY");
    if (!wayland || !bus.isConnected()) {
        QTimer::singleShot(0, this, [unavailable, wayland] {
            unavailable(QStringLiteral("not-kwin-wayland"),
                        wayland ? DisplayBackend::tr("Displays are configured through KWin, but this session has no D-Bus session bus.")
                                : DisplayBackend::tr("Displays are configured through the KWin Wayland compositor. This session is "
                                                     "not a Wayland session (WAYLAND_DISPLAY is not set), so no outputs are reported."));
        });
        return;
    }

    dbusNameHasOwner(bus, kKwin, this, [this, generation, unavailable](bool owned) {
        if (generation != m_generation)
            return;
        if (!owned) {
            unavailable(QStringLiteral("not-kwin-wayland"),
                        tr("Displays are configured through the KWin Wayland compositor, which is not running in this "
                           "session (org.kde.KWin is not on the session bus), so no outputs are reported."));
            return;
        }
        if (QStandardPaths::findExecutable(kDoctor).isEmpty()) {
            unavailable(QStringLiteral("no-tool"),
                        tr("kscreen-doctor is not installed. It is part of the libkscreen-bin package."));
            return;
        }
        m_kwinRunning = true;
        queryNightLight();
        runTool(this, kDoctor, {QStringLiteral("-j")}, [this, generation](const ToolResult &result) {
            if (generation != m_generation)
                return;
            if (!result.ok()) {
                QString reason = firstErrorLine(QString::fromUtf8(result.err));
                if (reason.isEmpty())
                    reason = result.exitCode == -2 ? tr("it did not answer in time") : tr("exit code %1").arg(result.exitCode);
                m_kwinRunning = true;
                m_outputs.clear();
                m_lastJson.clear();
                publish();
                setState(QStringLiteral("query-failed"), tr("kscreen-doctor could not read the display configuration: %1").arg(reason));
            } else if (result.out != m_lastJson || m_outputs.isEmpty()) {
                m_lastJson = result.out;
                parseConfiguration(result.out);
                publish();
                if (m_outputs.isEmpty())
                    setState(QStringLiteral("no-outputs"), tr("KWin reports no connected display."));
                else
                    setState(QStringLiteral("ok"), QString());
            }
            m_outputsLoaded = true;
            updateReady();
        });
    });
}

void DisplayBackend::parseConfiguration(const QByteArray &json)
{
    QList<Output> outputs;
    const QJsonArray array = QJsonDocument::fromJson(json).object().value(QStringLiteral("outputs")).toArray();
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        if (!object.value(QStringLiteral("connected")).toBool())
            continue;
        Output output;
        output.id = object.value(QStringLiteral("id")).toInt();
        output.name = object.value(QStringLiteral("name")).toString();
        output.type = object.value(QStringLiteral("type")).toInt();
        output.connected = true;
        const QJsonArray modes = object.value(QStringLiteral("modes")).toArray();
        for (const QJsonValue &modeValue : modes) {
            const QJsonObject modeObject = modeValue.toObject();
            const QJsonObject size = modeObject.value(QStringLiteral("size")).toObject();
            Mode mode;
            mode.id = modeObject.value(QStringLiteral("id")).toVariant().toString();
            mode.width = size.value(QStringLiteral("width")).toInt();
            mode.height = size.value(QStringLiteral("height")).toInt();
            mode.refresh = modeObject.value(QStringLiteral("refreshRate")).toDouble(0);
            if (!mode.id.isEmpty() && mode.width > 0 && mode.height > 0)
                output.modes.append(mode);
        }
        const QJsonArray preferred = object.value(QStringLiteral("preferredModes")).toArray();
        for (const QJsonValue &id : preferred)
            output.preferredModes.append(id.toVariant().toString());
        const QJsonObject sizeMm = object.value(QStringLiteral("sizeMM")).toObject();
        output.sizeMm = QSize(sizeMm.value(QStringLiteral("width")).toInt(), sizeMm.value(QStringLiteral("height")).toInt());

        Settings settings;
        settings.enabled = object.value(QStringLiteral("enabled")).toBool();
        settings.modeId = object.value(QStringLiteral("currentModeId")).toVariant().toString();
        settings.scale = object.value(QStringLiteral("scale")).toDouble(1);
        if (settings.scale <= 0)
            settings.scale = 1;
        settings.rotation = object.value(QStringLiteral("rotation")).toInt(1);
        const QJsonObject pos = object.value(QStringLiteral("pos")).toObject();
        settings.pos = QPoint(pos.value(QStringLiteral("x")).toInt(), pos.value(QStringLiteral("y")).toInt());
        settings.priority = settings.enabled ? object.value(QStringLiteral("priority")).toInt() : 0;
        // Only outputs with the capability carry these keys.
        if (object.contains(QStringLiteral("vrrPolicy")))
            settings.vrrPolicy = object.value(QStringLiteral("vrrPolicy")).toInt();
        if (object.contains(QStringLiteral("hdr")))
            settings.hdr = object.value(QStringLiteral("hdr")).toBool() ? 1 : 0;
        if (object.contains(QStringLiteral("wcg")))
            settings.wcg = object.value(QStringLiteral("wcg")).toBool() ? 1 : 0;
        output.current = settings;
        output.pending = settings;
        outputs.append(output);
    }

    // Enabled outputs in priority order (primary first), then the disabled ones.
    std::stable_sort(outputs.begin(), outputs.end(), [](const Output &a, const Output &b) {
        if (a.current.enabled != b.current.enabled)
            return a.current.enabled;
        if (a.current.priority != b.current.priority)
            return a.current.priority < b.current.priority;
        return a.name < b.name;
    });
    m_outputs = outputs;
}

// ---- model -------------------------------------------------------------------------

DisplayBackend::Output *DisplayBackend::find(const QString &name)
{
    for (Output &output : m_outputs) {
        if (output.name == name)
            return &output;
    }
    return nullptr;
}

const DisplayBackend::Mode *DisplayBackend::modeOf(const Output &output, const QString &modeId) const
{
    for (const Mode &mode : output.modes) {
        if (mode.id == modeId)
            return &mode;
    }
    return nullptr;
}

QSize DisplayBackend::logicalSize(const Output &output, const Settings &settings) const
{
    const Mode *mode = modeOf(output, settings.modeId);
    if (!mode)
        return QSize();
    QSize size(qRound(mode->width / settings.scale), qRound(mode->height / settings.scale));
    if (settings.rotation == 2 || settings.rotation == 8)
        size.transpose();
    return size;
}

QRect DisplayBackend::geometry(const Output &output) const
{
    return QRect(output.pending.pos, logicalSize(output, output.pending));
}

int DisplayBackend::enabledCount() const
{
    int count = 0;
    for (const Output &output : m_outputs)
        count += output.pending.enabled ? 1 : 0;
    return count;
}

QRect DisplayBackend::layoutBounds() const
{
    QRect bounds;
    for (const Output &output : m_outputs) {
        if (output.pending.enabled)
            bounds = bounds.united(geometry(output));
    }
    return bounds;
}

bool DisplayBackend::dirty() const
{
    for (const Output &output : m_outputs) {
        if (!(output.pending == output.current))
            return true;
    }
    return false;
}

void DisplayBackend::publish()
{
    QVariantList rows;
    for (const Output &output : std::as_const(m_outputs)) {
        const Settings &settings = output.pending;
        const Mode *mode = modeOf(output, settings.modeId);

        // Distinct resolutions, largest first.
        QList<QSize> sizes;
        for (const Mode &candidate : output.modes) {
            const QSize size(candidate.width, candidate.height);
            if (!sizes.contains(size))
                sizes.append(size);
        }
        std::sort(sizes.begin(), sizes.end(), [](const QSize &a, const QSize &b) {
            const qint64 areaA = qint64(a.width()) * a.height(), areaB = qint64(b.width()) * b.height();
            return areaA != areaB ? areaA > areaB : a.width() > b.width();
        });
        QVariantList resolutions;
        int resolutionIndex = -1;
        for (const QSize &size : std::as_const(sizes)) {
            if (mode && mode->width == size.width() && mode->height == size.height())
                resolutionIndex = int(resolutions.size());
            resolutions.append(QVariantMap{
                {QStringLiteral("text"), QStringLiteral("%1 × %2").arg(size.width()).arg(size.height())},
                {QStringLiteral("width"), size.width()},
                {QStringLiteral("height"), size.height()},
            });
        }

        // Refresh rates of the selected resolution, fastest first.
        QList<Mode> rates;
        if (mode) {
            for (const Mode &candidate : output.modes) {
                if (candidate.width == mode->width && candidate.height == mode->height)
                    rates.append(candidate);
            }
        }
        std::sort(rates.begin(), rates.end(), [](const Mode &a, const Mode &b) { return a.refresh > b.refresh; });
        QVariantList refreshRates;
        int refreshIndex = -1;
        for (const Mode &candidate : std::as_const(rates)) {
            if (mode && candidate.id == mode->id)
                refreshIndex = int(refreshRates.size());
            const QString text = formatRefresh(candidate.refresh);
            refreshRates.append(QVariantMap{
                {QStringLiteral("text"), text.isEmpty() ? tr("Not reported") : text},
                {QStringLiteral("modeId"), candidate.id},
            });
        }

        // 100 % – 300 % in steps of 25 %, plus the current value if it is in between.
        QList<int> percents;
        for (int percent = 100; percent <= 300; percent += 25)
            percents.append(percent);
        const int currentPercent = qRound(settings.scale * 100);
        if (!percents.contains(currentPercent))
            percents.append(currentPercent);
        std::sort(percents.begin(), percents.end());
        QVariantList scales;
        int scaleIndex = -1;
        for (int percent : std::as_const(percents)) {
            if (percent == currentPercent)
                scaleIndex = int(scales.size());
            scales.append(QVariantMap{
                {QStringLiteral("text"), tr("%1 %").arg(percent)},
                {QStringLiteral("value"), percent / 100.0},
            });
        }

        QString modeText;
        if (mode) {
            modeText = QStringLiteral("%1 × %2").arg(mode->width).arg(mode->height);
            const QString refresh = formatRefresh(mode->refresh);
            if (!refresh.isEmpty())
                modeText += QStringLiteral(" @ ") + refresh;
        }
        QString physical;
        if (output.sizeMm.width() > 0 && output.sizeMm.height() > 0) {
            const double inches = std::hypot(double(output.sizeMm.width()), double(output.sizeMm.height())) / 25.4;
            physical = tr("%1″").arg(QString::number(inches, 'f', 1));
        }
        const QString type = typeLabel(output.type);
        const QSize logical = logicalSize(output, settings);

        rows.append(QVariantMap{
            {QStringLiteral("name"), output.name},
            {QStringLiteral("outputTitle"), type.isEmpty() ? output.name : QStringLiteral("%1 — %2").arg(type, output.name)},
            {QStringLiteral("typeLabel"), type},
            {QStringLiteral("outputEnabled"), settings.enabled},
            {QStringLiteral("modeText"), modeText},
            {QStringLiteral("resolutions"), resolutions},
            {QStringLiteral("resolutionIndex"), resolutionIndex},
            {QStringLiteral("refreshRates"), refreshRates},
            {QStringLiteral("refreshIndex"), refreshIndex},
            {QStringLiteral("scales"), scales},
            {QStringLiteral("scaleIndex"), scaleIndex},
            {QStringLiteral("scaleFactor"), settings.scale},
            {QStringLiteral("orientation"), rotationName(settings.rotation)},
            {QStringLiteral("posX"), settings.pos.x()},
            {QStringLiteral("posY"), settings.pos.y()},
            {QStringLiteral("logicalWidth"), logical.width()},
            {QStringLiteral("logicalHeight"), logical.height()},
            {QStringLiteral("primary"), settings.enabled && settings.priority == 1},
            {QStringLiteral("vrrCapable"), settings.vrrPolicy >= 0},
            {QStringLiteral("vrrPolicy"), vrrName(settings.vrrPolicy)},
            {QStringLiteral("hdrCapable"), settings.hdr >= 0},
            {QStringLiteral("hdr"), settings.hdr == 1},
            {QStringLiteral("physicalSize"), physical},
            {QStringLiteral("changed"), !(output.pending == output.current)},
        });
    }
    m_model->setRows(rows);
    Q_EMIT outputsChanged();
}

// ---- arrangement -----------------------------------------------------------------------

QPoint DisplayBackend::nearestFreePosition(const QSize &size, const QPoint &wanted, const QList<QRect> &placed) const
{
    if (placed.isEmpty())
        return QPoint(0, 0);
    const int w = size.width(), h = size.height();
    const auto snap = [](int value, std::initializer_list<int> targets) {
        for (int target : targets) {
            if (qAbs(value - target) <= kSnapDistance)
                return target;
        }
        return value;
    };

    // Every position that puts the rectangle flush against one side of a
    // placed output, keeping a common edge and aligning edges that are close.
    QList<QPoint> candidates;
    for (const QRect &other : placed) {
        const int left = other.x(), top = other.y();
        const int right = left + other.width(), bottom = top + other.height();
        // Neighbours share a real stretch of edge (a quarter of the shorter
        // side), not just a corner the pointer could never cross.
        const int shareY = qMax(1, qMin(h, other.height()) / 4);
        const int shareX = qMax(1, qMin(w, other.width()) / 4);
        const int y = snap(qBound(top - h + shareY, wanted.y(), bottom - shareY), {top, bottom - h, top + (other.height() - h) / 2});
        candidates << QPoint(right, y) << QPoint(left - w, y);
        const int x = snap(qBound(left - w + shareX, wanted.x(), right - shareX), {left, right - w, left + (other.width() - w) / 2});
        candidates << QPoint(x, bottom) << QPoint(x, top - h);
    }

    QPoint best;
    qint64 bestDistance = -1;
    for (const QPoint &candidate : std::as_const(candidates)) {
        if (overlapsAny(QRect(candidate, size), placed))
            continue;
        const qint64 dx = candidate.x() - wanted.x(), dy = candidate.y() - wanted.y();
        const qint64 distance = dx * dx + dy * dy;
        if (bestDistance < 0 || distance < bestDistance) {
            best = candidate;
            bestDistance = distance;
        }
    }
    if (bestDistance >= 0)
        return best;

    // Boxed in on every side: go to the right of everything.
    int right = placed.constFirst().x() + placed.constFirst().width(), top = placed.constFirst().y();
    for (const QRect &other : placed) {
        if (other.x() + other.width() > right) {
            right = other.x() + other.width();
            top = other.y();
        }
    }
    return QPoint(right, top);
}

void DisplayBackend::settle(const QString &movedName, const QPoint &wanted)
{
    QList<Output *> remaining;
    Output *moved = nullptr;
    for (Output &output : m_outputs) {
        if (!output.pending.enabled || !logicalSize(output, output.pending).isValid())
            continue;
        if (!movedName.isEmpty() && output.name == movedName)
            moved = &output;
        else
            remaining.append(&output);
    }
    if (remaining.isEmpty() && !moved)
        return;

    // The primary output (lowest priority number) is the anchor; the others
    // follow nearest-first, each staying put if it still touches the group
    // without overlapping, otherwise moving to the nearest free position.
    std::stable_sort(remaining.begin(), remaining.end(), [](const Output *a, const Output *b) {
        return (a->pending.priority > 0 ? a->pending.priority : 1000) < (b->pending.priority > 0 ? b->pending.priority : 1000);
    });
    QList<QRect> placed;
    if (!remaining.isEmpty())
        placed.append(geometry(*remaining.takeFirst()));
    while (!remaining.isEmpty()) {
        int nearest = 0;
        int nearestGap = -1;
        for (int i = 0; i < remaining.size(); ++i) {
            int distance = -1;
            for (const QRect &other : std::as_const(placed)) {
                const int d = gap(geometry(*remaining.at(i)), other);
                if (distance < 0 || d < distance)
                    distance = d;
            }
            if (nearestGap < 0 || distance < nearestGap) {
                nearestGap = distance;
                nearest = i;
            }
        }
        Output *output = remaining.takeAt(nearest);
        const QRect rect = geometry(*output);
        if (overlapsAny(rect, placed) || !touchesAny(rect, placed))
            output->pending.pos = nearestFreePosition(rect.size(), rect.topLeft(), placed);
        placed.append(geometry(*output));
    }
    if (moved) {
        const QSize size = logicalSize(*moved, moved->pending);
        moved->pending.pos = placed.isEmpty() ? QPoint(0, 0) : nearestFreePosition(size, wanted, placed);
        placed.append(geometry(*moved));
    }

    // The arrangement starts at the origin.
    QRect bounds;
    for (const QRect &rect : std::as_const(placed))
        bounds = bounds.united(rect);
    for (Output &output : m_outputs) {
        if (output.pending.enabled)
            output.pending.pos -= bounds.topLeft();
    }
}

void DisplayBackend::normalisePriorities()
{
    QList<Output *> enabled;
    for (Output &output : m_outputs) {
        if (output.pending.enabled)
            enabled.append(&output);
        else
            output.pending.priority = 0;
    }
    // Keep the existing order; outputs without a priority yet go last.
    std::stable_sort(enabled.begin(), enabled.end(), [](const Output *a, const Output *b) {
        return (a->pending.priority > 0 ? a->pending.priority : 1000) < (b->pending.priority > 0 ? b->pending.priority : 1000);
    });
    int priority = 1;
    for (Output *output : std::as_const(enabled))
        output->pending.priority = priority++;
}

// ---- staged edits --------------------------------------------------------------------------

void DisplayBackend::setOutputEnabled(const QString &name, bool enabled)
{
    Output *output = find(name);
    if (!output || output->pending.enabled == enabled)
        return;
    if (!enabled && enabledCount() <= 1) {
        Q_EMIT failed(tr("At least one display has to stay switched on."));
        publish();
        return;
    }
    const QRect boundsBefore = layoutBounds();
    output->pending.enabled = enabled;
    if (enabled) {
        if (!modeOf(*output, output->pending.modeId)) {
            // Preferred mode, or the largest one.
            QString modeId;
            for (const QString &preferred : std::as_const(output->preferredModes)) {
                if (modeOf(*output, preferred)) {
                    modeId = preferred;
                    break;
                }
            }
            if (modeId.isEmpty()) {
                qint64 bestArea = -1;
                double bestRefresh = -1;
                for (const Mode &mode : std::as_const(output->modes)) {
                    const qint64 area = qint64(mode.width) * mode.height;
                    if (area > bestArea || (area == bestArea && mode.refresh > bestRefresh)) {
                        bestArea = area;
                        bestRefresh = mode.refresh;
                        modeId = mode.id;
                    }
                }
            }
            output->pending.modeId = modeId;
        }
        if (output->current.enabled)
            output->pending.priority = output->current.priority;
        else
            output->pending.priority = 0;       // appended by normalisePriorities()
        // Start to the right of everything; settle() snaps it into place.
        output->pending.pos = QPoint(boundsBefore.x() + boundsBefore.width(), boundsBefore.y());
    }
    normalisePriorities();
    settle();
    publish();
}

void DisplayBackend::setResolution(const QString &name, int width, int height)
{
    Output *output = find(name);
    if (!output)
        return;
    const Mode *current = modeOf(*output, output->pending.modeId);
    const double wantedRefresh = current ? current->refresh : 0;
    const Mode *best = nullptr;
    for (const Mode &mode : std::as_const(output->modes)) {
        if (mode.width != width || mode.height != height)
            continue;
        if (!best) {
            best = &mode;
            continue;
        }
        // Closest to the current refresh rate; the higher rate on a tie.
        const double a = std::abs(mode.refresh - wantedRefresh), b = std::abs(best->refresh - wantedRefresh);
        if (a < b || (qFuzzyCompare(a, b) && mode.refresh > best->refresh))
            best = &mode;
    }
    if (!best) {
        Q_EMIT failed(tr("%1 has no %2 × %3 mode.").arg(name).arg(width).arg(height));
        return;
    }
    setMode(name, best->id);
}

void DisplayBackend::setMode(const QString &name, const QString &modeId)
{
    Output *output = find(name);
    if (!output || !modeOf(*output, modeId) || output->pending.modeId == modeId)
        return;
    output->pending.modeId = modeId;
    settle();
    publish();
}

void DisplayBackend::setScale(const QString &name, double scale)
{
    Output *output = find(name);
    if (!output)
        return;
    scale = qBound(1.0, std::round(scale * 100) / 100, 3.0);
    if (qFuzzyCompare(output->pending.scale, scale))
        return;
    output->pending.scale = scale;
    settle();
    publish();
}

void DisplayBackend::setRotation(const QString &name, const QString &rotation)
{
    Output *output = find(name);
    const int value = rotationValue(rotation);
    if (!output || value == 0 || output->pending.rotation == value)
        return;
    output->pending.rotation = value;
    settle();
    publish();
}

void DisplayBackend::setPrimary(const QString &name)
{
    Output *output = find(name);
    if (!output || !output->pending.enabled || output->pending.priority == 1)
        return;
    for (Output &other : m_outputs) {
        if (other.pending.enabled && other.pending.priority < output->pending.priority)
            ++other.pending.priority;
    }
    output->pending.priority = 1;
    normalisePriorities();
    publish();
}

void DisplayBackend::moveOutput(const QString &name, int x, int y)
{
    Output *output = find(name);
    if (!output || !output->pending.enabled)
        return;
    settle(name, QPoint(x, y));
    publish();
}

void DisplayBackend::setVrrPolicy(const QString &name, const QString &policy)
{
    Output *output = find(name);
    const int value = vrrValue(policy);
    if (!output || value < 0 || output->pending.vrrPolicy < 0 || output->pending.vrrPolicy == value)
        return;
    output->pending.vrrPolicy = value;
    publish();
}

void DisplayBackend::setHdr(const QString &name, bool enabled)
{
    Output *output = find(name);
    if (!output || output->pending.hdr < 0 || (output->pending.hdr == 1) == enabled)
        return;
    output->pending.hdr = enabled ? 1 : 0;
    // KWin drives HDR with wide colour gamut; keep the two together when both exist.
    if (output->pending.wcg >= 0)
        output->pending.wcg = enabled ? 1 : 0;
    publish();
}

void DisplayBackend::discard()
{
    for (Output &output : m_outputs)
        output.pending = output.current;
    publish();
}

// ---- applying -----------------------------------------------------------------------------------

QString DisplayBackend::outputArgument(const Output &output)
{
    // kscreen-doctor splits its arguments at '.', so a name containing one
    // (or an empty name) has to be addressed by id.
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_:+-]+$"));
    return safe.match(output.name).hasMatch() ? output.name : QString::number(output.id);
}

QStringList DisplayBackend::argumentsFor(bool pending) const
{
    QStringList arguments;
    for (const Output &output : m_outputs) {
        const Settings &target = pending ? output.pending : output.current;
        const Settings &other = pending ? output.current : output.pending;
        const QString prefix = QStringLiteral("output.") + outputArgument(output) + QLatin1Char('.');
        if (!target.enabled) {
            if (other.enabled)
                arguments << prefix + QStringLiteral("disable");
            continue;
        }
        arguments << prefix + QStringLiteral("enable");
        if (modeOf(output, target.modeId))
            arguments << prefix + QStringLiteral("mode.") + target.modeId;
        arguments << prefix + QStringLiteral("scale.") + formatScale(target.scale);
        arguments << prefix + QStringLiteral("rotation.") + rotationName(target.rotation);
        arguments << prefix + QStringLiteral("position.%1,%2").arg(target.pos.x()).arg(target.pos.y());
        if (target.priority > 0)
            arguments << prefix + QStringLiteral("priority.%1").arg(target.priority);
        if (target.vrrPolicy >= 0 && target.vrrPolicy != other.vrrPolicy)
            arguments << prefix + QStringLiteral("vrrpolicy.") + vrrName(target.vrrPolicy);
        if (target.hdr >= 0 && target.hdr != other.hdr)
            arguments << prefix + QStringLiteral("hdr.") + (target.hdr == 1 ? QStringLiteral("enable") : QStringLiteral("disable"));
        if (target.wcg >= 0 && target.wcg != other.wcg)
            arguments << prefix + QStringLiteral("wcg.") + (target.wcg == 1 ? QStringLiteral("enable") : QStringLiteral("disable"));
    }
    return arguments;
}

QStringList DisplayBackend::pendingArguments() const
{
    return dirty() ? argumentsFor(true) : QStringList();
}

void DisplayBackend::apply()
{
    if (!available() || m_busy || m_confirming || !dirty())
        return;
    m_revertArguments = argumentsFor(false);
    runApply(argumentsFor(true), false, false);
}

void DisplayBackend::runApply(const QStringList &arguments, bool reverting, bool automatically)
{
    if (arguments.isEmpty())
        return;
    setBusy(true);
    runTool(this, kDoctor, arguments, [this, reverting, automatically](const ToolResult &result) {
        setBusy(false);
        if (!result.ok()) {
            QString reason = firstErrorLine(QString::fromUtf8(result.err));
            if (reason.isEmpty())
                reason = firstErrorLine(QString::fromUtf8(result.out));
            if (reason.isEmpty())
                reason = result.exitCode == -2 ? tr("it did not finish in time") : tr("exit code %1").arg(result.exitCode);
            Q_EMIT failed(reverting ? tr("The previous display settings could not be restored: %1").arg(reason)
                                    : tr("The display settings could not be applied: %1").arg(reason));
        } else if (reverting) {
            Q_EMIT reverted(automatically);
        } else {
            m_confirming = true;
            m_confirmSecondsLeft = kConfirmSeconds;
            m_confirmTimer.start();
            Q_EMIT confirmSecondsLeftChanged();
            Q_EMIT confirmingChanged();
            Q_EMIT applied();
        }
        m_lastJson.clear();
        refresh();      // show what the compositor made of it
    }, 20000);
}

void DisplayBackend::finishConfirmation()
{
    m_confirmTimer.stop();
    if (m_confirming) {
        m_confirming = false;
        Q_EMIT confirmingChanged();
    }
}

void DisplayBackend::keep()
{
    if (!m_confirming)
        return;
    finishConfirmation();
    m_revertArguments.clear();
}

void DisplayBackend::revert()
{
    if (!m_confirming)
        return;
    finishConfirmation();
    runApply(m_revertArguments, true, false);
}

// ---- Night Light ------------------------------------------------------------------------------------

void DisplayBackend::loadNightLightConfig()
{
#ifdef NEXUS_HAVE_KCONFIG
    const KConfig config(QStringLiteral("kwinrc"), KConfig::NoGlobals);
    const KConfigGroup group = config.group(QStringLiteral("NightColor"));
    m_nightEnabled = group.readEntry("Active", false);
    // An enum entry: KConfig stores the choice name, older files its index.
    const QString mode = group.readEntry("Mode", QStringLiteral("Automatic"));
    if (mode == QLatin1String("Constant") || mode == QLatin1String("3"))
        m_nightMode = QStringLiteral("constant");
    else if (mode == QLatin1String("Times") || mode == QLatin1String("2"))
        m_nightMode = QStringLiteral("times");
    else if (mode == QLatin1String("Location") || mode == QLatin1String("1"))
        m_nightMode = QStringLiteral("location");
    else
        m_nightMode = QStringLiteral("automatic");
    m_nightTemperature = qBound(kMinNightTemperature, group.readEntry("NightTemperature", 4500), kMaxNightTemperature);
    m_nightMorning = timeFromConfig(group.readEntry("MorningBeginFixed", QStringLiteral("0600")), QStringLiteral("06:00"));
    m_nightEvening = timeFromConfig(group.readEntry("EveningBeginFixed", QStringLiteral("1800")), QStringLiteral("18:00"));
    m_nightTransition = group.readEntry("TransitionTime", 30);
    m_nightLatitude = group.readEntry("LatitudeFixed", 0.0);
    m_nightLongitude = group.readEntry("LongitudeFixed", 0.0);
#endif
}

void DisplayBackend::queryNightLight()
{
    const auto unavailable = [this](const QString &status) {
        m_nightAvailable = false;
        m_nightStatus = status;
        m_nightSupported = false;
        m_nightRunning = false;
        m_nightCurrentTemperature = 0;
        Q_EMIT nightLightChanged();
        m_nightLoaded = true;
        updateReady();
    };
#ifndef NEXUS_HAVE_KCONFIG
    unavailable(tr("This build has no KConfig support, which Night Light settings need."));
#else
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!m_kwinRunning || !bus.isConnected()) {
        unavailable(tr("KWin is not running in this session."));
        return;
    }
    dbusGetAll(bus, kKwin, kNightPath, kNightInterface, this,
               [this, unavailable](bool ok, const QVariantMap &properties, const QString &error) {
        if (!ok) {
            unavailable(tr("KWin's Night Light plugin is not loaded (%1).").arg(error));
            return;
        }
        loadNightLightConfig();
        m_nightAvailable = true;
        m_nightStatus.clear();
        m_nightSupported = properties.value(QStringLiteral("available")).toBool();
        m_nightRunning = properties.value(QStringLiteral("running")).toBool();
        m_nightCurrentTemperature = properties.value(QStringLiteral("currentTemperature")).toInt();
        Q_EMIT nightLightChanged();
        m_nightLoaded = true;
        updateReady();
    });
#endif
}

void DisplayBackend::nightLightWritten()
{
    loadNightLightConfig();
    Q_EMIT nightLightChanged();
    // KWin reacts to the KConfig notification; read its state back afterwards.
    QTimer::singleShot(400, this, &DisplayBackend::queryNightLight);
}

void DisplayBackend::setNightLightEnabled(bool enabled)
{
#ifdef NEXUS_HAVE_KCONFIG
    if (!m_nightAvailable)
        return;
    const QString mode = m_nightMode;
    const bool ok = writeNightColor([enabled, mode](KConfigGroup &group) {
        group.writeEntry("Active", enabled, KConfig::Notify);
        // "Automatic" needs a location service feeding KWin, which NEXUS does
        // not run; switching Night Light on from here uses fixed times instead.
        if (enabled && mode == QLatin1String("automatic"))
            group.writeEntry("Mode", QStringLiteral("Times"), KConfig::Notify);
    });
    if (!ok)
        Q_EMIT failed(tr("The Night Light settings (kwinrc) could not be written."));
    nightLightWritten();
#else
    Q_UNUSED(enabled)
#endif
}

void DisplayBackend::setNightLightMode(const QString &mode)
{
#ifdef NEXUS_HAVE_KCONFIG
    if (!m_nightAvailable)
        return;
    QString value;
    if (mode == QLatin1String("constant"))
        value = QStringLiteral("Constant");
    else if (mode == QLatin1String("times"))
        value = QStringLiteral("Times");
    else if (mode == QLatin1String("location"))
        value = QStringLiteral("Location");
    else {
        Q_EMIT failed(tr("“%1” is not a Night Light schedule.").arg(mode));
        return;
    }
    if (!writeNightColor([value](KConfigGroup &group) { group.writeEntry("Mode", value, KConfig::Notify); }))
        Q_EMIT failed(tr("The Night Light settings (kwinrc) could not be written."));
    nightLightWritten();
#else
    Q_UNUSED(mode)
#endif
}

void DisplayBackend::setNightTemperature(int kelvin)
{
#ifdef NEXUS_HAVE_KCONFIG
    if (!m_nightAvailable)
        return;
    kelvin = qBound(kMinNightTemperature, kelvin, kMaxNightTemperature);
    stopNightPreview();
    if (!writeNightColor([kelvin](KConfigGroup &group) { group.writeEntry("NightTemperature", kelvin, KConfig::Notify); }))
        Q_EMIT failed(tr("The Night Light settings (kwinrc) could not be written."));
    nightLightWritten();
#else
    Q_UNUSED(kelvin)
#endif
}

bool DisplayBackend::setNightLightTimes(const QString &evening, const QString &morning)
{
#ifdef NEXUS_HAVE_KCONFIG
    if (!m_nightAvailable)
        return false;
    static const QRegularExpression pattern(QStringLiteral("^([01][0-9]|2[0-3]):[0-5][0-9]$"));
    if (!pattern.match(evening).hasMatch() || !pattern.match(morning).hasMatch()) {
        Q_EMIT failed(tr("Times are written as HH:MM, for example 18:30."));
        return false;
    }
    const QTime eveningTime = QTime::fromString(evening, QStringLiteral("HH:mm"));
    const QTime morningTime = QTime::fromString(morning, QStringLiteral("HH:mm"));
    // KWin falls back to 06:00/18:00 when the two transitions would overlap.
    const int apart = qAbs(eveningTime.msecsSinceStartOfDay() - morningTime.msecsSinceStartOfDay()) / 60000;
    if (qMin(apart, 1440 - apart) <= m_nightTransition) {
        Q_EMIT failed(tr("The start and end have to be more than %1 minutes (the transition time) apart.").arg(m_nightTransition));
        return false;
    }
    const QString eveningValue = eveningTime.toString(QStringLiteral("HHmm"));
    const QString morningValue = morningTime.toString(QStringLiteral("HHmm"));
    const bool ok = writeNightColor([eveningValue, morningValue](KConfigGroup &group) {
        group.writeEntry("EveningBeginFixed", eveningValue, KConfig::Notify);
        group.writeEntry("MorningBeginFixed", morningValue, KConfig::Notify);
    });
    if (!ok)
        Q_EMIT failed(tr("The Night Light settings (kwinrc) could not be written."));
    nightLightWritten();
    return ok;
#else
    Q_UNUSED(evening)
    Q_UNUSED(morning)
    return false;
#endif
}

bool DisplayBackend::setNightLightTransitionMinutes(int minutes)
{
#ifdef NEXUS_HAVE_KCONFIG
    if (!m_nightAvailable)
        return false;
    const int apart = qAbs(QTime::fromString(m_nightEvening, QStringLiteral("HH:mm")).msecsSinceStartOfDay()
                           - QTime::fromString(m_nightMorning, QStringLiteral("HH:mm")).msecsSinceStartOfDay()) / 60000;
    if (minutes < 1 || minutes >= qMin(apart, 1440 - apart)) {
        Q_EMIT failed(tr("The transition has to be at least 1 minute and shorter than the time between start and end."));
        return false;
    }
    const bool ok = writeNightColor([minutes](KConfigGroup &group) { group.writeEntry("TransitionTime", minutes, KConfig::Notify); });
    if (!ok)
        Q_EMIT failed(tr("The Night Light settings (kwinrc) could not be written."));
    nightLightWritten();
    return ok;
#else
    Q_UNUSED(minutes)
    return false;
#endif
}

bool DisplayBackend::setNightLightLocation(double latitude, double longitude)
{
#ifdef NEXUS_HAVE_KCONFIG
    if (!m_nightAvailable)
        return false;
    if (!(latitude >= -90 && latitude <= 90) || !(longitude >= -180 && longitude <= 180)) {
        Q_EMIT failed(tr("The latitude is between -90 and 90 degrees, the longitude between -180 and 180."));
        return false;
    }
    const bool ok = writeNightColor([latitude, longitude](KConfigGroup &group) {
        group.writeEntry("LatitudeFixed", latitude, KConfig::Notify);
        group.writeEntry("LongitudeFixed", longitude, KConfig::Notify);
    });
    if (!ok)
        Q_EMIT failed(tr("The Night Light settings (kwinrc) could not be written."));
    nightLightWritten();
    return ok;
#else
    Q_UNUSED(latitude)
    Q_UNUSED(longitude)
    return false;
#endif
}

void DisplayBackend::previewNightTemperature(int kelvin)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!m_nightAvailable || !bus.isConnected())
        return;
    kelvin = qBound(kMinNightTemperature, kelvin, kMaxNightTemperature);
    m_nightPreviewing = true;
    bus.send(dbusMethod(kKwin, kNightPath, kNightInterface, QStringLiteral("preview"), {QVariant::fromValue(uint(kelvin))}));
}

void DisplayBackend::stopNightPreview()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!m_nightPreviewing || !bus.isConnected())
        return;
    m_nightPreviewing = false;
    bus.send(dbusMethod(kKwin, kNightPath, kNightInterface, QStringLiteral("stopPreview")));
}

// ---- dump --------------------------------------------------------------------------------------------

QJsonObject DisplayBackend::toJson() const
{
    QJsonArray outputs;
    for (const Output &output : m_outputs) {
        const Settings &settings = output.current;
        const Mode *mode = modeOf(output, settings.modeId);
        QJsonArray modes;
        for (const Mode &candidate : output.modes) {
            modes.append(QJsonObject{
                {QStringLiteral("id"), candidate.id},
                {QStringLiteral("width"), candidate.width},
                {QStringLiteral("height"), candidate.height},
                {QStringLiteral("refreshRate"), candidate.refresh},
            });
        }
        QJsonObject object{
            {QStringLiteral("id"), output.id},
            {QStringLiteral("name"), output.name},
            {QStringLiteral("type"), output.type},
            {QStringLiteral("connected"), output.connected},
            {QStringLiteral("enabled"), settings.enabled},
            {QStringLiteral("primary"), settings.enabled && settings.priority == 1},
            {QStringLiteral("priority"), settings.priority},
            {QStringLiteral("currentModeId"), settings.modeId},
            {QStringLiteral("scale"), settings.scale},
            {QStringLiteral("rotation"), rotationName(settings.rotation)},
            {QStringLiteral("x"), settings.pos.x()},
            {QStringLiteral("y"), settings.pos.y()},
            {QStringLiteral("modes"), modes},
            {QStringLiteral("widthMm"), output.sizeMm.width()},
            {QStringLiteral("heightMm"), output.sizeMm.height()},
            {QStringLiteral("vrrCapable"), settings.vrrPolicy >= 0},
            {QStringLiteral("hdrCapable"), settings.hdr >= 0},
        };
        if (mode) {
            object.insert(QStringLiteral("width"), mode->width);
            object.insert(QStringLiteral("height"), mode->height);
            object.insert(QStringLiteral("refreshRate"), mode->refresh);
        }
        if (settings.vrrPolicy >= 0)
            object.insert(QStringLiteral("vrrPolicy"), vrrName(settings.vrrPolicy));
        if (settings.hdr >= 0)
            object.insert(QStringLiteral("hdr"), settings.hdr == 1);
        outputs.append(object);
    }
    return {
        {QStringLiteral("state"), m_state},
        {QStringLiteral("available"), available()},
        {QStringLiteral("statusMessage"), m_statusMessage},
        {QStringLiteral("outputs"), outputs},
        {QStringLiteral("nightLight"), QJsonObject{
            {QStringLiteral("available"), m_nightAvailable},
            {QStringLiteral("status"), m_nightStatus},
            {QStringLiteral("supported"), m_nightSupported},
            {QStringLiteral("enabled"), m_nightEnabled},
            {QStringLiteral("mode"), m_nightMode},
            {QStringLiteral("nightTemperature"), m_nightTemperature},
            {QStringLiteral("evening"), m_nightEvening},
            {QStringLiteral("morning"), m_nightMorning},
            {QStringLiteral("transitionMinutes"), m_nightTransition},
            {QStringLiteral("latitude"), m_nightLatitude},
            {QStringLiteral("longitude"), m_nightLongitude},
            {QStringLiteral("running"), m_nightRunning},
            {QStringLiteral("currentTemperature"), m_nightCurrentTemperature},
        }},
    };
}
