// SPDX-License-Identifier: GPL-3.0-or-later
#include "assistant.h"
#include "assistantconfig.h"
#include "toolregistry.h"
#include "voicecontroller.h"

#include <shadowapp.h>

#include <QCommandLineOption>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

int main(int argc, char **argv)
{
    Shadow::App app(argc, argv, {
        QStringLiteral("org.shadow.assistant"),
        QStringLiteral("Shadow Assistant"),
        QStringLiteral("org.shadow.assistant"),
        QStringLiteral("A personal AI agent that can chat and carry out tasks on this computer"),
        true, // single instance
    });
    app.setDumpTopics({QStringLiteral("config"), QStringLiteral("models"), QStringLiteral("tools"),
                       QStringLiteral("voice")});
    // Headless voice test hooks: run the real engines on a WAV and print the
    // result as JSON (used by the offline wake-word / STT / TTS tests).
    QCommandLineOption transcribeOpt(QStringLiteral("voice-transcribe"),
        QStringLiteral("Transcribe a 16 kHz mono WAV with the offline STT engine and print the result."),
        QStringLiteral("wav"));
    QCommandLineOption wakeOpt(QStringLiteral("voice-detect-wake"),
        QStringLiteral("Report whether the wake phrase occurs in a WAV (keyphrase spotting)."),
        QStringLiteral("wav"));
    QCommandLineOption synthOpt(QStringLiteral("voice-synthesize"),
        QStringLiteral("Synthesize text (given) to the WAV path in --voice-out and print status."),
        QStringLiteral("text"));
    QCommandLineOption synthOutOpt(QStringLiteral("voice-out"),
        QStringLiteral("Output WAV path for --voice-synthesize."), QStringLiteral("wav"));
    app.parser().addOptions({transcribeOpt, wakeOpt, synthOpt, synthOutOpt});
    if (!app.processArguments())
        return 0;

    if (app.parser().isSet(transcribeOpt) || app.parser().isSet(wakeOpt) || app.parser().isSet(synthOpt)) {
        AssistantConfig config;
        VoiceController voice;
        voice.setConfig(&config);
        QTextStream out(stdout);
        QJsonObject result;
        if (app.parser().isSet(transcribeOpt)) {
            QString err;
            const QString text = voice.transcribeWavSync(app.parser().value(transcribeOpt), &err);
            result = QJsonObject{{QStringLiteral("transcript"), text},
                                 {QStringLiteral("ok"), !text.isEmpty()},
                                 {QStringLiteral("error"), err}};
        } else if (app.parser().isSet(wakeOpt)) {
            QString err;
            const bool hit = voice.wakePhraseInWavSync(app.parser().value(wakeOpt),
                config.wakePhrase(), config.wakeThreshold(), &err);
            result = QJsonObject{{QStringLiteral("wakePhrase"), config.wakePhrase()},
                                 {QStringLiteral("detected"), hit},
                                 {QStringLiteral("error"), err}};
        } else {
            QString err;
            const QString outWav = app.parser().value(synthOutOpt);
            const bool ok = voice.synthesizeToWavSync(app.parser().value(synthOpt), outWav, &err);
            result = QJsonObject{{QStringLiteral("engine"), voice.resolvedTtsEngine()},
                                 {QStringLiteral("output"), outWav},
                                 {QStringLiteral("ok"), ok},
                                 {QStringLiteral("error"), err}};
        }
        out << QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Indented));
        return 0;
    }

    if (!app.dumpTopic().isEmpty()) {
        AssistantConfig config;
        const QString topic = app.dumpTopic();
        if (topic == QLatin1String("config")) {
            // Never includes the key itself, only whether one is configured.
            return app.finishDump(config.toJson());
        }
        if (topic == QLatin1String("models")) {
            return app.finishDump(QJsonObject{
                {QStringLiteral("brain"), config.brainModel()},
                {QStringLiteral("planner"), config.plannerModel()},
                {QStringLiteral("fast"), config.fastModel()},
                {QStringLiteral("omni"), config.omniModel()},
                {QStringLiteral("baseUrl"), config.baseUrl()},
            });
        }
        if (topic == QLatin1String("tools")) {
            ToolRegistry registry(&config);
            QJsonArray tools;
            const QJsonArray api = registry.apiTools();
            for (const QJsonValue &v : api) {
                const QJsonObject fn = v.toObject().value(QStringLiteral("function")).toObject();
                const QString name = fn.value(QStringLiteral("name")).toString();
                tools.append(QJsonObject{
                    {QStringLiteral("name"), name},
                    {QStringLiteral("description"), fn.value(QStringLiteral("description")).toString()},
                    {QStringLiteral("changesSystem"), registry.toolChangesSystem(name)},
                    {QStringLiteral("risky"), registry.toolRisky(name)},
                });
            }
            return app.finishDump(QJsonObject{
                {QStringLiteral("enabled"), tools},
                {QStringLiteral("autonomy"), config.autonomy()},
            });
        }
        if (topic == QLatin1String("voice")) {
            VoiceController voice;
            voice.setConfig(&config);
            return app.finishDump(voice.diagnostics());
        }
        return app.dumpError(QStringLiteral("unknown dump topic '%1'").arg(topic));
    }

    return app.run(QStringLiteral("org.shadow.assistant"));
}
