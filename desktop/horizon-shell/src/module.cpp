// SPDX-License-Identifier: GPL-3.0-or-later
#include "module.h"

#include <shadowapp.h>

#include <QHash>
#include <QUrl>

#include <algorithm>

namespace Horizon {

namespace {

struct Entry { QString name; ModuleInit init; };

// Function-local statics: registrations run during static initialisation,
// in an unspecified order across translation units.
QList<Entry> &moduleList()
{
    static QList<Entry> list;
    return list;
}
QHash<QString, ModuleDump> &dumpTable()
{
    static QHash<QString, ModuleDump> table;
    return table;
}

QString moduleUrl(const QString &qmlFile)
{
    return QStringLiteral("qrc:/qt/qml/org/shadow/horizon/") + qmlFile;
}

} // namespace

ModuleRegistration::ModuleRegistration(const char *name, ModuleInit init)
{
    moduleList().append({QString::fromLatin1(name), init});
}

DumpRegistration::DumpRegistration(const char *topic, ModuleDump dump)
{
    dumpTable().insert(QString::fromLatin1(topic), std::move(dump));
}

QObject *ShellContext::parent() const
{
    return m_modules;
}

void ShellContext::expose(const QString &name, QObject *object)
{
    app->expose(name, object);
}

void ShellContext::addPanelItem(const QString &qmlFile, int order)
{
    m_modules->m_panelItems.append({moduleUrl(qmlFile), order});
}

void ShellContext::addSurface(const QString &qmlFile)
{
    m_modules->m_surfaces.append(moduleUrl(qmlFile));
}

ShellModules::ShellModules(QObject *parent)
    : QObject(parent)
{
}

void ShellModules::initialize(ShellContext &context)
{
    context.m_modules = this;
    m_app = context.app;
    // A stable order (by name) so behaviour does not depend on link order.
    QList<Entry> modules = moduleList();
    std::sort(modules.begin(), modules.end(), [](const Entry &a, const Entry &b) { return a.name < b.name; });
    for (const Entry &module : std::as_const(modules))
        module.init(context);
}

QVariantList ShellModules::panelItems() const
{
    QList<PanelItem> items = m_panelItems;
    std::stable_sort(items.begin(), items.end(), [](const PanelItem &a, const PanelItem &b) { return a.order < b.order; });
    QVariantList out;
    for (const PanelItem &item : std::as_const(items))
        out << QVariantMap{{QStringLiteral("source"), item.source}, {QStringLiteral("order"), item.order}};
    return out;
}

QStringList ShellModules::names() const
{
    QStringList out;
    for (const Entry &module : std::as_const(moduleList()))
        out << module.name;
    out.sort();
    return out;
}

QStringList ShellModules::dumpTopics()
{
    QStringList topics = dumpTable().keys();
    topics.sort();
    return topics;
}

bool ShellModules::hasDump(const QString &topic)
{
    return dumpTable().contains(topic);
}

QJsonObject ShellModules::dump(const QString &topic)
{
    return dumpTable().value(topic)();
}

} // namespace Horizon
