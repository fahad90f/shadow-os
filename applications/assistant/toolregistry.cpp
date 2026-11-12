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

        // ---- Pentest tools (active when role == "pentest") -----------------
        {
            QStringLiteral("pentest_scan"),
            QStringLiteral("Run an Nmap scan against a target with a preset profile. "
                           "'target' is an IP, hostname or CIDR range. "
                           "'profile' is one of: quick (top-100 ports, default), "
                           "full (all 65535 ports), service (version + OS detection), "
                           "vuln (NSE vuln scripts, slower), stealth (SYN scan, low noise)."),
            obj({{QStringLiteral("type"), QStringLiteral("object")},
                 {QStringLiteral("properties"), obj({
                      {QStringLiteral("target"), strProp(QStringLiteral("IP address, hostname or CIDR range to scan."))},
                      {QStringLiteral("profile"), strProp(QStringLiteral("Scan profile: quick, full, service, vuln, stealth. Default: quick."))},
                      {QStringLiteral("output"), strProp(QStringLiteral("Optional file path to save the Nmap XML report."))},
                  })},
                 {QStringLiteral("required"), QJsonArray{QStringLiteral("target")}}}),
            false, false,
        },
        {
            QStringLiteral("web_enum"),
            QStringLiteral("Enumerate a web target: run Nikto (web server misconfig scanner) "
                           "and Gobuster (directory brute-force) against the given URL. "
                           "Returns findings from both tools. Use on sites you have authorization to test."),
            obj({{QStringLiteral("type"), QStringLiteral("object")},
                 {QStringLiteral("properties"), obj({
                      {QStringLiteral("url"), strProp(QStringLiteral("Target URL, e.g. http://10.10.10.10 or https://target.com."))},
                      {QStringLiteral("wordlist"), strProp(QStringLiteral("Optional wordlist path. Defaults to /usr/share/seclists/Discovery/Web-Content/common.txt."))},
                  })},
                 {QStringLiteral("required"), QJsonArray{QStringLiteral("url")}}}),
            false, false,
        },
        {
            QStringLiteral("search_exploits"),
            QStringLiteral("Search the local Exploit-DB copy (searchsploit) for exploits matching a query. "
                           "Returns a list of matching exploit titles, IDs and paths. Read-only, no network needed."),
            obj({{QStringLiteral("type"), QStringLiteral("object")},
                 {QStringLiteral("properties"), obj({
                      {QStringLiteral("query"), strProp(QStringLiteral("Search term, e.g. 'apache 2.4', 'CVE-2021-44228', 'windows smb'."))},
                  })},
                 {QStringLiteral("required"), QJsonArray{QStringLiteral("query")}}}),
            false, false,
        },
        {
            QStringLiteral("run_sqlmap"),
            QStringLiteral("Run sqlmap to automatically detect and exploit SQL injection vulnerabilities "
                           "in a URL. Set 'forms' to test all form parameters. Use only on authorized targets."),
            obj({{QStringLiteral("type"), QStringLiteral("object")},
                 {QStringLiteral("properties"), obj({
                      {QStringLiteral("url"), strProp(QStringLiteral("Target URL with parameter(s), e.g. http://target.com/page?id=1"))},
                      {QStringLiteral("forms"), obj({{QStringLiteral("type"), QStringLiteral("boolean")},
                                                      {QStringLiteral("description"), QStringLiteral("Crawl and test all HTML form parameters.")}})},
                      {QStringLiteral("level"), strProp(QStringLiteral("Test depth 1–5 (default 1). Higher = more vectors, slower."))},
                  })},
                 {QStringLiteral("required"), QJsonArray{QStringLiteral("url")}}}),
            true, true,
        },
        {
            QStringLiteral("crack_hash"),
            QStringLiteral("Identify a password hash type with hashID, then attempt to crack it with "
                           "hashcat (GPU) or john (CPU fallback) using a wordlist. Local operation only."),
            obj({{QStringLiteral("type"), QStringLiteral("object")},
                 {QStringLiteral("properties"), obj({
                      {QStringLiteral("hash"), strProp(QStringLiteral("The hash string to crack."))},
                      {QStringLiteral("wordlist"), strProp(QStringLiteral("Wordlist path. Default: /usr/share/wordlists/rockyou.txt"))},
                      {QStringLiteral("hash_type"), strProp(QStringLiteral("Optional hashcat hash type number (e.g. 0 for MD5). Auto-detected if omitted."))},
                  })},
                 {QStringLiteral("required"), QJsonArray{QStringLiteral("hash")}}}),
            false, false,
        },
        {
            QStringLiteral("brute_service"),
            QStringLiteral("Run Hydra to brute-force login credentials on a network service. "
                           "Supports ssh, ftp, http-get, http-post-form, smb, rdp, mysql, and more. "
                           "Use only on systems you are authorized to test."),
            obj({{QStringLiteral("type"), QStringLiteral("object")},
                 {QStringLiteral("properties"), obj({
                      {QStringLiteral("target"), strProp(QStringLiteral("IP address or hostname of the target."))},
                      {QStringLiteral("service"), strProp(QStringLiteral("Service to attack: ssh, ftp, smb, http-get, rdp, mysql, etc."))},
                      {QStringLiteral("userlist"), strProp(QStringLiteral("User wordlist path, or a single username prefixed with : e.g. ':admin'"))},
                      {QStringLiteral("passlist"), strProp(QStringLiteral("Password wordlist path. Default: /usr/share/wordlists/rockyou.txt"))},
                      {QStringLiteral("port"), strProp(QStringLiteral("Optional non-default port number."))},
                  })},
                 {QStringLiteral("required"), QJsonArray{QStringLiteral("target"), QStringLiteral("service")}}}),
            true, true,
        },
        {
            QStringLiteral("start_capture"),
            QStringLiteral("Start a tcpdump packet capture on a network interface. "
                           "Runs for 'duration' seconds (default 30) then stops and saves to 'output'. "
                           "An optional BPF 'filter' restricts what is captured."),
            obj({{QStringLiteral("type"), QStringLiteral("object")},
                 {QStringLiteral("properties"), obj({
                      {QStringLiteral("interface"), strProp(QStringLiteral("Network interface (e.g. eth0, wlan0). Default: any."))},
                      {QStringLiteral("filter"), strProp(QStringLiteral("BPF capture filter, e.g. 'tcp port 80'. Default: none."))},
                      {QStringLiteral("duration"), strProp(QStringLiteral("How many seconds to capture. Default: 30."))},
                      {QStringLiteral("output"), strProp(QStringLiteral("Output .pcap file path. Default: ~/capture.pcap"))},
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
    if (call.name == QLatin1String("pentest_scan"))
        return tr("Run an Nmap scan");
    if (call.name == QLatin1String("web_enum"))
        return tr("Enumerate a web target (Nikto + Gobuster)");
    if (call.name == QLatin1String("search_exploits"))
        return tr("Search Exploit-DB");
    if (call.name == QLatin1String("run_sqlmap"))
        return tr("Test for SQL injection (sqlmap)");
    if (call.name == QLatin1String("crack_hash"))
        return tr("Crack a password hash");
    if (call.name == QLatin1String("brute_service"))
        return tr("Brute-force a network service (Hydra)");
    if (call.name == QLatin1String("start_capture"))
        return tr("Capture network traffic (tcpdump)");
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
    if (call.name == QLatin1String("pentest_scan")) {
        const QString target = call.args.value(QStringLiteral("target")).toString();
        const QString profile = call.args.value(QStringLiteral("profile")).toString();
        return QStringLiteral("nmap [%1]  %2").arg(profile.isEmpty() ? QStringLiteral("quick") : profile, target);
    }
    if (call.name == QLatin1String("web_enum"))
        return call.args.value(QStringLiteral("url")).toString();
    if (call.name == QLatin1String("search_exploits"))
        return call.args.value(QStringLiteral("query")).toString();
    if (call.name == QLatin1String("run_sqlmap"))
        return call.args.value(QStringLiteral("url")).toString();
    if (call.name == QLatin1String("crack_hash")) {
        QString h = call.args.value(QStringLiteral("hash")).toString();
        if (h.size() > 40) h = h.left(40) + QStringLiteral("…");
        return h;
    }
    if (call.name == QLatin1String("brute_service")) {
        return QStringLiteral("%1://%2").arg(
            call.args.value(QStringLiteral("service")).toString(),
            call.args.value(QStringLiteral("target")).toString());
    }
    if (call.name == QLatin1String("start_capture")) {
        const QString iface = call.args.value(QStringLiteral("interface")).toString();
        const QString dur   = call.args.value(QStringLiteral("duration")).toString();
        return QStringLiteral("tcpdump  iface=%1  %2s").arg(
            iface.isEmpty() ? QStringLiteral("any") : iface,
            dur.isEmpty() ? QStringLiteral("30") : dur);
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

    // ---- Pentest tools -------------------------------------------------------

    if (call.name == QLatin1String("pentest_scan")) {
        const QString target = call.args.value(QStringLiteral("target")).toString().trimmed();
        if (target.isEmpty()) { done({false, tr("No target given.")}); return; }
        const QString profile = call.args.value(QStringLiteral("profile")).toString().trimmed().toLower();
        const QString output  = expandUser(call.args.value(QStringLiteral("output")).toString().trimmed());

        QStringList nmapArgs;
        if (profile == QLatin1String("full"))
            nmapArgs << QStringLiteral("-p-") << QStringLiteral("-T4");
        else if (profile == QLatin1String("service"))
            nmapArgs << QStringLiteral("-sV") << QStringLiteral("-sC") << QStringLiteral("-O") << QStringLiteral("-T4");
        else if (profile == QLatin1String("vuln"))
            nmapArgs << QStringLiteral("-sV") << QStringLiteral("--script=vuln") << QStringLiteral("-T4");
        else if (profile == QLatin1String("stealth"))
            nmapArgs << QStringLiteral("-sS") << QStringLiteral("-T2") << QStringLiteral("--top-ports") << QStringLiteral("1000");
        else // quick (default)
            nmapArgs << QStringLiteral("--top-ports") << QStringLiteral("100") << QStringLiteral("-T4");

        if (!output.isEmpty())
            nmapArgs << QStringLiteral("-oX") << output;
        nmapArgs << QStringLiteral("--") << target;

        Command::run(this, QStringLiteral("nmap"), nmapArgs, [done](const CommandResult &r) {
            QString body = r.outText();
            if (body.size() > 24000) body = body.left(24000) + QStringLiteral("\n…(truncated)");
            done({r.exitCode == 0, QObject::tr("Exit %1.\n").arg(r.exitCode) + body});
        }, 300000); // 5 min max
        return;
    }

    if (call.name == QLatin1String("web_enum")) {
        const QString url = call.args.value(QStringLiteral("url")).toString().trimmed();
        if (url.isEmpty()) { done({false, tr("No URL given.")}); return; }
        const QString wl = call.args.value(QStringLiteral("wordlist")).toString().trimmed();
        const QString wordlist = wl.isEmpty()
            ? QStringLiteral("/usr/share/seclists/Discovery/Web-Content/common.txt") : wl;

        // Run nikto first (best effort), then gobuster.
        QString result;
        const bool hasNikto = Command::available(QStringLiteral("nikto"));
        const bool hasGobuster = Command::available(QStringLiteral("gobuster"));
        if (!hasNikto && !hasGobuster) {
            done({false, tr("Neither nikto nor gobuster is available. Install shadow-arsenal-pentest.")});
            return;
        }

        // We chain the two runs using a shared lambda. gobuster runs after nikto.
        auto runGobuster = [this, done, url, wordlist, &result](const QString &niktoOut) mutable {
            result += QStringLiteral("=== Nikto ===\n") + niktoOut + QStringLiteral("\n\n");
            if (!Command::available(QStringLiteral("gobuster"))) {
                done({true, result});
                return;
            }
            QStringList gbArgs = {QStringLiteral("dir"), QStringLiteral("-u"), url,
                                  QStringLiteral("-w"), wordlist,
                                  QStringLiteral("-q"), QStringLiteral("--no-error"),
                                  QStringLiteral("-t"), QStringLiteral("20")};
            Command::run(this, QStringLiteral("gobuster"), gbArgs, [done, result](const CommandResult &r2) mutable {
                result += QStringLiteral("=== Gobuster ===\n") + r2.outText();
                if (result.size() > 24000) result = result.left(24000) + QStringLiteral("\n…(truncated)");
                done({true, result});
            }, 180000);
        };

        if (hasNikto) {
            Command::run(this, QStringLiteral("nikto"),
                         {QStringLiteral("-h"), url, QStringLiteral("-nointeractive")},
                         [runGobuster](const CommandResult &r) mutable {
                runGobuster(r.outText());
            }, 120000);
        } else {
            runGobuster(QStringLiteral("(nikto not available)"));
        }
        return;
    }

    if (call.name == QLatin1String("search_exploits")) {
        const QString query = call.args.value(QStringLiteral("query")).toString().trimmed();
        if (query.isEmpty()) { done({false, tr("No search query given.")}); return; }
        if (!Command::available(QStringLiteral("searchsploit"))) {
            done({false, tr("searchsploit is not installed. Install shadow-arsenal-pentest (exploitdb package).")});
            return;
        }
        Command::run(this, QStringLiteral("searchsploit"),
                     {QStringLiteral("--colour"), QStringLiteral("--"), query},
                     [done](const CommandResult &r) {
            QString body = r.outText();
            if (body.size() > 16000) body = body.left(16000) + QStringLiteral("\n…(truncated)");
            if (body.trimmed().isEmpty())
                done({true, QObject::tr("No exploits found for \"%1\".").arg(r.errText())});
            else
                done({r.exitCode == 0, body});
        }, 30000);
        return;
    }

    if (call.name == QLatin1String("run_sqlmap")) {
        const QString url = call.args.value(QStringLiteral("url")).toString().trimmed();
        if (url.isEmpty()) { done({false, tr("No URL given.")}); return; }
        if (!Command::available(QStringLiteral("sqlmap"))) {
            done({false, tr("sqlmap is not installed. Install shadow-arsenal-pentest.")});
            return;
        }
        const bool forms = call.args.value(QStringLiteral("forms")).toBool();
        const QString levelStr = call.args.value(QStringLiteral("level")).toString().trimmed();
        const int level = levelStr.isEmpty() ? 1 : qBound(1, levelStr.toInt(), 5);

        QStringList smArgs = {QStringLiteral("-u"), url,
                              QStringLiteral("--level"), QString::number(level),
                              QStringLiteral("--batch"),
                              QStringLiteral("--output-dir"), QStringLiteral("/tmp/sqlmap-shadow")};
        if (forms) smArgs << QStringLiteral("--forms");

        Command::run(this, QStringLiteral("sqlmap"), smArgs, [done](const CommandResult &r) {
            QString body = r.outText();
            if (body.size() > 24000) body = body.left(24000) + QStringLiteral("\n…(truncated)");
            done({r.exitCode == 0, QObject::tr("Exit %1.\n").arg(r.exitCode) + body});
        }, 300000);
        return;
    }

    if (call.name == QLatin1String("crack_hash")) {
        const QString hash = call.args.value(QStringLiteral("hash")).toString().trimmed();
        if (hash.isEmpty()) { done({false, tr("No hash given.")}); return; }
        const QString wlRaw  = call.args.value(QStringLiteral("wordlist")).toString().trimmed();
        const QString wordlist = wlRaw.isEmpty()
            ? QStringLiteral("/usr/share/wordlists/rockyou.txt") : wlRaw;
        const QString htRaw = call.args.value(QStringLiteral("hash_type")).toString().trimmed();

        // 1. Identify hash type with hashid.
        // 2. Try hashcat; fall back to john if hashcat fails.
        auto attemptCrack = [this, done, hash, wordlist, htRaw](const QString &identLine) {
            QString result = QStringLiteral("Hash: %1\n").arg(hash);
            if (!identLine.isEmpty())
                result += QStringLiteral("Identified as: %1\n\n").arg(identLine.trimmed());

            // Write hash to a temp file.
            const QString hfile = QStringLiteral("/tmp/shadow-crack-hash.txt");
            QFile f(hfile);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                done({false, QObject::tr("Could not write temp file.")});
                return;
            }
            f.write((hash + QStringLiteral("\n")).toUtf8());
            f.close();

            if (Command::available(QStringLiteral("hashcat"))) {
                QStringList hcArgs = {QStringLiteral("-a"), QStringLiteral("0"),
                                      QStringLiteral("--force"), QStringLiteral("--quiet")};
                if (!htRaw.isEmpty()) hcArgs << QStringLiteral("-m") << htRaw;
                hcArgs << hfile << wordlist;

                Command::run(this, QStringLiteral("hashcat"), hcArgs,
                             [done, result, hfile, wordlist](const CommandResult &r) mutable {
                    result += QStringLiteral("=== hashcat ===\n") + r.outText() + r.errText();
                    if (result.size() > 16000) result = result.left(16000) + QStringLiteral("\n…(truncated)");
                    done({r.exitCode == 0, result});
                }, 300000);
            } else if (Command::available(QStringLiteral("john"))) {
                Command::run(this, QStringLiteral("john"),
                             {QStringLiteral("--wordlist=") + wordlist, hfile},
                             [done, result, hfile](const CommandResult &r) mutable {
                    // john's results are shown with --show
                    result += QStringLiteral("=== john ===\n") + r.outText() + r.errText();
                    if (result.size() > 16000) result = result.left(16000) + QStringLiteral("\n…(truncated)");
                    done({r.exitCode == 0, result});
                }, 300000);
            } else {
                done({false, QObject::tr("Neither hashcat nor john is installed.")});
            }
        };

        if (Command::available(QStringLiteral("hashid"))) {
            Command::run(this, QStringLiteral("hashid"), {hash},
                         [attemptCrack](const CommandResult &r) {
                attemptCrack(r.outText().split(QLatin1Char('\n')).value(1));
            }, 10000);
        } else {
            attemptCrack(QString());
        }
        return;
    }

    if (call.name == QLatin1String("brute_service")) {
        const QString target  = call.args.value(QStringLiteral("target")).toString().trimmed();
        const QString service = call.args.value(QStringLiteral("service")).toString().trimmed();
        if (target.isEmpty() || service.isEmpty()) {
            done({false, tr("Both target and service are required.")});
            return;
        }
        if (!Command::available(QStringLiteral("hydra"))) {
            done({false, tr("hydra is not installed. Install shadow-arsenal-pentest.")});
            return;
        }
        const QString userlistRaw = call.args.value(QStringLiteral("userlist")).toString().trimmed();
        const QString passlistRaw = call.args.value(QStringLiteral("passlist")).toString().trimmed();
        const QString port        = call.args.value(QStringLiteral("port")).toString().trimmed();

        const QString passlist = passlistRaw.isEmpty()
            ? QStringLiteral("/usr/share/wordlists/rockyou.txt") : passlistRaw;

        QStringList hyArgs = {QStringLiteral("-t"), QStringLiteral("4")};
        if (!port.isEmpty()) hyArgs << QStringLiteral("-s") << port;

        if (userlistRaw.startsWith(QLatin1Char(':')))
            hyArgs << QStringLiteral("-l") << userlistRaw.mid(1);
        else if (!userlistRaw.isEmpty())
            hyArgs << QStringLiteral("-L") << userlistRaw;
        else
            hyArgs << QStringLiteral("-L") << QStringLiteral("/usr/share/seclists/Usernames/top-usernames-shortlist.txt");

        hyArgs << QStringLiteral("-P") << passlist
               << target << service;

        Command::run(this, QStringLiteral("hydra"), hyArgs, [done](const CommandResult &r) {
            QString body = r.outText();
            if (body.size() > 16000) body = body.left(16000) + QStringLiteral("\n…(truncated)");
            done({r.exitCode == 0, QObject::tr("Exit %1.\n").arg(r.exitCode) + body});
        }, 600000); // 10 min max
        return;
    }

    if (call.name == QLatin1String("start_capture")) {
        if (!Command::available(QStringLiteral("tcpdump"))) {
            done({false, tr("tcpdump is not installed.")});
            return;
        }
        const QString iface  = call.args.value(QStringLiteral("interface")).toString().trimmed();
        const QString filter = call.args.value(QStringLiteral("filter")).toString().trimmed();
        const QString durStr = call.args.value(QStringLiteral("duration")).toString().trimmed();
        const QString outRaw = call.args.value(QStringLiteral("output")).toString().trimmed();
        const int duration = durStr.isEmpty() ? 30 : qBound(1, durStr.toInt(), 3600);
        const QString output = outRaw.isEmpty()
            ? expandUser(QStringLiteral("~/capture.pcap")) : expandUser(outRaw);

        QStringList tdArgs = {QStringLiteral("-i"), iface.isEmpty() ? QStringLiteral("any") : iface,
                              QStringLiteral("-G"), QString::number(duration),
                              QStringLiteral("-W"), QStringLiteral("1"),
                              QStringLiteral("-w"), output,
                              QStringLiteral("-nn")};
        if (!filter.isEmpty()) tdArgs << QStringLiteral("--") << filter.split(QLatin1Char(' '));

        Command::run(this, QStringLiteral("tcpdump"), tdArgs, [done, output, duration](const CommandResult &r) {
            if (r.exitCode == 0 || r.exitCode == 1) // tcpdump exits 0 on -G timeout
                done({true, QObject::tr("Captured %1 seconds of traffic to %2.").arg(duration).arg(output)});
            else
                done({false, r.errText().isEmpty() ? QObject::tr("tcpdump failed (exit %1).").arg(r.exitCode) : r.errText()});
        }, (duration + 10) * 1000);
        return;
    }

    done({false, tr("Unknown tool \"%1\".").arg(call.name)});
}
