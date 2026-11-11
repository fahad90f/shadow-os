// SPDX-License-Identifier: GPL-3.0-or-later
//
// NEXUS Text Editor.
//
//   nexus-editor [file[:line]...]        open files (in the running window, if any)
//   nexus-editor --probe <file>          print what the editor detects (encoding, BOM,
//                                        line endings, syntax) as JSON; no window
//   nexus-editor --resave <in> <out>     decode <in> and write it to <out> exactly as the
//                                        editor would save it; no window (round-trip check)
//   nexus-editor --dump <topic>          syntaxes | encodings | themes | limits | settings
#include "document.h"
#include "textfile.h"

#include <nexusapp.h>

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/Theme>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>

#include <cstdio>
#include <string>
#include <vector>

namespace {

QJsonObject probe(const QString &path)
{
    QJsonObject out{{QStringLiteral("path"), QFileInfo(path).absoluteFilePath()}};
    const QFileInfo info(path);
    if (!info.isFile()) {
        out[QStringLiteral("ok")] = false;
        out[QStringLiteral("error")] = QStringLiteral("not a regular file");
        return out;
    }
    if (info.size() > EditorLimits::maxFileBytes) {
        out[QStringLiteral("ok")] = false;
        out[QStringLiteral("error")] = QStringLiteral("larger than the %1 byte limit").arg(EditorLimits::maxFileBytes);
        out[QStringLiteral("bytes")] = double(info.size());
        return out;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        out[QStringLiteral("ok")] = false;
        out[QStringLiteral("error")] = file.errorString();
        return out;
    }
    const QByteArray bytes = file.readAll();
    const TextFile::Decoded decoded = TextFile::decode(bytes);
    const QJsonObject described = TextFile::describe(decoded, bytes.size());
    for (auto it = described.begin(); it != described.end(); ++it)
        out[it.key()] = it.value();
    if (decoded.ok()) {
        const auto definition = syntaxRepository().definitionForFileName(info.fileName());
        out[QStringLiteral("syntax")] = definition.isValid() ? definition.name() : QStringLiteral("None");
        out[QStringLiteral("highlighted")] = definition.isValid() && decoded.text.size() <= EditorLimits::maxHighlightChars;
    }
    return out;
}

int resave(const QString &in, const QString &out)
{
    QFile file(in);
    if (!file.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "nexus-editor: %s: %s\n", qPrintable(in), qPrintable(file.errorString()));
        return 1;
    }
    const TextFile::Decoded decoded = TextFile::decode(file.readAll());
    if (!decoded.ok()) {
        std::fprintf(stderr, "nexus-editor: %s\n", qPrintable(decoded.error));
        return 1;
    }
    const TextFile::Encoded encoded = TextFile::encode(decoded.text, decoded.encoding, decoded.bom, decoded.lineEnding);
    if (!encoded.ok()) {
        std::fprintf(stderr, "nexus-editor: %s\n", qPrintable(encoded.error));
        return 1;
    }
    QSaveFile target(out);
    if (!target.open(QIODevice::WriteOnly) || target.write(encoded.bytes) != encoded.bytes.size() || !target.commit()) {
        std::fprintf(stderr, "nexus-editor: %s: %s\n", qPrintable(out), qPrintable(target.errorString()));
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    // A second launch hands its arguments to the running instance, whose
    // working directory may differ: make file arguments absolute first.
    std::vector<std::string> storage;
    std::vector<char *> arguments;
    bool headless = false;
    {
        const QString cwd = QDir::currentPath();
        bool optionsEnded = false, valueNext = false;
        storage.reserve(size_t(argc));
        for (int i = 0; i < argc; ++i) {
            std::string arg(argv[i]);
            if (i > 0) {
                if (valueNext) {
                    valueNext = false;
                } else if (!optionsEnded && arg == "--") {
                    optionsEnded = true;
                } else if (!optionsEnded && arg.size() > 1 && arg[0] == '-') {
                    if (arg == "--dump")
                        valueNext = true;
                    if (arg == "--probe" || arg == "--resave")
                        headless = true;
                } else if (!arg.empty() && arg[0] != '/') {
                    arg = QDir::cleanPath(QDir(cwd).filePath(QString::fromLocal8Bit(arg.c_str()))).toLocal8Bit().toStdString();
                }
            }
            storage.push_back(std::move(arg));
        }
        for (std::string &s : storage)
            arguments.push_back(s.data());
        arguments.push_back(nullptr);
    }
    if (headless)
        qputenv("QT_QPA_PLATFORM", "offscreen");
    int count = argc;

    Nexus::App app(count, arguments.data(), {
        QStringLiteral("org.nexus.editor"),
        QStringLiteral("Text Editor"),
        QStringLiteral("org.nexus.editor"),
        QStringLiteral("Text editor for source code and plain text"),
        !headless,      // single instance: files open as tabs of the running window
    });
    app.setDumpTopics({QStringLiteral("syntaxes"), QStringLiteral("encodings"), QStringLiteral("themes"),
                       QStringLiteral("limits"), QStringLiteral("settings")});
    QCommandLineOption probeOption(QStringLiteral("probe"),
        QStringLiteral("Print the detected encoding, line endings and syntax of the given files as JSON and exit."));
    QCommandLineOption resaveOption(QStringLiteral("resave"),
        QStringLiteral("Decode <in> and write it to <out> as the editor would save it, then exit: nexus-editor --resave <in> <out>."));
    app.parser().addOption(probeOption);
    app.parser().addOption(resaveOption);
    app.parser().addPositionalArgument(QStringLiteral("files"), QStringLiteral("Files to open; file:LINE jumps to a line."),
                                       QStringLiteral("[file[:line]...]"));
    if (!app.processArguments())
        return 0;

    const QStringList files = app.parser().positionalArguments();
    if (app.parser().isSet(probeOption)) {
        QJsonArray out;
        for (const QString &file : files)
            out.append(probe(file));
        return app.finishDump(out);
    }
    if (app.parser().isSet(resaveOption)) {
        if (files.size() != 2) {
            std::fprintf(stderr, "usage: nexus-editor --resave <in> <out>\n");
            return 2;
        }
        return resave(files.at(0), files.at(1));
    }

    const QString topic = app.dumpTopic();
    if (!topic.isEmpty()) {
        if (topic == QLatin1String("syntaxes")) {
            QJsonArray out;
            const auto definitions = syntaxRepository().definitions();
            for (const auto &definition : definitions) {
                out.append(QJsonObject{
                    {QStringLiteral("name"), definition.name()},
                    {QStringLiteral("section"), definition.section()},
                    {QStringLiteral("extensions"), QJsonArray::fromStringList(definition.extensions())},
                    {QStringLiteral("hidden"), definition.isHidden()},
                });
            }
            return app.finishDump(out);
        }
        if (topic == QLatin1String("encodings"))
            return app.finishDump(QJsonArray::fromStringList(TextFile::encodings()));
        if (topic == QLatin1String("themes")) {
            QJsonArray out;
            const auto themes = syntaxRepository().themes();
            for (const auto &theme : themes)
                out.append(theme.name());
            return app.finishDump(QJsonObject{
                {QStringLiteral("available"), out},
                {QStringLiteral("nexusDark"), syntaxRepository().theme(QStringLiteral("NEXUS Dark")).isValid()},
                {QStringLiteral("nexusLight"), syntaxRepository().theme(QStringLiteral("NEXUS Light")).isValid()},
            });
        }
        if (topic == QLatin1String("limits")) {
            return app.finishDump(QJsonObject{
                {QStringLiteral("maxFileBytes"), double(EditorLimits::maxFileBytes)},
                {QStringLiteral("maxHighlightChars"), double(EditorLimits::maxHighlightChars)},
            });
        }
        if (topic == QLatin1String("settings")) {
            QSettings settings;
            QJsonObject out{{QStringLiteral("file"), settings.fileName()}};
            const QStringList keys = settings.allKeys();
            for (const QString &key : keys)
                out[key] = QJsonValue::fromVariant(settings.value(key));
            out[QStringLiteral("jetBrainsMonoInstalled")] = QFontDatabase::families().contains(QStringLiteral("JetBrains Mono"));
            return app.finishDump(out);
        }
        return app.dumpError(QStringLiteral("unknown dump topic '%1'").arg(topic));
    }
    return app.run(QStringLiteral("org.nexus.editor"));
}
