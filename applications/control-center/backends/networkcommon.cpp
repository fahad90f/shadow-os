// SPDX-License-Identifier: GPL-3.0-or-later
#include "networkcommon.h"

#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusSignature>
#include <QDBusVariant>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSet>

#include <memory>

namespace NetHw {

namespace {
const int kFirstRole = Qt::UserRole + 1;

QString readSmallFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.read(4096)).trimmed();
}
} // namespace

// ---- RowModel ---------------------------------------------------------------

RowModel::RowModel(const QStringList &roles, const QString &keyRole, QObject *parent)
    : QAbstractListModel(parent), m_roles(roles), m_keyRole(keyRole)
{
    Q_ASSERT(m_roles.contains(m_keyRole));
}

int RowModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant RowModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return QVariant();
    const int i = role - kFirstRole;
    if (i < 0 || i >= m_roles.size())
        return QVariant();
    return m_rows.at(index.row()).value(m_roles.at(i));
}

QHash<int, QByteArray> RowModel::roleNames() const
{
    QHash<int, QByteArray> names;
    for (int i = 0; i < m_roles.size(); ++i)
        names.insert(kFirstRole + i, m_roles.at(i).toUtf8());
    return names;
}

QVariantList RowModel::rows() const
{
    QVariantList list;
    list.reserve(m_rows.size());
    for (const QVariantMap &row : m_rows)
        list.append(row);
    return list;
}

QVariantMap RowModel::get(int row) const
{
    return row >= 0 && row < m_rows.size() ? m_rows.at(row) : QVariantMap();
}

QVariantMap RowModel::find(const QString &key) const
{
    for (const QVariantMap &row : m_rows) {
        if (keyOf(row) == key)
            return row;
    }
    return QVariantMap();
}

void RowModel::setRows(const QVariantList &rows)
{
    QList<QVariantMap> next;
    next.reserve(rows.size());
    QSet<QString> keys;
    for (const QVariant &value : rows) {
        const QVariantMap row = value.toMap();
        const QString key = keyOf(row);
        if (keys.contains(key))
            continue;                       // keys are unique; a repeated key is a duplicate row
        keys.insert(key);
#ifndef QT_NO_DEBUG
        for (const QString &role : m_roles) {
            if (!row.contains(role))
                qWarning("RowModel: row '%s' has no value for role '%s'", qPrintable(key), qPrintable(role));
        }
#endif
        next.append(row);
    }

    const qsizetype before = m_rows.size();

    // 1. Rows that are gone.
    for (int i = int(m_rows.size()) - 1; i >= 0; --i) {
        if (!keys.contains(keyOf(m_rows.at(i)))) {
            beginRemoveRows(QModelIndex(), i, i);
            m_rows.removeAt(i);
            endRemoveRows();
        }
    }

    // 2. Front to back: move or insert each row into place, then update it.
    for (int i = 0; i < next.size(); ++i) {
        const QString key = keyOf(next.at(i));
        if (i >= m_rows.size() || keyOf(m_rows.at(i)) != key) {
            int from = -1;
            for (int j = i + 1; j < m_rows.size(); ++j) {
                if (keyOf(m_rows.at(j)) == key) {
                    from = j;
                    break;
                }
            }
            if (from < 0) {
                beginInsertRows(QModelIndex(), i, i);
                m_rows.insert(i, next.at(i));
                endInsertRows();
                continue;
            }
            beginMoveRows(QModelIndex(), from, from, QModelIndex(), i);
            m_rows.move(from, i);
            endMoveRows();
        }
        if (m_rows.at(i) != next.at(i)) {
            QList<int> changed;
            for (int r = 0; r < m_roles.size(); ++r) {
                if (m_rows.at(i).value(m_roles.at(r)) != next.at(i).value(m_roles.at(r)))
                    changed.append(kFirstRole + r);
            }
            m_rows[i] = next.at(i);
            if (!changed.isEmpty())
                Q_EMIT dataChanged(index(i), index(i), changed);
        }
    }

    if (before != m_rows.size())
        Q_EMIT countChanged();
}

// ---- D-Bus ------------------------------------------------------------------

QDBusMessage dbusMethod(const QString &service, const QString &path, const QString &interface,
                        const QString &method, const QVariantList &arguments)
{
    QDBusMessage message = QDBusMessage::createMethodCall(service, path, interface, method);
    message.setArguments(arguments);
    return message;
}

void dbusCall(const QDBusConnection &bus, const QDBusMessage &message, QObject *context,
              DBusReplyCallback done, int timeoutMs)
{
    if (!bus.isConnected()) {
        // Report through the event loop so callers always get an asynchronous answer.
        const QDBusMessage error = message.createErrorReply(QDBusError::Disconnected,
                                                            QStringLiteral("Not connected to the message bus"));
        QTimer::singleShot(0, context, [done, error] {
            if (done)
                done(error);
        });
        return;
    }
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, timeoutMs), context);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context, [done](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (done)
            done(w->reply());
    });
}

void dbusGetAll(const QDBusConnection &bus, const QString &service, const QString &path,
                const QString &interface, QObject *context, DBusPropertiesCallback done, int timeoutMs)
{
    const QDBusMessage message = dbusMethod(service, path, QStringLiteral("org.freedesktop.DBus.Properties"),
                                            QStringLiteral("GetAll"), {interface});
    dbusCall(bus, message, context, [done](const QDBusMessage &reply) {
        if (!done)
            return;
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
            done(false, QVariantMap(), dbusErrorText(reply));
            return;
        }
        const QVariant first = reply.arguments().constFirst();
        QVariantMap map;
        if (first.canConvert<QDBusArgument>())
            first.value<QDBusArgument>() >> map;
        else
            map = first.toMap();
        done(true, map, QString());
    }, timeoutMs);
}

void dbusSet(const QDBusConnection &bus, const QString &service, const QString &path,
             const QString &interface, const QString &property, const QVariant &value,
             QObject *context, DBusResultCallback done, int timeoutMs)
{
    const QDBusMessage message = dbusMethod(service, path, QStringLiteral("org.freedesktop.DBus.Properties"),
                                            QStringLiteral("Set"),
                                            {interface, property, QVariant::fromValue(QDBusVariant(value))});
    dbusCall(bus, message, context, [done](const QDBusMessage &reply) {
        if (!done)
            return;
        const bool ok = reply.type() == QDBusMessage::ReplyMessage;
        done(ok, ok ? QString() : dbusErrorText(reply));
    }, timeoutMs);
}

void dbusNameHasOwner(const QDBusConnection &bus, const QString &service, QObject *context,
                      std::function<void(bool)> done)
{
    const QDBusMessage message = dbusMethod(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                                            QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameHasOwner"),
                                            {service});
    dbusCall(bus, message, context, [done](const QDBusMessage &reply) {
        if (!done)
            return;
        done(reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()
             && reply.arguments().constFirst().toBool());
    });
}

QString dbusErrorText(const QDBusMessage &reply)
{
    if (reply.type() != QDBusMessage::ErrorMessage)
        return QString();
    const QString text = reply.errorMessage().trimmed();
    return text.isEmpty() ? reply.errorName() : text;
}

QVariant dbusPlain(const QVariant &value)
{
    const int type = value.userType();
    if (type == qMetaTypeId<QDBusVariant>())
        return dbusPlain(value.value<QDBusVariant>().variant());
    if (type == qMetaTypeId<QDBusObjectPath>())
        return value.value<QDBusObjectPath>().path();
    if (type == qMetaTypeId<QDBusSignature>())
        return value.value<QDBusSignature>().signature();
    if (type == qMetaTypeId<QList<QDBusObjectPath>>()) {
        QStringList paths;
        const auto objects = value.value<QList<QDBusObjectPath>>();
        for (const QDBusObjectPath &object : objects)
            paths.append(object.path());
        return paths;
    }
    if (type == qMetaTypeId<QDBusArgument>()) {
        const QDBusArgument argument = value.value<QDBusArgument>();
        switch (argument.currentType()) {
        case QDBusArgument::BasicType:
        case QDBusArgument::VariantType:
            return dbusPlain(argument.asVariant());
        case QDBusArgument::ArrayType: {
            QVariantList list;
            argument.beginArray();
            while (!argument.atEnd())
                list.append(dbusPlain(argument.asVariant()));
            argument.endArray();
            return list;
        }
        case QDBusArgument::StructureType: {
            QVariantList list;
            argument.beginStructure();
            while (!argument.atEnd())
                list.append(dbusPlain(argument.asVariant()));
            argument.endStructure();
            return list;
        }
        case QDBusArgument::MapType: {
            QVariantMap map;
            argument.beginMap();
            while (!argument.atEnd()) {
                argument.beginMapEntry();
                const QVariant key = dbusPlain(argument.asVariant());
                const QVariant entry = dbusPlain(argument.asVariant());
                argument.endMapEntry();
                map.insert(key.toString(), entry);
            }
            argument.endMap();
            return map;
        }
        default:
            return QVariant();
        }
    }
    if (type == QMetaType::QVariantList) {
        QVariantList list;
        const QVariantList source = value.toList();
        for (const QVariant &item : source)
            list.append(dbusPlain(item));
        return list;
    }
    if (type == QMetaType::QVariantMap) {
        QVariantMap map;
        const QVariantMap source = value.toMap();
        for (auto it = source.constBegin(); it != source.constEnd(); ++it)
            map.insert(it.key(), dbusPlain(it.value()));
        return map;
    }
    return value;
}

// ---- rfkill -----------------------------------------------------------------

RfkillState rfkillState(const QString &type)
{
    RfkillState state;
    const QDir dir(QStringLiteral("/sys/class/rfkill"));
    const QStringList entries = dir.entryList({QStringLiteral("rfkill*")}, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        const QString base = dir.absoluteFilePath(entry);
        if (readSmallFile(base + QStringLiteral("/type")) != type)
            continue;
        state.present = true;
        if (readSmallFile(base + QStringLiteral("/hard")) == QLatin1String("1"))
            state.hard = true;
        if (readSmallFile(base + QStringLiteral("/soft")) == QLatin1String("1"))
            state.soft = true;
    }
    return state;
}

// ---- misc ---------------------------------------------------------------------

QProcessEnvironment toolEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
    env.insert(QStringLiteral("LANGUAGE"), QStringLiteral("C"));
    // --dump and the test hooks force this process onto the offscreen (or X11)
    // platform; a tool that has to reach the Wayland compositor must not
    // inherit that choice.
    if (env.contains(QStringLiteral("WAYLAND_DISPLAY")))
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("wayland"));
    else
        env.remove(QStringLiteral("QT_QPA_PLATFORM"));
    env.remove(QStringLiteral("QT_QUICK_CONTROLS_STYLE"));
    return env;
}

void runTool(QObject *context, const QString &program, const QStringList &arguments,
             std::function<void(const ToolResult &)> done, int timeoutMs)
{
    auto *process = new QProcess(context);
    process->setProcessEnvironment(toolEnvironment());
    process->setProgram(program);
    process->setArguments(arguments);

    auto finished = std::make_shared<bool>(false);
    const auto finish = [process, done, finished](int code) {
        if (*finished)
            return;
        *finished = true;
        ToolResult result;
        result.exitCode = code;
        result.out = process->readAllStandardOutput();
        result.err = process->readAllStandardError();
        process->deleteLater();
        if (done)
            done(result);
    };
    QObject::connect(process, &QProcess::finished, context, [finish](int code, QProcess::ExitStatus status) {
        finish(status == QProcess::NormalExit ? code : -1);
    });
    QObject::connect(process, &QProcess::errorOccurred, context, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(-1);
    });
    if (timeoutMs > 0) {
        QTimer::singleShot(timeoutMs, process, [process, finish] {
            if (process->state() != QProcess::NotRunning) {
                finish(-2);
                process->kill();
                process->waitForFinished(500);
            }
        });
    }
    process->start();
    process->closeWriteChannel();
}

QString firstErrorLine(const QString &text)
{
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &raw : lines) {
        QString line = raw.trimmed();
        if (line.isEmpty())
            continue;
        if (line.startsWith(QLatin1String("Error: ")))
            line = line.mid(7);
        return line;
    }
    return QString();
}

} // namespace NetHw
