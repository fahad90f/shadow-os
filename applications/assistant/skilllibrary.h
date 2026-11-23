// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QVariantList>

class AssistantConfig;

// Skills: short instruction sets that teach the assistant how to do one kind
// of task well (reviewing code, finding a bug, running an authorized security
// assessment, …). Each is a Markdown file with a little front matter; the ones
// that ship with the app live in its resources, and the user can add or
// override them under ~/.local/share/shadow/assistant/skills.
//
// For each request the library picks the skills whose triggers best match what
// the user asked, and their text is added to the models' instructions for that
// turn (the whole team sees it). A skill tagged for "pentest" is only offered
// while the assistant is in pentest mode. Skills can be turned off one by one,
// or all at once, in Settings.
class SkillLibrary : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("agent.skills")
    // The master switch: when off, no skill is ever added to a request.
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
    // How many skills are available in the current mode.
    Q_PROPERTY(int count READ count NOTIFY changed)
    // For Settings: [{group, title, items: [{name, title, summary, mode,
    // builtin, enabled, available}]}] with groups "general" and "pentest".
    Q_PROPERTY(QVariantList groups READ groups NOTIFY changed)

public:
    struct Skill
    {
        QString name;       // "code-review"
        QString title;      // "Review a change"
        QString summary;    // one line, for Settings
        QString mode;       // "general" | "pentest" | "any"
        QStringList kinds;  // "coding" | "agents" | "chat" the skill leans to (a bias, not a gate)
        QStringList triggers;  // lowercased phrases that call for it
        QString body;       // the Markdown instructions
        QString source;     // "built-in", or the file it was read from
        bool builtIn = false;
    };

    explicit SkillLibrary(AssistantConfig *config, QObject *parent = nullptr);

    bool enabled() const;
    void setEnabled(bool on);
    int count() const;
    QVariantList groups() const;

    // Re-read the built-in and user skill files.
    Q_INVOKABLE void reload();
    Q_INVOKABLE bool skillEnabled(const QString &name) const;
    Q_INVOKABLE void setSkillEnabled(const QString &name, bool on);

    // The skills chosen for one request: the best matches among those offered
    // in this mode and enabled, most relevant first, capped in number and total
    // size. Empty when the master switch is off or nothing matches.
    QStringList match(const QString &userText, const QString &kind) const;
    // The chosen skills rendered as one instruction block for the models, or
    // "" when 'names' is empty.
    QString render(const QStringList &names) const;

    // For the command line / tests: every skill, with why it would or would not
    // be chosen for 'userText' (empty text: just the catalogue).
    QVariantList describe(const QString &userText, const QString &kind) const;
    // Preview for QML/tests: the names match() would pick.
    Q_INVOKABLE QStringList matchNames(const QString &userText) const;

    const Skill *skill(const QString &name) const;
    QList<Skill> all() const { return m_skills; }

Q_SIGNALS:
    void changed();

private:
    bool availableNow(const Skill &s) const;  // offered in the current mode
    double score(const Skill &s, const QString &text, const QString &kind) const;
    void loadFrom(const QString &dir, bool builtIn);
    static Skill parse(const QString &text, const QString &source, bool builtIn);

    AssistantConfig *m_config;
    QList<Skill> m_skills;
};
