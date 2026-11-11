// SPDX-License-Identifier: GPL-3.0-or-later
#include "bluetoothbackend.h"

#include "dumpregistry.h"
#include "networkcommon.h"

#include <QDBusConnection>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <functional>

#ifdef NEXUS_HAVE_BLUEZQT
#include <BluezQt/Adapter>
#include <BluezQt/Agent>
#include <BluezQt/Device>
#include <BluezQt/InitManagerJob>
#include <BluezQt/Manager>
#include <BluezQt/PendingCall>
#include <QDBusObjectPath>
#endif

using namespace NetHw;

NEXUS_CC_DUMP("bluetooth", [] {
    BluetoothBackend backend;
    waitUntilReady(&backend);
    return QJsonValue(backend.toJson());
})

namespace {
const QStringList kDeviceRoles = {
    QStringLiteral("ubi"), QStringLiteral("address"), QStringLiteral("name"), QStringLiteral("kind"),
    QStringLiteral("deviceIcon"), QStringLiteral("paired"), QStringLiteral("trusted"), QStringLiteral("connected"),
    QStringLiteral("battery"), QStringLiteral("rssi"), QStringLiteral("busy"),
};
const int kScanSeconds = 30;

#ifdef NEXUS_HAVE_BLUEZQT
const QString kBluez = QStringLiteral("org.bluez");
const QString kBattery = QStringLiteral("org.bluez.Battery1");
const QString kProperties = QStringLiteral("org.freedesktop.DBus.Properties");
const int kInvalidRssi = -32768;

QString kindLabel(BluezQt::Device::Type type)
{
    using BluezQt::Device;
    switch (type) {
    case Device::Phone: return BluetoothBackend::tr("Phone");
    case Device::Modem: return BluetoothBackend::tr("Modem");
    case Device::Computer: return BluetoothBackend::tr("Computer");
    case Device::Network: return BluetoothBackend::tr("Network device");
    case Device::Headset: return BluetoothBackend::tr("Headset");
    case Device::Headphones: return BluetoothBackend::tr("Headphones");
    case Device::AudioVideo: return BluetoothBackend::tr("Audio device");
    case Device::Keyboard: return BluetoothBackend::tr("Keyboard");
    case Device::Mouse: return BluetoothBackend::tr("Mouse");
    case Device::Joypad: return BluetoothBackend::tr("Game controller");
    case Device::Tablet: return BluetoothBackend::tr("Graphics tablet");
    case Device::Peripheral: return BluetoothBackend::tr("Input device");
    case Device::Camera: return BluetoothBackend::tr("Camera");
    case Device::Printer: return BluetoothBackend::tr("Printer");
    case Device::Imaging: return BluetoothBackend::tr("Imaging device");
    case Device::Wearable: return BluetoothBackend::tr("Wearable");
    case Device::Toy: return BluetoothBackend::tr("Toy");
    case Device::Health: return BluetoothBackend::tr("Health device");
    default: return BluetoothBackend::tr("Bluetooth device");
    }
}

// Glyphs of the NEXUS symbolic set; types without a fitting glyph use the
// Bluetooth mark.
QString kindIcon(BluezQt::Device::Type type)
{
    using BluezQt::Device;
    switch (type) {
    case Device::Computer: return QStringLiteral("laptop");
    case Device::Network: case Device::Modem: return QStringLiteral("network");
    case Device::Headset: case Device::Headphones: case Device::AudioVideo: return QStringLiteral("music");
    case Device::Keyboard: return QStringLiteral("keyboard");
    case Device::Mouse: return QStringLiteral("mouse");
    case Device::Camera: case Device::Imaging: return QStringLiteral("camera");
    case Device::Printer: return QStringLiteral("printer");
    default: return QStringLiteral("bluetooth");
    }
}

QString callError(int error, const QString &text)
{
    using BluezQt::PendingCall;
    switch (error) {
    case PendingCall::NotReady: return BluetoothBackend::tr("Bluetooth is switched off or not ready.");
    case PendingCall::AuthenticationCanceled: return BluetoothBackend::tr("Pairing was cancelled.");
    case PendingCall::AuthenticationFailed: return BluetoothBackend::tr("The device refused, or the codes did not match.");
    case PendingCall::AuthenticationRejected: return BluetoothBackend::tr("The device rejected the request.");
    case PendingCall::AuthenticationTimeout: return BluetoothBackend::tr("The device did not answer in time.");
    case PendingCall::ConnectionAttemptFailed: return BluetoothBackend::tr("The device could not be reached. Is it switched on and nearby?");
    case PendingCall::InProgress: return BluetoothBackend::tr("Another operation on this device is still in progress.");
    case PendingCall::NotAuthorized: return BluetoothBackend::tr("This user is not allowed to do that.");
    default: return text.isEmpty() ? BluetoothBackend::tr("Unknown error %1").arg(error) : text;
    }
}

template<typename Done>
void whenFinished(BluezQt::PendingCall *call, QObject *context, Done done)
{
    QObject::connect(call, &BluezQt::PendingCall::finished, context, [done](BluezQt::PendingCall *finished) {
        done(finished->error(), finished->errorText());
    });
}

// BlueZ uses the address with dashes as the name of a device that has none.
bool hasRealName(const QString &name)
{
    static const QRegularExpression addressLike(QStringLiteral("^([0-9A-Fa-f]{2}[-:]){5}[0-9A-Fa-f]{2}$"));
    return !name.isEmpty() && !addressLike.match(name).hasMatch();
}
#endif
} // namespace

// ---- private part ---------------------------------------------------------------

#ifdef NEXUS_HAVE_BLUEZQT
class PairingAgent;
#endif

class BluetoothBackend::Private
{
public:
    explicit Private(BluetoothBackend *backend) : q(backend) {}

    BluetoothBackend *q;
    RowModel *paired = nullptr;
    RowModel *available = nullptr;
    QTimer rebuildTimer;
    QTimer scanTimer;
    QHash<QString, int> batteries;      // ubi → percent
    QSet<QString> batteryQueried;
    QHash<QString, QString> busy;       // ubi → what is going on
    RfkillState rfkill;
    std::function<void(bool, const QString &)> answer;

    void showPrompt(const QVariantMap &prompt, std::function<void(bool, const QString &)> answerFunction)
    {
        answer = std::move(answerFunction);
        q->m_prompt = prompt;
        Q_EMIT q->promptChanged();
    }

    void clearPrompt()
    {
        answer = nullptr;
        if (!q->m_prompt.isEmpty()) {
            q->m_prompt.clear();
            Q_EMIT q->promptChanged();
        }
    }

    void setBusy(const QString &ubi, const QString &activity)
    {
        if (activity.isEmpty())
            busy.remove(ubi);
        else
            busy.insert(ubi, activity);
        q->scheduleRebuild();
    }

#ifdef NEXUS_HAVE_BLUEZQT
    BluezQt::Manager *manager = nullptr;
    PairingAgent *agent = nullptr;
    bool initialized = false;
    bool agentRegistered = false;
    bool powerOnWhenAdapterAppears = false;
    bool startedScan = false;

    BluezQt::AdapterPtr adapter() const
    {
        if (!manager || !manager->isOperational())
            return BluezQt::AdapterPtr();
        // Prefer a powered adapter, otherwise the first one.
        if (BluezQt::AdapterPtr usable = manager->usableAdapter())
            return usable;
        const QList<BluezQt::AdapterPtr> adapters = manager->adapters();
        return adapters.isEmpty() ? BluezQt::AdapterPtr() : adapters.constFirst();
    }

    BluezQt::DevicePtr device(const QString &ubi) const
    {
        return manager && manager->isOperational() ? manager->deviceForUbi(ubi) : BluezQt::DevicePtr();
    }

    void registerAgent();
    void queryBattery(const QString &ubi);
#endif
};

#ifdef NEXUS_HAVE_BLUEZQT
// Answers BlueZ's questions during a pairing started from this page by
// putting them in front of the user (BluetoothBackend::prompt).
class PairingAgent : public BluezQt::Agent
{
public:
    PairingAgent(BluetoothBackend::Private *owner, const QString &path, QObject *parent)
        : BluezQt::Agent(parent), d(owner), m_path(path)
    {
    }

    QDBusObjectPath objectPath() const override { return QDBusObjectPath(m_path); }

    void requestPinCode(BluezQt::DevicePtr device, const BluezQt::Request<QString> &request) override
    {
        d->showPrompt(describe(device, QStringLiteral("pin")), [request](bool accept, const QString &text) {
            if (accept && !text.isEmpty() && text.size() <= 16)
                request.accept(text);
            else
                request.reject();
        });
    }

    void displayPinCode(BluezQt::DevicePtr device, const QString &pinCode) override
    {
        QVariantMap prompt = describe(device, QStringLiteral("display-pin"));
        prompt.insert(QStringLiteral("code"), pinCode);
        d->showPrompt(prompt, [device](bool accept, const QString &) {
            if (!accept && device)
                device->cancelPairing();
        });
    }

    void requestPasskey(BluezQt::DevicePtr device, const BluezQt::Request<quint32> &request) override
    {
        d->showPrompt(describe(device, QStringLiteral("passkey")), [request](bool accept, const QString &text) {
            bool ok = false;
            const uint passkey = text.toUInt(&ok);
            if (accept && ok && passkey <= 999999)
                request.accept(passkey);
            else
                request.reject();
        });
    }

    void displayPasskey(BluezQt::DevicePtr device, const QString &passkey, const QString &entered) override
    {
        QVariantMap prompt = describe(device, QStringLiteral("display-passkey"));
        prompt.insert(QStringLiteral("code"), passkey);
        prompt.insert(QStringLiteral("entered"), entered.toInt());
        d->showPrompt(prompt, [device](bool accept, const QString &) {
            if (!accept && device)
                device->cancelPairing();
        });
    }

    void requestConfirmation(BluezQt::DevicePtr device, const QString &passkey, const BluezQt::Request<> &request) override
    {
        QVariantMap prompt = describe(device, QStringLiteral("confirm"));
        prompt.insert(QStringLiteral("code"), passkey);
        d->showPrompt(prompt, [request](bool accept, const QString &) {
            if (accept)
                request.accept();
            else
                request.reject();
        });
    }

    void requestAuthorization(BluezQt::DevicePtr device, const BluezQt::Request<> &request) override
    {
        d->showPrompt(describe(device, QStringLiteral("authorize")), [request](bool accept, const QString &) {
            if (accept)
                request.accept();
            else
                request.reject();
        });
    }

    void authorizeService(BluezQt::DevicePtr device, const QString &uuid, const BluezQt::Request<> &request) override
    {
        QVariantMap prompt = describe(device, QStringLiteral("authorize-service"));
        prompt.insert(QStringLiteral("uuid"), uuid);
        d->showPrompt(prompt, [request](bool accept, const QString &) {
            if (accept)
                request.accept();
            else
                request.reject();
        });
    }

    void cancel() override { d->clearPrompt(); }
    void release() override { d->agentRegistered = false; }

private:
    static QVariantMap describe(const BluezQt::DevicePtr &device, const QString &kind)
    {
        QVariantMap prompt{{QStringLiteral("kind"), kind}, {QStringLiteral("code"), QString()},
                           {QStringLiteral("uuid"), QString()}, {QStringLiteral("entered"), 0}};
        prompt.insert(QStringLiteral("device"), device ? device->name() : QString());
        prompt.insert(QStringLiteral("address"), device ? device->address() : QString());
        return prompt;
    }

    BluetoothBackend::Private *d;
    QString m_path;
};

void BluetoothBackend::Private::registerAgent()
{
    if (!manager || !manager->isOperational() || agentRegistered || !agent)
        return;
    agentRegistered = true;
    whenFinished(manager->registerAgent(agent), q, [this](int error, const QString &text) {
        if (error != BluezQt::PendingCall::NoError && error != BluezQt::PendingCall::AlreadyExists) {
            agentRegistered = false;
            qWarning("Bluetooth: the pairing agent could not be registered: %s", qPrintable(text));
        }
    });
}

void BluetoothBackend::Private::queryBattery(const QString &ubi)
{
    batteryQueried.insert(ubi);
    const QDBusMessage get = dbusMethod(kBluez, ubi, kProperties, QStringLiteral("Get"),
                                        {kBattery, QStringLiteral("Percentage")});
    dbusCall(QDBusConnection::systemBus(), get, q, [this, ubi](const QDBusMessage &reply) {
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
            return;             // the device does not report a battery
        bool ok = false;
        const int percent = dbusPlain(reply.arguments().constFirst()).toInt(&ok);
        if (ok && percent >= 0 && percent <= 100 && batteries.value(ubi, -1) != percent) {
            batteries.insert(ubi, percent);
            q->scheduleRebuild();
        }
    });
}
#endif

// ---- BluetoothBackend ---------------------------------------------------------------

BluetoothBackend::BluetoothBackend(QObject *parent)
    : QObject(parent), d(std::make_unique<Private>(this))
{
    d->paired = new RowModel(kDeviceRoles, QStringLiteral("ubi"), this);
    d->available = new RowModel(kDeviceRoles, QStringLiteral("ubi"), this);

    d->rebuildTimer.setSingleShot(true);
    d->rebuildTimer.setInterval(40);
    connect(&d->rebuildTimer, &QTimer::timeout, this, &BluetoothBackend::rebuild);

    d->scanTimer.setSingleShot(true);
    d->scanTimer.setInterval(kScanSeconds * 1000);
    connect(&d->scanTimer, &QTimer::timeout, this, &BluetoothBackend::stopScan);

#ifdef NEXUS_HAVE_BLUEZQT
    static int agentCounter = 0;
    d->manager = new BluezQt::Manager(this);
    d->agent = new PairingAgent(d.get(), QStringLiteral("/org/nexus/controlcenter/BluetoothAgent%1").arg(++agentCounter), this);

    using BluezQt::Manager;
    connect(d->manager, &Manager::operationalChanged, this, [this](bool operational) {
        if (operational) {
            d->registerAgent();
        } else {
            d->agentRegistered = false;
            d->clearPrompt();
            d->busy.clear();
            d->batteries.clear();
            d->batteryQueried.clear();
        }
        scheduleRebuild();
    });
    connect(d->manager, &Manager::bluetoothBlockedChanged, this, &BluetoothBackend::scheduleRebuild);
    connect(d->manager, &Manager::adapterAdded, this, [this](BluezQt::AdapterPtr adapter) {
        if (d->powerOnWhenAdapterAppears && adapter) {
            d->powerOnWhenAdapterAppears = false;
            adapter->setPowered(true);
        }
        scheduleRebuild();
    });
    connect(d->manager, &Manager::adapterRemoved, this, &BluetoothBackend::scheduleRebuild);
    connect(d->manager, &Manager::adapterChanged, this, &BluetoothBackend::scheduleRebuild);
    connect(d->manager, &Manager::usableAdapterChanged, this, &BluetoothBackend::scheduleRebuild);
    connect(d->manager, &Manager::deviceAdded, this, &BluetoothBackend::scheduleRebuild);
    connect(d->manager, &Manager::deviceRemoved, this, [this](BluezQt::DevicePtr device) {
        if (device) {
            d->batteries.remove(device->ubi());
            d->batteryQueried.remove(device->ubi());
            d->busy.remove(device->ubi());
        }
        scheduleRebuild();
    });
    connect(d->manager, &Manager::deviceChanged, this, &BluetoothBackend::scheduleRebuild);

    QDBusConnection bus = QDBusConnection::systemBus();
    if (bus.isConnected()) {
        // Battery levels of connected devices (org.bluez.Battery1 on the device object).
        bus.connect(kBluez, QString(), kProperties, QStringLiteral("PropertiesChanged"), {kBattery}, QString(), this,
                    SLOT(onBatteryPropertiesChanged(QString, QVariantMap, QStringList, QDBusMessage)));
    }

    if (!bus.isConnected()) {
        // Without a system bus BluezQt's initialisation never reports back.
        d->initialized = true;
        QTimer::singleShot(0, this, &BluetoothBackend::rebuild);
    } else {
        BluezQt::InitManagerJob *job = d->manager->init();
        connect(job, &BluezQt::InitManagerJob::result, this, [this] {
            d->initialized = true;
            d->registerAgent();
            rebuild();
        });
        job->start();
        // Never leave the page without a state if BlueZ does not answer.
        QTimer::singleShot(4000, this, [this] {
            if (!d->initialized) {
                d->initialized = true;
                rebuild();
            }
        });
    }
#else
    QTimer::singleShot(0, this, &BluetoothBackend::rebuild);
#endif
}

BluetoothBackend::~BluetoothBackend()
{
#ifdef NEXUS_HAVE_BLUEZQT
    // Leave nothing running on behalf of a page that is gone.
    if (d->startedScan) {
        if (BluezQt::AdapterPtr adapter = d->adapter())
            adapter->stopDiscovery();
    }
    if (d->answer)
        d->answer(false, QString());
    if (d->agentRegistered && d->manager->isOperational())
        d->manager->unregisterAgent(d->agent);
#endif
}

QAbstractItemModel *BluetoothBackend::pairedDevices() const { return d->paired; }
QAbstractItemModel *BluetoothBackend::availableDevices() const { return d->available; }
int BluetoothBackend::pairedCount() const { return d->paired->count(); }
int BluetoothBackend::availableCount() const { return d->available->count(); }

void BluetoothBackend::scheduleRebuild()
{
    if (!d->rebuildTimer.isActive())
        d->rebuildTimer.start();
}

void BluetoothBackend::rebuild()
{
    QString state, message;
    bool powered = false, discoverable = false, discovering = false;
    QString adapterName, adapterAddress;
    int adapterCount = 0, discoverableTimeout = 0;
    QVariantList paired, available;

    d->rfkill = rfkillState(QStringLiteral("bluetooth"));

#ifdef NEXUS_HAVE_BLUEZQT
    if (!d->initialized)
        return;
    if (!QDBusConnection::systemBus().isConnected()) {
        state = QStringLiteral("no-system-bus");
        message = tr("The system message bus is not available, so the Bluetooth service (BlueZ) cannot be reached.");
    } else if (!d->manager->isOperational()) {
        state = QStringLiteral("service-not-running");
        message = tr("The Bluetooth service (bluetooth.service, BlueZ) is not running, so adapters and devices cannot be listed.");
    } else if (d->rfkill.hard) {
        state = QStringLiteral("rfkill-hard");
        message = tr("Bluetooth is switched off by a hardware switch or by the firmware (rfkill hard block). "
                     "Use the wireless key or switch of this computer to turn it back on.");
    } else {
        const BluezQt::AdapterPtr adapter = d->adapter();
        adapterCount = int(d->manager->adapters().size());
        if (d->rfkill.soft) {
            state = QStringLiteral("rfkill-soft");
            message = tr("Bluetooth is blocked in software (rfkill soft block). Switching Bluetooth on removes the block.");
        } else if (!adapter) {
            state = QStringLiteral("no-adapter");
            message = tr("The Bluetooth service is running, but this computer has no Bluetooth adapter.");
        } else {
            state = QStringLiteral("ok");
        }
        if (adapter) {
            powered = adapter->isPowered();
            discoverable = adapter->isDiscoverable();
            discovering = adapter->isDiscovering();
            adapterName = adapter->name();
            adapterAddress = adapter->address();
            discoverableTimeout = int(adapter->discoverableTimeout());

            QList<BluezQt::DevicePtr> devices = adapter->devices();
            std::sort(devices.begin(), devices.end(), [](const BluezQt::DevicePtr &a, const BluezQt::DevicePtr &b) {
                if (a->isConnected() != b->isConnected())
                    return a->isConnected();
                if (a->rssi() != b->rssi())
                    return a->rssi() > b->rssi();
                return a->name().compare(b->name(), Qt::CaseInsensitive) < 0;
            });
            QSet<QString> present;
            for (const BluezQt::DevicePtr &device : std::as_const(devices)) {
                const QString ubi = device->ubi();
                present.insert(ubi);
                const bool mine = device->isPaired() || device->isConnected();
                const QString name = device->name();
                if (!mine && !hasRealName(name))
                    continue;           // anonymous advertisers nobody can pick by name
                if (device->isConnected() && !d->batteryQueried.contains(ubi))
                    d->queryBattery(ubi);
                if (!device->isConnected()) {
                    d->batteries.remove(ubi);
                    d->batteryQueried.remove(ubi);
                }
                const int rssi = device->rssi();
                const QVariantMap row{
                    {QStringLiteral("ubi"), ubi},
                    {QStringLiteral("address"), device->address()},
                    {QStringLiteral("name"), hasRealName(name) ? name : device->address()},
                    {QStringLiteral("kind"), kindLabel(device->type())},
                    {QStringLiteral("deviceIcon"), kindIcon(device->type())},
                    {QStringLiteral("paired"), device->isPaired()},
                    {QStringLiteral("trusted"), device->isTrusted()},
                    {QStringLiteral("connected"), device->isConnected()},
                    {QStringLiteral("battery"), d->batteries.value(ubi, -1)},
                    {QStringLiteral("rssi"), rssi == kInvalidRssi ? 0 : rssi},
                    {QStringLiteral("busy"), d->busy.value(ubi)},
                };
                if (mine)
                    paired.append(row);
                else
                    available.append(row);
            }
            for (auto it = d->busy.begin(); it != d->busy.end();) {
                if (present.contains(it.key()))
                    ++it;
                else
                    it = d->busy.erase(it);
            }
        }
    }
#else
    state = QStringLiteral("unsupported-build");
    message = tr("This build of the Control Center has no Bluetooth support (it was built without KDE's BluezQt library).");
#endif

    d->paired->setRows(paired);
    d->available->setRows(available);
    Q_EMIT devicesChanged();

    if (powered != m_powered || discoverable != m_discoverable || discovering != m_discovering
        || adapterName != m_adapterName || adapterAddress != m_adapterAddress || adapterCount != m_adapterCount
        || discoverableTimeout != m_discoverableTimeout) {
        m_powered = powered;
        m_discoverable = discoverable;
        m_discovering = discovering;
        m_adapterName = adapterName;
        m_adapterAddress = adapterAddress;
        m_adapterCount = adapterCount;
        m_discoverableTimeout = discoverableTimeout;
        Q_EMIT adapterChanged();
    }
    if (state != m_state || message != m_statusMessage) {
        m_state = state;
        m_statusMessage = message;
        Q_EMIT stateChanged();
    }
    if (!m_ready) {
        m_ready = true;
        Q_EMIT readyChanged();
    }
}

void BluetoothBackend::onBatteryPropertiesChanged(const QString &interface, const QVariantMap &changed,
                                                  const QStringList &invalidated, const QDBusMessage &message)
{
    Q_UNUSED(interface)
    Q_UNUSED(invalidated)
    const QVariant value = dbusPlain(changed.value(QStringLiteral("Percentage")));
    bool ok = false;
    const int percent = value.toInt(&ok);
    if (!value.isValid() || !ok || percent < 0 || percent > 100)
        return;
    d->batteries.insert(message.path(), percent);
    d->batteryQueried.insert(message.path());
    scheduleRebuild();
}

// ---- actions ---------------------------------------------------------------------------

void BluetoothBackend::setPowered(bool powered)
{
#ifdef NEXUS_HAVE_BLUEZQT
    if (!d->manager->isOperational()) {
        Q_EMIT failed(m_statusMessage);
        return;
    }
    if (powered && d->rfkill.soft)
        d->manager->setBluetoothBlocked(false);
    const BluezQt::AdapterPtr adapter = d->adapter();
    if (!adapter) {
        if (powered && d->rfkill.soft) {
            d->powerOnWhenAdapterAppears = true;    // the adapter shows up once the block is gone
            scheduleRebuild();
        } else {
            Q_EMIT failed(tr("There is no Bluetooth adapter to switch."));
        }
        return;
    }
    whenFinished(adapter->setPowered(powered), this, [this, powered](int error, const QString &text) {
        if (error != BluezQt::PendingCall::NoError) {
            Q_EMIT failed(powered ? tr("Bluetooth could not be switched on: %1").arg(callError(error, text))
                                  : tr("Bluetooth could not be switched off: %1").arg(callError(error, text)));
        }
        scheduleRebuild();
    });
#else
    Q_UNUSED(powered)
    Q_EMIT failed(m_statusMessage);
#endif
}

void BluetoothBackend::setDiscoverable(bool discoverable)
{
#ifdef NEXUS_HAVE_BLUEZQT
    const BluezQt::AdapterPtr adapter = d->adapter();
    if (!adapter || !adapter->isPowered()) {
        Q_EMIT failed(tr("Switch Bluetooth on first."));
        return;
    }
    whenFinished(adapter->setDiscoverable(discoverable), this, [this](int error, const QString &text) {
        if (error != BluezQt::PendingCall::NoError)
            Q_EMIT failed(tr("The visibility could not be changed: %1").arg(callError(error, text)));
        scheduleRebuild();
    });
#else
    Q_UNUSED(discoverable)
    Q_EMIT failed(m_statusMessage);
#endif
}

void BluetoothBackend::startScan()
{
#ifdef NEXUS_HAVE_BLUEZQT
    const BluezQt::AdapterPtr adapter = d->adapter();
    if (!adapter || !adapter->isPowered()) {
        Q_EMIT failed(tr("Switch Bluetooth on first."));
        return;
    }
    d->startedScan = true;
    d->scanTimer.start();
    whenFinished(adapter->startDiscovery(), this, [this](int error, const QString &text) {
        if (error != BluezQt::PendingCall::NoError && error != BluezQt::PendingCall::InProgress) {
            d->startedScan = false;
            d->scanTimer.stop();
            Q_EMIT failed(tr("The search for devices could not be started: %1").arg(callError(error, text)));
        }
        scheduleRebuild();
    });
#else
    Q_EMIT failed(m_statusMessage);
#endif
}

void BluetoothBackend::stopScan()
{
#ifdef NEXUS_HAVE_BLUEZQT
    d->scanTimer.stop();
    if (!d->startedScan)
        return;
    d->startedScan = false;
    if (const BluezQt::AdapterPtr adapter = d->adapter())
        whenFinished(adapter->stopDiscovery(), this, [this](int, const QString &) { scheduleRebuild(); });
#endif
}

void BluetoothBackend::pair(const QString &ubi)
{
#ifdef NEXUS_HAVE_BLUEZQT
    const BluezQt::DevicePtr device = d->device(ubi);
    if (!device) {
        Q_EMIT failed(tr("The device is no longer in range."));
        return;
    }
    if (d->busy.contains(ubi))
        return;
    d->registerAgent();
    d->setBusy(ubi, QStringLiteral("pairing"));
    const QString name = device->name();
    whenFinished(device->pair(), this, [this, ubi, name](int error, const QString &text) {
        d->clearPrompt();
        if (error != BluezQt::PendingCall::NoError && error != BluezQt::PendingCall::AlreadyExists) {
            d->setBusy(ubi, QString());
            Q_EMIT failed(tr("Pairing with %1 failed: %2").arg(name, callError(error, text)));
            return;
        }
        // A paired device is one the user chose: trust it, so it may
        // reconnect on its own, and connect it now.
        const BluezQt::DevicePtr paired = d->device(ubi);
        if (!paired) {
            d->setBusy(ubi, QString());
            return;
        }
        paired->setTrusted(true);
        d->setBusy(ubi, QStringLiteral("connecting"));
        whenFinished(paired->connectToDevice(), this, [this, ubi, name](int connectError, const QString &connectText) {
            d->setBusy(ubi, QString());
            // Devices without a profile this computer can use stay paired but unconnected.
            if (connectError != BluezQt::PendingCall::NoError && connectError != BluezQt::PendingCall::AlreadyConnected)
                Q_EMIT failed(tr("%1 is paired, but could not be connected: %2").arg(name, callError(connectError, connectText)));
        });
    });
#else
    Q_UNUSED(ubi)
    Q_EMIT failed(m_statusMessage);
#endif
}

void BluetoothBackend::connectDevice(const QString &ubi)
{
#ifdef NEXUS_HAVE_BLUEZQT
    const BluezQt::DevicePtr device = d->device(ubi);
    if (!device) {
        Q_EMIT failed(tr("The device is no longer known."));
        return;
    }
    if (d->busy.contains(ubi))
        return;
    d->setBusy(ubi, QStringLiteral("connecting"));
    const QString name = device->name();
    whenFinished(device->connectToDevice(), this, [this, ubi, name](int error, const QString &text) {
        d->setBusy(ubi, QString());
        if (error != BluezQt::PendingCall::NoError && error != BluezQt::PendingCall::AlreadyConnected)
            Q_EMIT failed(tr("%1 could not be connected: %2").arg(name, callError(error, text)));
    });
#else
    Q_UNUSED(ubi)
    Q_EMIT failed(m_statusMessage);
#endif
}

void BluetoothBackend::disconnectDevice(const QString &ubi)
{
#ifdef NEXUS_HAVE_BLUEZQT
    const BluezQt::DevicePtr device = d->device(ubi);
    if (!device)
        return;
    d->setBusy(ubi, QStringLiteral("disconnecting"));
    const QString name = device->name();
    whenFinished(device->disconnectFromDevice(), this, [this, ubi, name](int error, const QString &text) {
        d->setBusy(ubi, QString());
        if (error != BluezQt::PendingCall::NoError && error != BluezQt::PendingCall::NotConnected)
            Q_EMIT failed(tr("%1 could not be disconnected: %2").arg(name, callError(error, text)));
    });
#else
    Q_UNUSED(ubi)
#endif
}

void BluetoothBackend::setTrusted(const QString &ubi, bool trusted)
{
#ifdef NEXUS_HAVE_BLUEZQT
    const BluezQt::DevicePtr device = d->device(ubi);
    if (!device)
        return;
    whenFinished(device->setTrusted(trusted), this, [this](int error, const QString &text) {
        if (error != BluezQt::PendingCall::NoError)
            Q_EMIT failed(tr("The device could not be changed: %1").arg(callError(error, text)));
        scheduleRebuild();
    });
#else
    Q_UNUSED(ubi)
    Q_UNUSED(trusted)
#endif
}

void BluetoothBackend::removeDevice(const QString &ubi)
{
#ifdef NEXUS_HAVE_BLUEZQT
    const BluezQt::DevicePtr device = d->device(ubi);
    if (!device || !device->adapter())
        return;
    d->setBusy(ubi, QStringLiteral("removing"));
    const QString name = device->name();
    whenFinished(device->adapter()->removeDevice(device), this, [this, ubi, name](int error, const QString &text) {
        d->setBusy(ubi, QString());
        if (error != BluezQt::PendingCall::NoError)
            Q_EMIT failed(tr("%1 could not be removed: %2").arg(name, callError(error, text)));
    });
#else
    Q_UNUSED(ubi)
#endif
}

void BluetoothBackend::answerPrompt(bool accept, const QString &text)
{
    const auto answer = d->answer;
    d->clearPrompt();
    if (answer)
        answer(accept, text);
}

void BluetoothBackend::startService()
{
#ifdef NEXUS_HAVE_BLUEZQT
    whenFinished(BluezQt::Manager::startService(), this, [this](int error, const QString &text) {
        if (error != BluezQt::PendingCall::NoError)
            Q_EMIT failed(tr("The Bluetooth service could not be started: %1").arg(callError(error, text)));
        scheduleRebuild();
    });
#else
    Q_EMIT failed(m_statusMessage);
#endif
}

QJsonObject BluetoothBackend::toJson() const
{
    QJsonArray devices = QJsonArray::fromVariantList(d->paired->rows());
    const QJsonArray others = QJsonArray::fromVariantList(d->available->rows());
    for (const QJsonValue &other : others)
        devices.append(other);
    return {
        {QStringLiteral("state"), m_state},
        {QStringLiteral("available"), available()},
        {QStringLiteral("statusMessage"), m_statusMessage},
        {QStringLiteral("rfkill"), QJsonObject{
            {QStringLiteral("present"), d->rfkill.present},
            {QStringLiteral("hardBlocked"), d->rfkill.hard},
            {QStringLiteral("softBlocked"), d->rfkill.soft},
        }},
        {QStringLiteral("adapterCount"), m_adapterCount},
        {QStringLiteral("adapter"), QJsonObject{
            {QStringLiteral("name"), m_adapterName},
            {QStringLiteral("address"), m_adapterAddress},
            {QStringLiteral("powered"), m_powered},
            {QStringLiteral("discoverable"), m_discoverable},
            {QStringLiteral("discovering"), m_discovering},
        }},
        {QStringLiteral("devices"), devices},
    };
}
