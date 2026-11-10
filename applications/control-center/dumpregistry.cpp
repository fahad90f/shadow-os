// SPDX-License-Identifier: GPL-3.0-or-later
#include "dumpregistry.h"

#include <QMap>

namespace ControlCenter {

namespace {
QMap<QString, DumpFn> &registry()
{
    static QMap<QString, DumpFn> map;   // function-local: safe during static initialisation
    return map;
}
} // namespace

void registerDump(const QString &topic, DumpFn fn)
{
    registry().insert(topic, std::move(fn));
}

QStringList dumpTopics()
{
    return registry().keys();
}

bool runDump(const QString &topic, QJsonValue *out)
{
    const auto it = registry().constFind(topic);
    if (it == registry().constEnd())
        return false;
    *out = (*it)();
    return true;
}

} // namespace ControlCenter
