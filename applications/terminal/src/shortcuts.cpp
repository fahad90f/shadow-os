// SPDX-License-Identifier: GPL-3.0-or-later
#include "shortcuts.h"

#include "settings.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QSettings>

namespace Shortcuts {

namespace {

const char kGroup[] = "Shortcuts";
const char kUnbound[] = "none";

QString tr(const char *text)
{
    return QCoreApplication::translate("Shortcuts", text);
}

QList<QKeySequence> keys(std::initializer_list<const char *> sequences)
{
    QList<QKeySequence> list;
    for (const char *s : sequences)
        list.append(QKeySequence(QString::fromLatin1(s), QKeySequence::PortableText));
    return list;
}

QList<ActionDef> build()
{
    const QString file = tr("File");
    const QString edit = tr("Edit");
    const QString view = tr("View");
    const QString tabs = tr("Tabs");
    const QString panes = tr("Panes");
    const QString process = tr("Process");
    const QString help = tr("Help");
    auto icon = [](const char *name) { return QString::fromLatin1(name); };

    QList<ActionDef> defs = {
        // ---- File ------------------------------------------------------------
        {QStringLiteral("new-tab"), tr("New Tab"), file, icon("plus"), keys({"Ctrl+Shift+T"})},
        {QStringLiteral("new-window"), tr("New Window"), file, icon("window"), keys({"Ctrl+Shift+N"})},
        {QStringLiteral("new-ssh"), tr("New SSH Connection…"), file, icon("server"), keys({"Ctrl+Shift+S"})},
        {QStringLiteral("open-file-manager"), tr("Open File Manager Here"), file, icon("folder-open"),
         keys({"Ctrl+Shift+O"})},
        {QStringLiteral("close-pane"), tr("Close Pane"), file, icon("close"), keys({"Ctrl+Shift+X"})},
        {QStringLiteral("close-tab"), tr("Close Tab"), file, icon("x-circle"), keys({"Ctrl+Shift+W"})},
        {QStringLiteral("quit"), tr("Quit"), file, icon("logout"), keys({"Ctrl+Shift+Q"})},

        // ---- Edit ------------------------------------------------------------
        {QStringLiteral("copy"), tr("Copy"), edit, icon("copy"), keys({"Ctrl+Shift+C", "Ctrl+Ins"})},
        {QStringLiteral("paste"), tr("Paste"), edit, icon("paste"), keys({"Ctrl+Shift+V"})},
        {QStringLiteral("paste-selection"), tr("Paste Selection"), edit, QString(), keys({"Shift+Ins"})},
        {QStringLiteral("find"), tr("Find…"), edit, icon("search"), keys({"Ctrl+Shift+F"})},
        {QStringLiteral("find-next"), tr("Find Next"), edit, icon("chevron-down"), keys({"Ctrl+Shift+G"})},
        {QStringLiteral("find-previous"), tr("Find Previous"), edit, icon("chevron-up"), keys({"Ctrl+Shift+H"})},
        {QStringLiteral("clear"), tr("Clear Scrollback and Reset"), edit, icon("trash"), keys({"Ctrl+Shift+K"})},
        {QStringLiteral("preferences"), tr("Preferences…"), edit, icon("settings"), keys({"Ctrl+,"})},
        {QStringLiteral("shortcuts"), tr("Keyboard Shortcuts…"), edit, icon("keyboard"), {}},

        // ---- View ------------------------------------------------------------
        {QStringLiteral("zoom-in"), tr("Zoom In"), view, icon("zoom-in"), keys({"Ctrl++", "Ctrl+="})},
        {QStringLiteral("zoom-out"), tr("Zoom Out"), view, icon("zoom-out"), keys({"Ctrl+-"})},
        {QStringLiteral("zoom-reset"), tr("Reset Zoom"), view, QString(), keys({"Ctrl+0"})},
        {QStringLiteral("fullscreen"), tr("Full Screen"), view, icon("maximize"), keys({"F11"}), true},
        {QStringLiteral("compact-mode"), tr("Compact Mode"), view, QString(), keys({"Ctrl+Shift+M"}), true},
        {QStringLiteral("status-bar"), tr("Status Bar"), view, QString(), {}, true},

        // ---- Tabs ------------------------------------------------------------
        {QStringLiteral("next-tab"), tr("Next Tab"), tabs, QString(), keys({"Ctrl+PgDown", "Ctrl+Tab"})},
        {QStringLiteral("previous-tab"), tr("Previous Tab"), tabs, QString(),
         keys({"Ctrl+PgUp", "Ctrl+Shift+Tab"})},
        {QStringLiteral("move-tab-left"), tr("Move Tab Left"), tabs, QString(), keys({"Ctrl+Shift+PgUp"})},
        {QStringLiteral("move-tab-right"), tr("Move Tab Right"), tabs, QString(), keys({"Ctrl+Shift+PgDown"})},
        {QStringLiteral("rename-tab"), tr("Rename Tab…"), tabs, icon("edit"), {}},

        // ---- Panes -----------------------------------------------------------
        {QStringLiteral("split-right"), tr("Split Right"), panes, icon("columns"), keys({"Ctrl+Shift+R"})},
        {QStringLiteral("split-down"), tr("Split Down"), panes, icon("rows"), keys({"Ctrl+Shift+D"})},
        {QStringLiteral("focus-left"), tr("Focus Pane to the Left"), panes, QString(), keys({"Alt+Left"})},
        {QStringLiteral("focus-right"), tr("Focus Pane to the Right"), panes, QString(), keys({"Alt+Right"})},
        {QStringLiteral("focus-up"), tr("Focus Pane Above"), panes, QString(), keys({"Alt+Up"})},
        {QStringLiteral("focus-down"), tr("Focus Pane Below"), panes, QString(), keys({"Alt+Down"})},

        // ---- Process ---------------------------------------------------------
        {QStringLiteral("signal-int"), tr("Interrupt (SIGINT)"), process, QString(), {}},
        {QStringLiteral("signal-term"), tr("Terminate (SIGTERM)"), process, QString(), {}},
        {QStringLiteral("signal-kill"), tr("Kill (SIGKILL)…"), process, QString(), {}},
        {QStringLiteral("signal-stop"), tr("Pause (SIGSTOP)"), process, QString(), {}},
        {QStringLiteral("signal-cont"), tr("Resume (SIGCONT)"), process, QString(), {}},

        // ---- Help ------------------------------------------------------------
        {QStringLiteral("about"), tr("About Terminal"), help, icon("info"), {}},
    };

    // Alt+1 … Alt+9 go to a tab by position, after the tab navigation entries.
    qsizetype insertAt = 0;
    for (qsizetype i = 0; i < defs.size(); ++i) {
        if (defs.at(i).id == QLatin1String("rename-tab"))
            insertAt = i + 1;
    }
    for (int n = 1; n <= 9; ++n) {
        ActionDef def;
        def.id = QStringLiteral("tab-%1").arg(n);
        def.text = tr("Switch to Tab %1").arg(n);
        def.category = tabs;
        def.defaults = {QKeySequence(QStringLiteral("Alt+%1").arg(n), QKeySequence::PortableText)};
        defs.insert(insertAt++, def);
    }
    return defs;
}

QList<QKeySequence> parse(const QString &stored)
{
    QList<QKeySequence> list;
    if (stored.trimmed() == QLatin1String(kUnbound))
        return list;
    const QList<QKeySequence> parsed = QKeySequence::listFromString(stored, QKeySequence::PortableText);
    for (const QKeySequence &seq : parsed) {
        if (!seq.isEmpty() && !list.contains(seq) && list.size() < MaxBindings)
            list.append(seq);
    }
    return list;
}

} // namespace

const QList<ActionDef> &definitions()
{
    static const QList<ActionDef> defs = build();
    return defs;
}

const ActionDef *definition(const QString &id)
{
    for (const ActionDef &def : definitions()) {
        if (def.id == id)
            return &def;
    }
    return nullptr;
}

QStringList categories()
{
    QStringList list;
    for (const ActionDef &def : definitions()) {
        if (!list.contains(def.category))
            list.append(def.category);
    }
    return list;
}

QList<QKeySequence> defaults(const QString &id)
{
    const ActionDef *def = definition(id);
    return def ? def->defaults : QList<QKeySequence>();
}

bool isCustomized(const QString &id)
{
    return Settings::instance()->store().contains(QLatin1String(kGroup) + QLatin1Char('/') + id);
}

QList<QKeySequence> current(const QString &id)
{
    QSettings &s = Settings::instance()->store();
    const QString key = QLatin1String(kGroup) + QLatin1Char('/') + id;
    if (!s.contains(key))
        return defaults(id);
    // A value with commas comes back from QSettings as a list; join it again.
    const QVariant value = s.value(key);
    const QString stored = value.typeId() == QMetaType::QStringList
        ? value.toStringList().join(QLatin1Char(','))
        : value.toString();
    return parse(stored);
}

void store(const QHash<QString, QList<QKeySequence>> &bindings)
{
    QSettings &s = Settings::instance()->store();
    s.remove(QLatin1String(kGroup));
    s.beginGroup(QLatin1String(kGroup));
    for (const ActionDef &def : definitions()) {
        if (!bindings.contains(def.id))
            continue;
        const QList<QKeySequence> wanted = bindings.value(def.id);
        if (wanted == def.defaults)
            continue;
        s.setValue(def.id, wanted.isEmpty()
                               ? QString::fromLatin1(kUnbound)
                               : QKeySequence::listToString(wanted, QKeySequence::PortableText));
    }
    s.endGroup();
    s.sync();
}

QString toText(const QList<QKeySequence> &bindings)
{
    QStringList parts;
    for (const QKeySequence &seq : bindings)
        parts << seq.toString(QKeySequence::NativeText);
    return parts.join(QStringLiteral(", "));
}

QJsonArray toJson()
{
    QJsonArray list;
    for (const ActionDef &def : definitions()) {
        auto toArray = [](const QList<QKeySequence> &sequences) {
            QJsonArray array;
            for (const QKeySequence &seq : sequences)
                array.append(seq.toString(QKeySequence::PortableText));
            return array;
        };
        list.append(QJsonObject{
            {QStringLiteral("id"), def.id},
            {QStringLiteral("name"), def.text},
            {QStringLiteral("category"), def.category},
            {QStringLiteral("default"), toArray(def.defaults)},
            {QStringLiteral("current"), toArray(current(def.id))},
            {QStringLiteral("customized"), isCustomized(def.id)},
        });
    }
    return list;
}

} // namespace Shortcuts
