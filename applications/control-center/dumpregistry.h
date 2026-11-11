// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <functional>

// Registry behind "shadow-control-center --dump <topic>".
//
// Each back-end registers the data it shows, so the integration tests can
// compare it with the system's own tools without driving the GUI:
//
//     SHADOW_CC_DUMP("datetime", [] { return DateTimeBackend().toJson(); })
//
// A dump function runs on the main thread before any window exists. A
// back-end that gathers data asynchronously may spin a local QEventLoop
// (with a timeout) until its data is ready.
namespace ControlCenter {

using DumpFn = std::function<QJsonValue()>;

void registerDump(const QString &topic, DumpFn fn);
QStringList dumpTopics();
bool runDump(const QString &topic, QJsonValue *out);

struct DumpRegistrar
{
    DumpRegistrar(const char *topic, DumpFn fn) { registerDump(QString::fromLatin1(topic), std::move(fn)); }
};

} // namespace ControlCenter

#define SHADOW_CC_CONCAT_(a, b) a##b
#define SHADOW_CC_CONCAT(a, b) SHADOW_CC_CONCAT_(a, b)
#define SHADOW_CC_DUMP(topic, fn) \
    static const ControlCenter::DumpRegistrar SHADOW_CC_CONCAT(shadowCcDump_, __LINE__)(topic, fn);
