// SPDX-License-Identifier: GPL-3.0-or-later
#include "assistant.h"
#include "assistantconfig.h"
#include "toolregistry.h"

#include <shadowapp.h>

#include <QJsonArray>
#include <QJsonObject>

int main(int argc, char **argv)
{
    Shadow::App app(argc, argv, {
        QStringLiteral("org.shadow.assistant"),
        QStringLiteral("Shadow Assistant"),
        QStringLiteral("org.shadow.assistant"),
        QStringLiteral("A personal AI agent that can chat and carry out tasks on this computer"),
        true, // single instance
    });
    app.setDumpTopics({QStringLiteral("config"), QStringLiteral("models"), QStringLiteral("tools")});
    if (!app.processArguments())
        return 0;

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
        return app.dumpError(QStringLiteral("unknown dump topic '%1'").arg(topic));
    }

    return app.run(QStringLiteral("org.shadow.assistant"));
}
