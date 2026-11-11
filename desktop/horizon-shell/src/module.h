// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shell modules: optional parts of the shell (notifications, system tray,
// quick settings, ...) that live in modules/<name>/ and plug into the core
// without the core knowing them.
//
// A module consists of
//   modules/<name>/*.cpp, *.h     C++ back-end, compiled into horizon-shell
//   modules/<name>/qml/*.qml      QML, part of the org.shadow.horizon module
//   modules/<name>/module.cmake   optional: find_package / target_link_libraries
//   modules/<name>/smoke-tests.list (optional, see tests/desktop/smoke.sh)
//
// and registers itself with
//
//   static void init(Horizon::ShellContext &ctx)
//   {
//       auto *server = new NotificationServer(ctx.parent());
//       ctx.expose(QStringLiteral("notifications"), server);
//       ctx.addPanelItem(QStringLiteral("NotificationButton.qml"), 40);
//       ctx.addSurface(QStringLiteral("NotificationPopups.qml"));
//   }
//   HORIZON_SHELL_MODULE(notifications, init)
//
// See docs/desktop/shell-modules.md.
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVariantList>

#include <functional>

namespace Shadow {
class App;
}
class Applications;
class Session;
class ShellSettings;
class Shortcuts;
class WindowModel;
class Workspaces;

namespace Horizon {

class ShellModules;

class ShellContext
{
public:
    // Core services a module may use. Never null while a module's init runs.
    Shadow::App *app = nullptr;
    ShellSettings *settings = nullptr;
    Applications *applications = nullptr;
    WindowModel *windows = nullptr;
    Workspaces *workspaces = nullptr;
    Session *session = nullptr;
    Shortcuts *shortcuts = nullptr;

    // Owner for objects that should live as long as the shell.
    QObject *parent() const;

    // Makes 'object' available to all shell QML under 'name' (a context property).
    void expose(const QString &name, QObject *object);

    // Adds an item to the right side of the panel, left of the session button.
    // 'qmlFile' is a file of the module's qml/ directory ("VolumeButton.qml");
    // items are ordered by 'order', lowest leftmost. The item is created with
    // the initial properties { shell, panel }.
    void addPanelItem(const QString &qmlFile, int order);

    // Instantiates a top-level component once when the shell starts (a popup
    // surface, an on-screen display). Created with the initial property { shell }.
    void addSurface(const QString &qmlFile);

private:
    friend class ShellModules;
    ShellModules *m_modules = nullptr;
};

using ModuleInit = void (*)(ShellContext &);
// Dump handler for "horizon-shell --dump <topic>": returns the JSON document.
// It runs without a compositor and without the core services (ctx is not
// available) — construct what you need yourself.
using ModuleDump = std::function<QJsonObject()>;

struct ModuleRegistration {
    ModuleRegistration(const char *name, ModuleInit init);
};
struct DumpRegistration {
    DumpRegistration(const char *topic, ModuleDump dump);
};

// The registry, exposed to QML as "shellModules".
class ShellModules : public QObject
{
    Q_OBJECT
    // [{ source: url, order: int }] sorted by order
    Q_PROPERTY(QVariantList panelItems READ panelItems CONSTANT)
    // [url]
    Q_PROPERTY(QVariantList surfaces READ surfaces CONSTANT)
    Q_PROPERTY(QStringList names READ names CONSTANT)

public:
    explicit ShellModules(QObject *parent = nullptr);

    // Runs every registered module's init function.
    void initialize(ShellContext &context);

    QVariantList panelItems() const;
    QVariantList surfaces() const { return m_surfaces; }
    QStringList names() const;

    static QStringList dumpTopics();
    static bool hasDump(const QString &topic);
    static QJsonObject dump(const QString &topic);

private:
    friend class ShellContext;
    struct PanelItem { QString source; int order; };
    QList<PanelItem> m_panelItems;
    QVariantList m_surfaces;
    Shadow::App *m_app = nullptr;
};

} // namespace Horizon

#define HORIZON_SHELL_MODULE(name, initFunction) \
    static const Horizon::ModuleRegistration horizonShellModule_##name(#name, initFunction);

#define HORIZON_SHELL_DUMP(identifier, topic, dumpFunction) \
    static const Horizon::DumpRegistration horizonShellDump_##identifier(topic, dumpFunction);
