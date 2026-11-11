// SPDX-License-Identifier: GPL-3.0-or-later
#include "toolregistry.h"

#include "assistantconfig.h"

#include <command.h>

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>

using Shadow::Command;
using Shadow::CommandResult;

namespace {

QJsonObject obj(std::initializer_list<QPair<QString, QJsonValue>> items)
{
    QJsonObject o;
    for (const auto &it : items)
        o.insert(it.first, it.second);
    return o;
}

QJsonObject strProp(const QString &desc)
{
    return obj({{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("description"), desc}});
}

// Expand a leading ~ to the user's home; leave the rest untouched.
QString expandUser(const QString &path)
{
    if (path == QLatin1String("~"))
        return QDir::homePath();
    if (path.startsWith(QLatin1String("~/")))
        return QDir::homePath() + path.mid(1);
    return path;
}

constexpr qint64 kMaxReadBytes = 256 * 1024;

// Resolve the command used to open a URL/path in the default handler. Prefers
// xdg-open; falls back to `gio open`. Returns an argv or empty if neither tool
// is present (reported honestly to the model).
QStringList openerArgv(const QString &target)
{
    if (Shadow::Command::available(QStringLiteral("xdg-open")))
        return {QStringLiteral("xdg-open"), target};
    if (Shadow::Command::available(QStringLiteral("gio")))
        return {QStringLiteral("gio"), QStringLiteral("open"), target};
    return {};
}

// Refuse obviously catastrophic delete/move targets even though the user also
// confirms. Returns a reason if the path must be refused, otherwise empty.
QString dangerousPathReason(const QString &rawPath)
{
    const QString p = QDir::cleanPath(expandUser(rawPath));
    if (p.isEmpty())
        return QObject::tr("Empty path.");
    if (p == QLatin1String("/") || p == QDir::rootPath())
        return QObject::tr("Refusing to act on the filesystem root.");
    if (p == QDir::homePath())
        return QObject::tr("Refusing to act on the whole home directory.");
    // A bare top-level directory like /usr, /etc, /bin …
    if (p.count(QLatin1Char('/')) == 1 && p.startsWith(QLatin1Char('/')) && p.size() > 1)
        return QObject::tr("Refusing to act on a top-level system directory (%1).").arg(p);
    return {};
}

} // namespace

ToolRegistry::ToolRegistry(AssistantConfig *config, QObject *parent)
    : QObject(parent), m_config(config)
{
    m_specs = {
        {
            QStringLiteral("run_shell_command"),
            QStringLiteral("Run a command on the computer. Provide 'command' as the program name and "
                           "'args' as an array of its arguments. The command is run directly (no shell), "
                           "so shell features like pipes, globbing or redirection are not interpreted."),
            obj({{QStringLiteral("type"), QStringLiteral("object")},
                 {QStringLiteral("properties"), obj({
                      {QStringLiteral("command"), strProp(QStringLiteral("The program to run, e.g. 'ls' or 'systemctl'. "
                                                                        "May include arguments; it is split into an argv safely."))},
                      {QStringLiteral("args"), obj({{QStringLiteral("type"), QStringLiteral("array")},
                                                    {QStringLiteral("items"), obj({{QStringLiteral("type"), QStringLiteral("string")}})},
                                                    {QStringLiteral("description"), QStringLiteral("Arguments, each a separate string.")}})},
                  })},
                 {QStringLiteral("required"), QJsonArray{QStringLiteral("command")}}}),
            true, true,
        },
        {
            QStringLiteral("read_file"),
            QStringLiteral("Read and return the text contents of a file (first 256 KiB)."),
            obj({{QStringLiteral("type"), QStringLiteral("object")},
                 {QStringLiteral("properties"), obj({{QStringLiteral("path"), strProp(QStringLiteral("Absolute or ~-relative file path."))}})},
                 {QStringLiteral("required"), QJsonArray{QStringLiteral("path")}}}),
            false, false,
        },
        {
            QStringLiteral("write_file"),
            QStringLiteral("Create or overwrite a text file with the given contents."),
            obj({{QStringLiteral("type"), QStringLiteral("object")},
                 {QStringLiteral("properties"), obj({
                      {QStringLiteral("path"), strProp(QStringLiteral("Absolute or ~-relative file path."))},
                      {QStringLiteral("content"), strProp(QStringLiteral("The full contents to write."))},
                  })},
                 {QStringLiteral("required"), QJsonArray{QStringLiteral("path"), QStringLiteral("content")}}}),
            true, false,
        },
        {
            QStringLiteral("search_files"),
            QStringLiteral("Search for a text pattern inside files under a directory (like grep -rn). "
                           "Returns matching lines with their file and line number."),
            obj({{QStringLiteral("type"), QStringLiteral("object")},
                 {QStringLiteral("properties"), obj({
                      {QStringLiteral("directory"), strProp(QStringLiteral("Directory to search in."))},
                      {QStringLiteral("pattern"), strProp(QStringLiteral("Fixed-string text to search for."))},
                  })},
                 {QStringLiteral("required"), QJsonArray{QStringLiteral("directory"), QStringLiteral("pattern")}}}),
            false, false,
        },
        {
            QStringLiteral("open_application"),
            QStringLiteral("Launch an installed application by its desktop id (e.g. 'org.shadow.files')."),
            obj({{QStringLiteral("type"), QStringLiteral("object")},
                 {QStringLiteral("properties"), obj({{QStringLiteral("desktop_id"), strProp(QStringLiteral("Desktop entry id, with or without the .desktop suffix."))}})},
                 {QStringLiteral("required"), QJsonArray{QStringLiteral("desktop_id")}}}),
            true, false,
        },
        {
            QStringLiteral("get_system_info"),
            QStringLiteral("Return a short summary of the computer: OS, kernel, architecture, host name, memory."),
            obj({{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("properties"), QJsonObject{}}}),
            false, false,
        },
        {
            QStringLiteral("open_url"),
            QStringLiteral("Open a URL (web page, file:// path, mailto:, …) in the default browser/handler."),
            obj({{QStringLiteral("type"), QStringLiteral("object")},
                 {QStringLiteral("properties"), obj({{QStringLiteral("url"), strProp(QStringLiteral("The URL to open, e.g. 'https://example.com'."))}})},
                 {QStringLiteral("required"), QJsonArray{QStringLiteral("url")}}}),
            true, false,
        },
        {
            QStringLiteral("web_search"),
            QStringLiteral("Open the default browser on a web-search results page for a query. Does not scrape; the user sees the results in their browser."),
            obj({{QStringLiteral("type"), QStringLiteral("object")},
                 {QStringLiteral("properties"), obj({{QStringLiteral("query"), strProp(QStringLiteral("What to search for."))}})},
                 {QStringLiteral("required"), QJsonArray{QStringLiteral("query")}}}),
            true, false,
        },
        {
            QStringLiteral("make_directory"),
            QStringLiteral("Create a directory (and any missing parents)."),
            obj({{QStringLiteral("type"), QStringLiteral("object")},
                 {QStringLiteral("properties"), obj({{QStringLiteral("path"), strProp(QStringLiteral("Directory path to create."))}})},
                 {QStringLiteral("required"), QJsonArray{QStringLiteral("path")}}}),
            true, false,
        },
        {
            QStringLiteral("move_path"),
            QStringLiteral("Move or rename a file or folder from 'source' to 'destination'."),
            obj({{QStringLiteral("type"), QStringLiteral("object")},
                 {QStringLiteral("properties"), obj({
                      {QStringLiteral("source"), strProp(QStringLiteral("Existing file or folder."))},
                      {QStringLiteral("destination"), strProp(QStringLiteral("New path or target folder."))},
                  })},
                 {QStringLiteral("required"), QJsonArray{QStringLiteral("source"), QStringLiteral("destination")}}}),
            true, true,
        },
        {
            QStringLiteral("copy_path"),
            QStringLiteral("Copy a file or folder (recursively) from 'source' to 'destination'."),
            obj({{QStringLiteral("type"), QStringLiteral("object")},
                 {QStringLiteral("properties"), obj({
                      {QStringLiteral("source"), strProp(QStringLiteral("Existing file or folder."))},
                      {QStringLiteral("destination"), strProp(QStringLiteral("Destination path or target folder."))},
                  })},
                 {QStringLiteral("required"), QJsonArray{QStringLiteral("source"), QStringLiteral("destination")}}}),
            true, false,
        },
        {
            QStringLiteral("delete_path"),
            QStringLiteral("Delete a file or folder. This is irreversible; the user must confirm."),
            obj({{QStringLiteral("type"), QStringLiteral("object")},
                 {QStringLiteral("properties"), obj({
                      {QStringLiteral("path"), strProp(QStringLiteral("File or folder to delete."))},
                      {QStringLiteral("recursive"), obj({{QStringLiteral("type"), QStringLiteral("boolean")},
                                                         {QStringLiteral("description"), QStringLiteral("Set true to delete a non-empty folder and its contents.")}})},
                  })},
                 {QStringLiteral("required"), QJsonArray{QStringLiteral("path")}}}),
            true, true,
        },
        {
            QStringLiteral("write_code"),
            QStringLiteral("Write source code (or any text) to a file, optionally opening it in the Shadow editor afterwards."),
            obj({{QStringLiteral("type"), QStringLiteral("object")},
                 {QStringLiteral("properties"), obj({
                      {QStringLiteral("path"), strProp(QStringLiteral("File path to write."))},
                      {QStringLiteral("content"), strProp(QStringLiteral("The full file contents."))},
                      {QStringLiteral("open_in_editor"), obj({{QStringLiteral("type"), QStringLiteral("boolean")},
                                                              {QStringLiteral("description"), QStringLiteral("Open the file in shadow-editor after writing.")}})},
                  })},
                 {QStringLiteral("required"), QJsonArray{QStringLiteral("path"), QStringLiteral("content")}}}),
            true, false,
        },
        {
            QStringLiteral("run_in_terminal"),
            QStringLiteral("Open the Shadow terminal and run a command in it, so the user can watch it and interact. Provide 'command' as a single command line; optional 'workdir' sets the working directory."),
            obj({{QStringLiteral("type"), QStringLiteral("object")},
                 {QStringLiteral("properties"), obj({
                      {QStringLiteral("command"), strProp(QStringLiteral("The command line to run in the terminal."))},
                      {QStringLiteral("workdir"), strProp(QStringLiteral("Optional working directory."))},
                  })},
                 {QStringLiteral("required"), QJsonArray{QStringLiteral("command")}}}),
            true, true,
        },
        {
            QStringLiteral("type_text"),
            QStringLiteral("Type text into the currently focused window (dictation / coding into the active editor), as if typed on the keyboard."),
            obj({{QStringLiteral("type"), QStringLiteral("object")},
                 {QStringLiteral("properties"), obj({{QStringLiteral("text"), strProp(QStringLiteral("The text to type into the focused window."))}})},
                 {QStringLiteral("required"), QJsonArray{QStringLiteral("text")}}}),
            true, true,
        },
        {
            QStringLiteral("take_screenshot"),
            QStringLiteral("Capture the screen with shadow-screenshot. 'mode' is 'full' (default), 'window' or 'region'; optional 'output' saves to a file path instead of opening the editor."),
            obj({{QStringLiteral("type"), QStringLiteral("object")},
                 {QStringLiteral("properties"), obj({
                      {QStringLiteral("mode"), strProp(QStringLiteral("'full', 'window' or 'region'."))},
                      {QStringLiteral("output"), strProp(QStringLiteral("Optional output file path (.png/.jpg/.webp)."))},
                  })}}),
            true, false,
        },
    };
}

const ToolSpec *ToolRegistry::spec(const QString &name) const
{
    for (const ToolSpec &s : m_specs)
        if (s.name == name)
            return &s;
    return nullptr;
}

bool ToolRegistry::toolChangesSystem(const QString &name) const
{
    const ToolSpec *s = spec(name);
    return s && s->changesSystem;
}

bool ToolRegistry::toolRisky(const QString &name) const
{
    const ToolSpec *s = spec(name);
    return s && s->risky;
}

QJsonArray ToolRegistry::apiTools() const
{
    QJsonArray out;
    for (const ToolSpec &s : m_specs) {
        if (!m_config->toolEnabled(s.name))
            continue;
        out.append(obj({
            {QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"), obj({
                 {QStringLiteral("name"), s.name},
                 {QStringLiteral("description"), s.description},
                 {QStringLiteral("parameters"), s.parameters},
             })},
        }));
    }
    return out;
}

bool ToolRegistry::hasEnabledTools() const
{
    for (const ToolSpec &s : m_specs)
        if (m_config->toolEnabled(s.name))
            return true;
    return false;
}

QStringList ToolRegistry::resolvedArgv(const ToolInvocation &call) const
{
    if (call.name != QLatin1String("run_shell_command"))
        return {};
    const QString command = call.args.value(QStringLiteral("command")).toString();
    const QJsonArray argsArr = call.args.value(QStringLiteral("args")).toArray();
    QStringList argv;
    if (!argsArr.isEmpty()) {
        // Model supplied program + explicit argument list: use verbatim.
        argv << command;
        for (const QJsonValue &v : argsArr)
            argv << v.toString();
    } else {
        // Model supplied a single command line: split into an argv WITHOUT a
        // shell (no variable/glob/redirection interpretation).
        argv = QProcess::splitCommand(command);
    }
    argv.removeAll(QString());
    return argv;
}

QString ToolRegistry::summarize(const ToolInvocation &call) const
{
    if (call.name == QLatin1String("run_shell_command"))
        return tr("Run a command");
    if (call.name == QLatin1String("read_file"))
        return tr("Read a file");
    if (call.name == QLatin1String("write_file"))
        return tr("Write a file");
    if (call.name == QLatin1String("search_files"))
        return tr("Search files");
    if (call.name == QLatin1String("open_application"))
        return tr("Open an application");
    if (call.name == QLatin1String("get_system_info"))
        return tr("Read system information");
    if (call.name == QLatin1String("open_url"))
        return tr("Open a URL in the browser");
    if (call.name == QLatin1String("web_search"))
        return tr("Search the web");
    if (call.name == QLatin1String("make_directory"))
        return tr("Create a folder");
    if (call.name == QLatin1String("move_path"))
        return tr("Move / rename");
    if (call.name == QLatin1String("copy_path"))
        return tr("Copy");
    if (call.name == QLatin1String("delete_path"))
        return tr("Delete");
    if (call.name == QLatin1String("write_code"))
        return tr("Write code to a file");
    if (call.name == QLatin1String("run_in_terminal"))
        return tr("Run in the terminal");
    if (call.name == QLatin1String("type_text"))
        return tr("Type into the focused window");
    if (call.name == QLatin1String("take_screenshot"))
        return tr("Take a screenshot");
    return call.name;
}

QString ToolRegistry::preview(const ToolInvocation &call) const
{
    if (call.name == QLatin1String("run_shell_command")) {
        const QStringList argv = resolvedArgv(call);
        return argv.join(QLatin1Char(' '));
    }
    if (call.name == QLatin1String("read_file"))
        return call.args.value(QStringLiteral("path")).toString();
    if (call.name == QLatin1String("write_file")) {
        const QString path = call.args.value(QStringLiteral("path")).toString();
        const QString content = call.args.value(QStringLiteral("content")).toString();
        return tr("%1  (%n byte(s))", nullptr, int(content.toUtf8().size())).arg(path);
    }
    if (call.name == QLatin1String("search_files"))
        return tr("\"%1\" in %2").arg(call.args.value(QStringLiteral("pattern")).toString(),
                                      call.args.value(QStringLiteral("directory")).toString());
    if (call.name == QLatin1String("open_application"))
        return call.args.value(QStringLiteral("desktop_id")).toString();
    if (call.name == QLatin1String("open_url"))
        return call.args.value(QStringLiteral("url")).toString();
    if (call.name == QLatin1String("web_search"))
        return call.args.value(QStringLiteral("query")).toString();
    if (call.name == QLatin1String("make_directory"))
        return call.args.value(QStringLiteral("path")).toString();
    if (call.name == QLatin1String("move_path") || call.name == QLatin1String("copy_path")) {
        const QString op = call.name == QLatin1String("move_path") ? QStringLiteral("mv") : QStringLiteral("cp -r");
        return QStringLiteral("%1  %2  →  %3").arg(op,
            call.args.value(QStringLiteral("source")).toString(),
            call.args.value(QStringLiteral("destination")).toString());
    }
    if (call.name == QLatin1String("delete_path")) {
        const bool rec = call.args.value(QStringLiteral("recursive")).toBool();
        return (rec ? QStringLiteral("rm -r  ") : QStringLiteral("rm  ")) + call.args.value(QStringLiteral("path")).toString();
    }
    if (call.name == QLatin1String("write_code")) {
        const QString path = call.args.value(QStringLiteral("path")).toString();
        const QString content = call.args.value(QStringLiteral("content")).toString();
        const bool open = call.args.value(QStringLiteral("open_in_editor")).toBool();
        return tr("%1  (%n byte(s))%2", nullptr, int(content.toUtf8().size()))
            .arg(path, open ? tr(", then open in editor") : QString());
    }
    if (call.name == QLatin1String("run_in_terminal")) {
        const QString wd = call.args.value(QStringLiteral("workdir")).toString();
        const QString cmd = call.args.value(QStringLiteral("command")).toString();
        return wd.isEmpty() ? cmd : QStringLiteral("(%1) %2").arg(wd, cmd);
    }
    if (call.name == QLatin1String("type_text")) {
        QString t = call.args.value(QStringLiteral("text")).toString();
        if (t.size() > 400)
            t = t.left(400) + QStringLiteral("…");
        return t;
    }
    if (call.name == QLatin1String("take_screenshot")) {
        const QString mode = call.args.value(QStringLiteral("mode")).toString();
        const QString out = call.args.value(QStringLiteral("output")).toString();
        return (mode.isEmpty() ? QStringLiteral("full") : mode) + (out.isEmpty() ? QString() : QStringLiteral("  →  ") + out);
    }
    return {};
}

void ToolRegistry::execute(const ToolInvocation &call, Done done)
{
    if (!m_config->toolEnabled(call.name)) {
        done({false, tr("The tool \"%1\" is disabled in settings.").arg(call.name)});
        return;
    }

    if (call.name == QLatin1String("run_shell_command")) {
        const QStringList argv = resolvedArgv(call);
        if (argv.isEmpty()) {
            done({false, tr("Empty command.")});
            return;
        }
        const QString program = argv.first();
        const QStringList args = argv.mid(1);
        Command::run(this, program, args, [done](const CommandResult &r) {
            QString body = r.outText();
            const QString err = r.errText();
            if (!err.isEmpty())
                body += (body.isEmpty() ? QString() : QStringLiteral("\n")) + QStringLiteral("[stderr]\n") + err;
            if (body.size() > 16000)
                body = body.left(16000) + QStringLiteral("\n…(truncated)");
            const QString header = r.exitCode == -1 ? QObject::tr("Command could not be started.")
                : r.exitCode == -2 ? QObject::tr("Command timed out.")
                : QObject::tr("Exit code %1.").arg(r.exitCode);
            done({r.exitCode == 0, header + QStringLiteral("\n") + body});
        }, 60000);
        return;
    }

    if (call.name == QLatin1String("read_file")) {
        const QString path = expandUser(call.args.value(QStringLiteral("path")).toString());
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            done({false, tr("Could not open %1: %2").arg(path, f.errorString())});
            return;
        }
        const QByteArray data = f.read(kMaxReadBytes);
        QString text = QString::fromUtf8(data);
        if (f.pos() >= kMaxReadBytes)
            text += QStringLiteral("\n…(truncated at 256 KiB)");
        done({true, text});
        return;
    }

    if (call.name == QLatin1String("write_file")) {
        const QString path = expandUser(call.args.value(QStringLiteral("path")).toString());
        const QString content = call.args.value(QStringLiteral("content")).toString();
        QFileInfo fi(path);
        QDir().mkpath(fi.absolutePath());
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            done({false, tr("Could not write %1: %2").arg(path, f.errorString())});
            return;
        }
        const qint64 n = f.write(content.toUtf8());
        f.close();
        done({n >= 0, tr("Wrote %n byte(s) to %1.", nullptr, int(content.toUtf8().size())).arg(path)});
        return;
    }

    if (call.name == QLatin1String("search_files")) {
        const QString dir = expandUser(call.args.value(QStringLiteral("directory")).toString());
        const QString pattern = call.args.value(QStringLiteral("pattern")).toString();
        if (pattern.isEmpty()) {
            done({false, tr("Empty search pattern.")});
            return;
        }
        // grep with fixed strings; pattern is passed as an argument, never via a shell.
        Command::run(this, QStringLiteral("grep"),
                     {QStringLiteral("-rnI"), QStringLiteral("--max-count=50"), QStringLiteral("-F"),
                      QStringLiteral("-e"), pattern, dir},
                     [done](const CommandResult &r) {
            QString body = r.outText();
            if (body.size() > 16000)
                body = body.left(16000) + QStringLiteral("\n…(truncated)");
            if (r.exitCode == 1 && body.isEmpty())
                done({true, QObject::tr("No matches.")});
            else if (r.exitCode == 0 || !body.isEmpty())
                done({true, body});
            else
                done({false, QObject::tr("Search failed: %1").arg(r.errText())});
        }, 30000);
        return;
    }

    if (call.name == QLatin1String("open_application")) {
        QString id = call.args.value(QStringLiteral("desktop_id")).toString();
        if (id.endsWith(QLatin1String(".desktop")))
            id.chop(8);
        // Launch via gtk-launch (respects the desktop entry); argv, no shell.
        if (!Command::available(QStringLiteral("gtk-launch"))) {
            done({false, tr("gtk-launch is not available to open applications.")});
            return;
        }
        Command::run(this, QStringLiteral("gtk-launch"), {id},
                     [done, id](const CommandResult &r) {
            if (r.exitCode == 0)
                done({true, QObject::tr("Launched %1.").arg(id)});
            else
                done({false, QObject::tr("Could not launch %1: %2").arg(id, r.errText().isEmpty() ? QObject::tr("unknown error") : r.errText())});
        }, 15000);
        return;
    }

    if (call.name == QLatin1String("get_system_info")) {
        QString out;
        QTextStream s(&out);
        auto readFirst = [](const QString &path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
        };
        s << "host: " << QSysInfo::machineHostName() << '\n';
        s << "os: " << QSysInfo::prettyProductName() << '\n';
        s << "kernel: " << QSysInfo::kernelType() << ' ' << QSysInfo::kernelVersion() << '\n';
        s << "arch: " << QSysInfo::currentCpuArchitecture() << '\n';
        const QString mem = readFirst(QStringLiteral("/proc/meminfo"));
        for (const QString &line : mem.split(QLatin1Char('\n')))
            if (line.startsWith(QLatin1String("MemTotal")) || line.startsWith(QLatin1String("MemAvailable")))
                s << line.simplified() << '\n';
        done({true, out});
        return;
    }

    if (call.name == QLatin1String("open_url") || call.name == QLatin1String("web_search")) {
        QString target;
        if (call.name == QLatin1String("open_url")) {
            target = call.args.value(QStringLiteral("url")).toString().trimmed();
            if (target.isEmpty()) {
                done({false, tr("No URL given.")});
                return;
            }
            // Add a scheme for a bare host so the handler treats it as a URL.
            if (!target.contains(QStringLiteral("://")) && !target.startsWith(QLatin1String("mailto:"))
                && !target.startsWith(QLatin1String("file:")))
                target.prepend(QStringLiteral("https://"));
        } else {
            const QString q = call.args.value(QStringLiteral("query")).toString().trimmed();
            if (q.isEmpty()) {
                done({false, tr("Empty search query.")});
                return;
            }
            target = QStringLiteral("https://duckduckgo.com/?q=") + QString::fromUtf8(QUrl::toPercentEncoding(q));
        }
        const QStringList argv = openerArgv(target);
        if (argv.isEmpty()) {
            done({false, tr("No URL handler is available (neither xdg-open nor gio).")});
            return;
        }
        Command::run(this, argv.first(), argv.mid(1), [done, target](const CommandResult &r) {
            if (r.exitCode == 0)
                done({true, QObject::tr("Opened %1 in the browser.").arg(target)});
            else
                done({false, QObject::tr("Could not open %1: %2").arg(target,
                    r.errText().isEmpty() ? QObject::tr("exit code %1").arg(r.exitCode) : r.errText())});
        }, 15000);
        return;
    }

    if (call.name == QLatin1String("make_directory")) {
        const QString path = expandUser(call.args.value(QStringLiteral("path")).toString());
        if (path.trimmed().isEmpty()) {
            done({false, tr("No path given.")});
            return;
        }
        if (QDir().mkpath(path))
            done({true, tr("Created directory %1.").arg(path)});
        else
            done({false, tr("Could not create directory %1.").arg(path)});
        return;
    }

    if (call.name == QLatin1String("move_path") || call.name == QLatin1String("copy_path")) {
        const QString src = expandUser(call.args.value(QStringLiteral("source")).toString());
        const QString dst = expandUser(call.args.value(QStringLiteral("destination")).toString());
        if (src.trimmed().isEmpty() || dst.trimmed().isEmpty()) {
            done({false, tr("Both source and destination are required.")});
            return;
        }
        const QString reason = dangerousPathReason(src);
        if (!reason.isEmpty()) {
            done({false, reason});
            return;
        }
        if (!QFileInfo::exists(src)) {
            done({false, tr("Source does not exist: %1").arg(src)});
            return;
        }
        const bool move = call.name == QLatin1String("move_path");
        const QString program = move ? QStringLiteral("mv") : QStringLiteral("cp");
        QStringList args;
        if (!move)
            args << QStringLiteral("-r");
        args << QStringLiteral("--") << src << dst;
        Command::run(this, program, args, [done, move, src, dst](const CommandResult &r) {
            if (r.exitCode == 0)
                done({true, (move ? QObject::tr("Moved %1 to %2.") : QObject::tr("Copied %1 to %2.")).arg(src, dst)});
            else
                done({false, r.errText().isEmpty() ? QObject::tr("Failed (exit %1).").arg(r.exitCode) : r.errText()});
        }, 60000);
        return;
    }

    if (call.name == QLatin1String("delete_path")) {
        const QString path = expandUser(call.args.value(QStringLiteral("path")).toString());
        if (path.trimmed().isEmpty()) {
            done({false, tr("No path given.")});
            return;
        }
        const QString reason = dangerousPathReason(path);
        if (!reason.isEmpty()) {
            done({false, reason});
            return;
        }
        if (!QFileInfo::exists(path)) {
            done({false, tr("Nothing to delete at %1.").arg(path)});
            return;
        }
        const bool recursive = call.args.value(QStringLiteral("recursive")).toBool();
        QStringList args;
        args << (recursive ? QStringLiteral("-rf") : QStringLiteral("-f")) << QStringLiteral("--") << path;
        Command::run(this, QStringLiteral("rm"), args, [done, path](const CommandResult &r) {
            if (r.exitCode == 0)
                done({true, QObject::tr("Deleted %1.").arg(path)});
            else
                done({false, r.errText().isEmpty() ? QObject::tr("Failed (exit %1).").arg(r.exitCode) : r.errText()});
        }, 60000);
        return;
    }

    if (call.name == QLatin1String("write_code")) {
        const QString path = expandUser(call.args.value(QStringLiteral("path")).toString());
        const QString content = call.args.value(QStringLiteral("content")).toString();
        const bool open = call.args.value(QStringLiteral("open_in_editor")).toBool();
        if (path.trimmed().isEmpty()) {
            done({false, tr("No path given.")});
            return;
        }
        QFileInfo fi(path);
        QDir().mkpath(fi.absolutePath());
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            done({false, tr("Could not write %1: %2").arg(path, f.errorString())});
            return;
        }
        f.write(content.toUtf8());
        f.close();
        const QString wrote = tr("Wrote %n byte(s) to %1.", nullptr, int(content.toUtf8().size())).arg(path);
        if (open && Command::available(QStringLiteral("shadow-editor"))) {
            Command::run(this, QStringLiteral("shadow-editor"), {path}, [done, wrote](const CommandResult &) {
                done({true, wrote + QStringLiteral(" ") + QObject::tr("Opened it in the editor.")});
            }, 15000);
        } else if (open) {
            done({true, wrote + QStringLiteral(" ") + tr("(shadow-editor is not available to open it.)")});
        } else {
            done({true, wrote});
        }
        return;
    }

    if (call.name == QLatin1String("run_in_terminal")) {
        const QString command = call.args.value(QStringLiteral("command")).toString().trimmed();
        const QString workdir = expandUser(call.args.value(QStringLiteral("workdir")).toString());
        if (command.isEmpty()) {
            done({false, tr("No command given.")});
            return;
        }
        if (!Command::available(QStringLiteral("shadow-terminal"))) {
            done({false, tr("shadow-terminal is not available.")});
            return;
        }
        QStringList args;
        if (!workdir.trimmed().isEmpty())
            args << QStringLiteral("--workdir") << workdir;
        args << QStringLiteral("-e") << command;
        Command::run(this, QStringLiteral("shadow-terminal"), args, [done, command](const CommandResult &r) {
            if (r.exitCode == 0)
                done({true, QObject::tr("Opened the terminal running: %1").arg(command)});
            else
                done({false, r.errText().isEmpty() ? QObject::tr("Failed (exit %1).").arg(r.exitCode) : r.errText()});
        }, 15000);
        return;
    }

    if (call.name == QLatin1String("type_text")) {
        const QString text = call.args.value(QStringLiteral("text")).toString();
        if (text.isEmpty()) {
            done({false, tr("No text to type.")});
            return;
        }
        // Wayland first (Shadow OS is Wayland), X11 fallback. argv, never a shell.
        if (Command::available(QStringLiteral("wtype"))) {
            Command::run(this, QStringLiteral("wtype"), {QStringLiteral("--"), text}, [done](const CommandResult &r) {
                if (r.exitCode == 0)
                    done({true, QObject::tr("Typed the text into the focused window.")});
                else
                    done({false, r.errText().isEmpty() ? QObject::tr("wtype failed (exit %1).").arg(r.exitCode) : r.errText()});
            }, 20000);
        } else if (Command::available(QStringLiteral("xdotool"))) {
            Command::run(this, QStringLiteral("xdotool"), {QStringLiteral("type"), QStringLiteral("--clearmodifiers"), text},
                         [done](const CommandResult &r) {
                if (r.exitCode == 0)
                    done({true, QObject::tr("Typed the text into the focused window (xdotool/X11).")});
                else
                    done({false, r.errText().isEmpty() ? QObject::tr("xdotool failed (exit %1).").arg(r.exitCode) : r.errText()});
            }, 20000);
        } else {
            done({false, tr("Neither wtype (Wayland) nor xdotool (X11) is available to type text.")});
        }
        return;
    }

    if (call.name == QLatin1String("take_screenshot")) {
        if (!Command::available(QStringLiteral("shadow-screenshot"))) {
            done({false, tr("shadow-screenshot is not available.")});
            return;
        }
        QString mode = call.args.value(QStringLiteral("mode")).toString().trimmed().toLower();
        const QString output = expandUser(call.args.value(QStringLiteral("output")).toString());
        QStringList args;
        if (mode == QLatin1String("window"))
            args << QStringLiteral("--window");
        else if (mode == QLatin1String("region"))
            args << QStringLiteral("--region");
        else
            args << QStringLiteral("--full");
        if (!output.trimmed().isEmpty())
            args << QStringLiteral("--output") << output;
        Command::run(this, QStringLiteral("shadow-screenshot"), args, [done, output](const CommandResult &r) {
            if (r.exitCode == 0)
                done({true, output.isEmpty() ? QObject::tr("Took a screenshot.")
                                             : QObject::tr("Saved a screenshot to %1.").arg(output)});
            else
                done({false, r.errText().isEmpty() ? QObject::tr("Screenshot failed (exit %1).").arg(r.exitCode) : r.errText()});
        }, 70000);
        return;
    }

    done({false, tr("Unknown tool \"%1\".").arg(call.name)});
}
