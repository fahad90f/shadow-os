// SPDX-License-Identifier: GPL-3.0-or-later
#include "shells.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSet>
#include <QStandardPaths>

#include <pwd.h>
#include <unistd.h>
#include <vector>

namespace Shells {

namespace {

const char kShellsFile[] = "/etc/shells";

// passwd entry of the calling user; fields are empty if there is none.
struct PasswdEntry
{
    QString shell;
    QString home;
};

PasswdEntry passwdEntry()
{
    PasswdEntry entry;
    long size = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    if (size <= 0)
        size = 16384;
    std::vector<char> buffer(static_cast<size_t>(size));
    struct passwd pwd;
    struct passwd *result = nullptr;
    if (::getpwuid_r(::getuid(), &pwd, buffer.data(), buffer.size(), &result) == 0 && result) {
        entry.shell = QFile::decodeName(result->pw_shell);
        entry.home = QFile::decodeName(result->pw_dir);
    }
    return entry;
}

} // namespace

bool isExecutable(const QString &path)
{
    const QFileInfo info(path);
    return info.isAbsolute() && info.isFile() && info.isExecutable();
}

QList<Shell> listed()
{
    QList<Shell> shells;
    QFile file(QString::fromLatin1(kShellsFile));
    if (!file.open(QIODevice::ReadOnly))
        return shells;
    while (!file.atEnd()) {
        QString line = QFile::decodeName(file.readLine());
        const qsizetype hash = line.indexOf(QLatin1Char('#'));
        if (hash >= 0)
            line.truncate(hash);
        line = line.trimmed();
        if (!line.startsWith(QLatin1Char('/')))
            continue;
        const QFileInfo info(line);
        Shell shell;
        shell.path = line;
        shell.name = info.fileName();
        shell.canonical = info.canonicalFilePath();
        shell.executable = isExecutable(line);
        shells.append(shell);
    }
    return shells;
}

QList<Shell> available()
{
    QList<Shell> shells;
    QSet<QString> seen;
    const QList<Shell> all = listed();
    for (const Shell &shell : all) {
        if (!shell.executable)
            continue;
        // Same program under the same name → one entry. A different name for
        // the same file (sh → dash, rbash → bash) changes how the shell
        // behaves and stays separate.
        const QString key = shell.name + QLatin1Char('\n') + shell.canonical;
        if (seen.contains(key))
            continue;
        seen.insert(key);
        shells.append(shell);
    }
    return shells;
}

QString loginShell(QString *source)
{
    const QString fromPasswd = passwdEntry().shell;
    if (isExecutable(fromPasswd)) {
        if (source)
            *source = QStringLiteral("passwd");
        return fromPasswd;
    }
    const QString fromEnvironment = QFile::decodeName(qgetenv("SHELL"));
    if (isExecutable(fromEnvironment)) {
        if (source)
            *source = QStringLiteral("environment");
        return fromEnvironment;
    }
    if (source)
        *source = QStringLiteral("fallback");
    return QStringLiteral("/bin/sh");
}

QString homeDirectory()
{
    // $HOME is what the shells call "~"; the passwd entry only stands in
    // when the variable is missing or wrong.
    const QString fromEnvironment = QFile::decodeName(qgetenv("HOME"));
    if (!fromEnvironment.isEmpty() && QFileInfo(fromEnvironment).isDir())
        return QDir::cleanPath(fromEnvironment);
    const QString fromPasswd = passwdEntry().home;
    if (!fromPasswd.isEmpty() && QFileInfo(fromPasswd).isDir())
        return fromPasswd;
    return QDir::rootPath();
}

QString displayName(const QString &path)
{
    const QString name = QFileInfo(path).fileName();
    if (name.isEmpty())
        return path;
    // Shell names are conventionally lower case; only the well-known ones
    // have a "proper" spelling.
    static const struct { const char *file; const char *label; } known[] = {
        {"bash", "Bash"}, {"zsh", "Zsh"}, {"fish", "Fish"}, {"dash", "Dash"}, {"ksh", "KornShell"},
        {"tcsh", "tcsh"}, {"csh", "C shell"}, {"nu", "Nushell"}, {"pwsh", "PowerShell"},
        {"rbash", "Bash (restricted)"}, {"sh", "sh"},
    };
    for (const auto &k : known) {
        if (name == QLatin1String(k.file))
            return QString::fromLatin1(k.label);
    }
    return name;
}

QStringList splitCommand(const QString &commandLine, bool *ok)
{
    // The quoting rules of a POSIX shell, and nothing else: no variables, no
    // globbing, no operators — the words go to exec() as they are.
    QStringList words;
    QString word;
    bool inWord = false;
    QChar quote;                    // null, '\'' or '"'
    if (ok)
        *ok = true;

    for (qsizetype i = 0; i < commandLine.size(); ++i) {
        const QChar ch = commandLine.at(i);
        if (quote == QLatin1Char('\'')) {
            if (ch == quote)
                quote = QChar();
            else
                word += ch;
        } else if (quote == QLatin1Char('"')) {
            if (ch == quote) {
                quote = QChar();
            } else if (ch == QLatin1Char('\\') && i + 1 < commandLine.size()
                       && (commandLine.at(i + 1) == QLatin1Char('"') || commandLine.at(i + 1) == QLatin1Char('\\'))) {
                word += commandLine.at(++i);
            } else {
                word += ch;
            }
        } else if (ch == QLatin1Char('\'') || ch == QLatin1Char('"')) {
            quote = ch;
            inWord = true;          // "" is a word, even if empty
        } else if (ch == QLatin1Char('\\')) {
            if (i + 1 < commandLine.size())
                word += commandLine.at(++i);
            inWord = true;
        } else if (ch.isSpace()) {
            if (inWord || !word.isEmpty())
                words << word;
            word.clear();
            inWord = false;
        } else {
            word += ch;
            inWord = true;
        }
    }
    if (!quote.isNull()) {
        if (ok)
            *ok = false;            // a quote was never closed
        return {};
    }
    if (inWord || !word.isEmpty())
        words << word;
    return words;
}

QString resolveProgram(const QString &program)
{
    if (program.isEmpty())
        return QString();
    if (program.contains(QLatin1Char('/'))) {
        const QFileInfo info(program);
        if (info.isFile() && info.isExecutable())
            return info.absoluteFilePath();
        return QString();
    }
    return QStandardPaths::findExecutable(program);
}

QJsonObject toJson()
{
    QJsonArray entries;
    const QList<Shell> all = listed();
    for (const Shell &shell : all) {
        entries.append(QJsonObject{
            {QStringLiteral("path"), shell.path},
            {QStringLiteral("name"), shell.name},
            {QStringLiteral("displayName"), displayName(shell.path)},
            {QStringLiteral("canonical"), shell.canonical},
            {QStringLiteral("executable"), shell.executable},
        });
    }
    QJsonArray offered;
    const QList<Shell> usable = available();
    for (const Shell &shell : usable)
        offered.append(shell.path);

    QString source;
    const QString login = loginShell(&source);
    return QJsonObject{
        {QStringLiteral("source"), QString::fromLatin1(kShellsFile)},
        {QStringLiteral("readable"), QFileInfo(QString::fromLatin1(kShellsFile)).isReadable()},
        {QStringLiteral("default"), login},
        {QStringLiteral("defaultSource"), source},
        {QStringLiteral("defaultName"), displayName(login)},
        {QStringLiteral("home"), homeDirectory()},
        {QStringLiteral("shells"), entries},
        {QStringLiteral("offered"), offered},
    };
}

} // namespace Shells
