// SPDX-License-Identifier: GPL-3.0-or-later
//
// status-test-dbus-fixture — stands in for system services on a PRIVATE
// message bus, so the status back-ends' D-Bus handling (object tracking,
// PropertiesChanged, property writes) can be exercised where the real
// services cannot run (the build chroot has no system bus).
//
//   status-test-dbus-fixture <fixture.json> [--control <fifo>]
//
// It connects to the bus named by DBUS_SYSTEM_BUS_ADDRESS and refuses to
// run without that variable: it must never be pointed at a real system bus.
//
// fixture.json:
//   { "names":   ["org.freedesktop.NetworkManager", ...],
//     "objects": { "<object path>": { "<interface>": { "<Property>": ["<type>", <value>], ... } } } }
//
// Types: s b y i u x t d o as ao ay (a string, sent as its UTF-8 bytes) and
// aa{sv} (a list of objects with string values).
//
// The fixture is passive. It answers org.freedesktop.DBus.Properties
// (Get, GetAll, Set); a client's Set is stored, announced with
// PropertiesChanged and printed:
//
//   client-set <path> <interface> <Property> <json value>
//
// Lines written to the control FIFO change a property the same way:
//
//   set <path> <interface> <Property> <type> <json value>
//
// This is test scaffolding. It proves how the back-ends treat the documented
// D-Bus interfaces; it proves nothing about the real services.
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSocketNotifier>
#include <QTextStream>

#include <fcntl.h>
#include <unistd.h>

namespace {

const QString kProperties = QStringLiteral("org.freedesktop.DBus.Properties");

QTextStream &out()
{
    static QTextStream stream(stdout);
    return stream;
}

QVariant typed(const QString &type, const QJsonValue &value, bool *ok)
{
    *ok = true;
    if (type == QLatin1String("s"))
        return value.toString();
    if (type == QLatin1String("b"))
        return value.toBool();
    if (type == QLatin1String("y"))
        return QVariant::fromValue(uchar(value.toInt()));
    if (type == QLatin1String("i"))
        return value.toInt();
    if (type == QLatin1String("u"))
        return QVariant::fromValue(quint32(value.toDouble()));
    if (type == QLatin1String("x"))
        return QVariant::fromValue(qlonglong(value.toDouble()));
    if (type == QLatin1String("t"))
        return QVariant::fromValue(qulonglong(value.toDouble()));
    if (type == QLatin1String("d"))
        return value.toDouble();
    if (type == QLatin1String("o"))
        return QVariant::fromValue(QDBusObjectPath(value.toString()));
    if (type == QLatin1String("ay"))
        return value.toString().toUtf8();
    if (type == QLatin1String("as")) {
        QStringList list;
        for (const QJsonValue &item : value.toArray())
            list << item.toString();
        return list;
    }
    if (type == QLatin1String("ao")) {
        QList<QDBusObjectPath> list;
        for (const QJsonValue &item : value.toArray())
            list << QDBusObjectPath(item.toString());
        return QVariant::fromValue(list);
    }
    if (type == QLatin1String("aa{sv}")) {
        QList<QVariantMap> list;
        for (const QJsonValue &item : value.toArray())
            list << item.toObject().toVariantMap();
        return QVariant::fromValue(list);
    }
    *ok = false;
    return {};
}

QString jsonText(const QVariant &value)
{
    return QString::fromUtf8(QJsonDocument(QJsonArray{QJsonValue::fromVariant(value)}).toJson(QJsonDocument::Compact))
        .mid(1).chopped(1);
}

class Fixture : public QDBusVirtualObject
{
public:
    explicit Fixture(const QDBusConnection &bus)
        : m_bus(bus)
    {
    }

    bool load(const QJsonObject &objects)
    {
        for (auto object = objects.begin(); object != objects.end(); ++object) {
            const QJsonObject interfaces = object.value().toObject();
            for (auto interface = interfaces.begin(); interface != interfaces.end(); ++interface) {
                const QJsonObject properties = interface.value().toObject();
                QVariantMap &values = m_objects[object.key()][interface.key()];
                for (auto property = properties.begin(); property != properties.end(); ++property) {
                    const QJsonArray pair = property.value().toArray();
                    bool ok = false;
                    values.insert(property.key(), typed(pair.at(0).toString(), pair.at(1), &ok));
                    if (!ok) {
                        QTextStream(stderr) << "unknown type '" << pair.at(0).toString() << "' for " << object.key()
                                            << " " << property.key() << Qt::endl;
                        return false;
                    }
                }
            }
        }
        return true;
    }

    QString introspect(const QString &) const override { return {}; }

    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        if (message.interface() != kProperties)
            return false;
        const QVariantList args = message.arguments();
        const QString interface = args.value(0).toString();
        const auto object = m_objects.constFind(message.path());
        if (object == m_objects.cend() || !object->contains(interface)) {
            connection.send(message.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.UnknownObject"),
                                                     QStringLiteral("The fixture has no %1 at %2").arg(interface, message.path())));
            return true;
        }
        const QVariantMap values = object->value(interface);
        if (message.member() == QLatin1String("GetAll")) {
            connection.send(message.createReply(QVariant(values)));
            return true;
        }
        const QString name = args.value(1).toString();
        if (!values.contains(name)) {
            connection.send(message.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.UnknownProperty"), name));
            return true;
        }
        if (message.member() == QLatin1String("Get")) {
            connection.send(message.createReply(QVariant::fromValue(QDBusVariant(values.value(name)))));
            return true;
        }
        if (message.member() == QLatin1String("Set")) {
            const QVariant value = args.value(2).value<QDBusVariant>().variant();
            out() << "client-set " << message.path() << ' ' << interface << ' ' << name << ' ' << jsonText(value) << Qt::endl;
            connection.send(message.createReply());
            change(message.path(), interface, name, value);
            return true;
        }
        return false;
    }

    void change(const QString &path, const QString &interface, const QString &name, const QVariant &value)
    {
        m_objects[path][interface].insert(name, value);
        QDBusMessage signal = QDBusMessage::createSignal(path, kProperties, QStringLiteral("PropertiesChanged"));
        signal << interface << QVariantMap{{name, value}} << QStringList();
        m_bus.send(signal);
    }

    // "set <path> <interface> <Property> <type> <json value>"
    void command(const QString &line)
    {
        const QStringList words = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (words.size() < 6 || words.at(0) != QLatin1String("set")) {
            out() << "ignored: " << line << Qt::endl;
            return;
        }
        const QString json = QStringList(words.mid(5)).join(QLatin1Char(' '));
        const QJsonValue value = QJsonDocument::fromJson("[" + json.toUtf8() + "]").array().at(0);
        bool ok = false;
        const QVariant variant = typed(words.at(4), value, &ok);
        if (!ok || value.isUndefined()) {
            out() << "ignored (type or value): " << line << Qt::endl;
            return;
        }
        change(words.at(1), words.at(2), words.at(3), variant);
        out() << "set " << words.at(1) << ' ' << words.at(3) << Qt::endl;
    }

private:
    QDBusConnection m_bus;
    QHash<QString, QHash<QString, QVariantMap>> m_objects;
};

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    qDBusRegisterMetaType<QList<QVariantMap>>();

    const QStringList args = app.arguments();
    QString fixturePath, controlPath;
    for (int i = 1; i < args.size(); ++i) {
        if (args.at(i) == QLatin1String("--control") && i + 1 < args.size())
            controlPath = args.at(++i);
        else
            fixturePath = args.at(i);
    }
    if (fixturePath.isEmpty()) {
        QTextStream(stderr) << "usage: status-test-dbus-fixture <fixture.json> [--control <fifo>]" << Qt::endl;
        return 2;
    }
    if (qEnvironmentVariableIsEmpty("DBUS_SYSTEM_BUS_ADDRESS")) {
        QTextStream(stderr) << "status-test-dbus-fixture: DBUS_SYSTEM_BUS_ADDRESS is not set; "
                               "this tool only runs on a private test bus" << Qt::endl;
        return 2;
    }

    QFile file(fixturePath);
    if (!file.open(QIODevice::ReadOnly)) {
        QTextStream(stderr) << "cannot read " << fixturePath << Qt::endl;
        return 1;
    }
    QJsonParseError parseError;
    const QJsonObject fixtureJson = QJsonDocument::fromJson(file.readAll(), &parseError).object();
    if (parseError.error != QJsonParseError::NoError) {
        QTextStream(stderr) << fixturePath << ": " << parseError.errorString() << Qt::endl;
        return 1;
    }

    QDBusConnection bus = QDBusConnection::systemBus();
    Fixture fixture(bus);
    if (!fixture.load(fixtureJson.value(QLatin1String("objects")).toObject()))
        return 1;
    if (!bus.isConnected() || !bus.registerVirtualObject(QStringLiteral("/"), &fixture, QDBusConnection::SubPath)) {
        QTextStream(stderr) << "cannot use the bus at " << qEnvironmentVariable("DBUS_SYSTEM_BUS_ADDRESS") << Qt::endl;
        return 1;
    }
    for (const QJsonValue &name : fixtureJson.value(QLatin1String("names")).toArray()) {
        if (!bus.registerService(name.toString())) {
            QTextStream(stderr) << "cannot own " << name.toString() << Qt::endl;
            return 1;
        }
    }

    QByteArray pending;
    if (!controlPath.isEmpty()) {
        // Read-write, so the FIFO stays open between writers.
        const int fd = ::open(QFile::encodeName(controlPath).constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            QTextStream(stderr) << "cannot open " << controlPath << Qt::endl;
            return 1;
        }
        auto *notifier = new QSocketNotifier(fd, QSocketNotifier::Read, &app);
        QObject::connect(notifier, &QSocketNotifier::activated, &app, [&, fd] {
            char buffer[4096];
            ssize_t size;
            while ((size = ::read(fd, buffer, sizeof(buffer))) > 0)
                pending.append(buffer, size);
            int end;
            while ((end = pending.indexOf('\n')) >= 0) {
                fixture.command(QString::fromUtf8(pending.left(end)).trimmed());
                pending.remove(0, end + 1);
            }
        });
    }

    out() << "ready" << Qt::endl;
    return app.exec();
}
