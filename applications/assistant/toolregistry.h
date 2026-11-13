// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <functional>

#include "undojournal.h"

class AssistantConfig;

// Describes one capability the model may invoke. Execution that changes the
// system or reaches the network is marked and gated behind explicit in-UI
// confirmation by the orchestrator — the registry itself never auto-runs a
// changing action.
struct ToolSpec
{
    QString name;
    QString description;
    QJsonObject parameters;   // JSON-Schema object for the function parameters
    bool changesSystem = false; // writes, launches, or otherwise mutates state
    bool risky = false;         // destructive potential (shell)
};

struct ToolInvocation
{
    QString id;      // tool_call id from the model
    QString name;
    QJsonObject args;
};

struct ToolOutcome
{
    bool ok = false;
    QString text;    // result fed back to the model and shown in the UI
};

// The tool framework. Holds the specs, renders the OpenAI "tools" array for a
// request, and executes a single invocation synchronously-from-the-caller's
// point-of-view via a completion callback (work runs through Shadow::Command).
class ToolRegistry : public QObject
{
    Q_OBJECT
public:
    explicit ToolRegistry(AssistantConfig *config, QObject *parent = nullptr);

    // Tools currently enabled in configuration, as the API "tools" array.
    QJsonArray apiTools() const;
    bool hasEnabledTools() const;

    const ToolSpec *spec(const QString &name) const;
    bool toolChangesSystem(const QString &name) const;
    bool toolRisky(const QString &name) const;

    // Human-readable, one-line summary of what a call will do (for the
    // confirmation card header).
    QString summarize(const ToolInvocation &call) const;
    // The exact command/argument vector or path a call will act on, shown
    // verbatim for confirmation. Returns a display string (may be multi-line).
    QString preview(const ToolInvocation &call) const;
    // For run_shell_command: the resolved argv (program + args), never a shell
    // string. Empty if the call is not a shell command or cannot be parsed.
    QStringList resolvedArgv(const ToolInvocation &call) const;

    using Done = std::function<void(const ToolOutcome &)>;
    // Execute the call. 'done' is invoked on this object's thread. The caller
    // is responsible for having obtained confirmation where required.
    void execute(const ToolInvocation &call, Done done);

    // The log of file-changing actions, with undo.
    UndoJournal *journal() { return &m_journal; }

private:
    AssistantConfig *m_config;
    QList<ToolSpec> m_specs;
    UndoJournal m_journal;
};
