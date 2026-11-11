// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QJsonArray>
#include <QKeySequence>
#include <QList>
#include <QString>

// One user-triggerable command of the application.
struct ActionDef
{
    QString id;                         // stable key ("new-tab")
    QString text;                       // menu text
    QString category;                   // group in menus and in the shortcut editor
    QString icon;                       // symbolic icon name, may be empty
    QList<QKeySequence> defaults;       // at most two: primary and alternative binding
    bool checkable = false;
};

// The table of all commands and their key bindings.
//
// Bindings the user changed are stored in the [Shortcuts] section of
// terminal.conf, one entry per command ("none" = deliberately unbound);
// commands without an entry use their defaults.
namespace Shortcuts {

constexpr int MaxBindings = 2;

const QList<ActionDef> &definitions();
const ActionDef *definition(const QString &id);
QStringList categories();

QList<QKeySequence> defaults(const QString &id);
QList<QKeySequence> current(const QString &id);
bool isCustomized(const QString &id);

// Persist the given bindings for all commands. Only bindings that differ
// from the defaults are written.
void store(const QHash<QString, QList<QKeySequence>> &bindings);

// Text of a binding list for menus, tool tips and the dump ("Ctrl+Shift+T").
QString toText(const QList<QKeySequence> &bindings);

// Data behind "--dump shortcuts".
QJsonArray toJson();

} // namespace Shortcuts
