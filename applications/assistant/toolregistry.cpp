// SPDX-License-Identifier: GPL-3.0-or-later
#include "toolregistry.h"

#include "assistantconfig.h"

#include <command.h>

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>

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

    done({false, tr("Unknown tool \"%1\".").arg(call.name)});
}
