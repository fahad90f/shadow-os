// SPDX-License-Identifier: GPL-3.0-or-later
#include "skilllibrary.h"

#include "assistantconfig.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>

namespace {
// At most this many skills, and this much of their text, go into one request:
// enough to guide the models, not so much that the skills crowd out the
// conversation.
constexpr int kMaxSkills = 3;
constexpr int kMaxChars = 9000;
// A skill is only used when it clears this score (see score()).
constexpr double kThreshold = 1.0;

// Where the built-in skills are compiled in (qt_add_qml_module RESOURCES, under
// the module's URI path). A couple of spellings, so a Qt version change in the
// resource layout does not silently drop every skill.
const QStringList kResourceRoots = {
    QStringLiteral(":/qt/qml/org/shadow/assistant/skills"),
    QStringLiteral(":/qt/qml/org.shadow.assistant/skills"),
    QStringLiteral(":/org/shadow/assistant/skills"),
};

QString firstLineValue(const QString &line)
{
    const int colon = line.indexOf(u':');
    return colon < 0 ? QString() : line.mid(colon + 1).trimmed();
}

QStringList splitList(const QString &value)
{
    QStringList out;
    for (const QString &part : value.split(u',', Qt::SkipEmptyParts)) {
        const QString t = part.trimmed().toLower();
        if (!t.isEmpty())
            out << t;
    }
    return out;
}
} // namespace

SkillLibrary::SkillLibrary(AssistantConfig *config, QObject *parent)
    : QObject(parent), m_config(config)
{
    reload();
    connect(m_config, &AssistantConfig::changed, this, [this]() {
        // The mode (general/pentest), the master switch or the per-skill set
        // may have changed: what is offered follows the configuration.
        Q_EMIT changed();
    });
}

bool SkillLibrary::enabled() const
{
    return m_config->skillsEnabled();
}

void SkillLibrary::setEnabled(bool on)
{
    m_config->setSkillsEnabled(on);
}

int SkillLibrary::count() const
{
    int n = 0;
    for (const Skill &s : m_skills)
        if (availableNow(s))
            ++n;
    return n;
}

bool SkillLibrary::skillEnabled(const QString &name) const
{
    return m_config->skillEnabled(name);
}

void SkillLibrary::setSkillEnabled(const QString &name, bool on)
{
    m_config->setSkillEnabled(name, on);
}

const SkillLibrary::Skill *SkillLibrary::skill(const QString &name) const
{
    for (const Skill &s : m_skills)
        if (s.name == name)
            return &s;
    return nullptr;
}

// ---- loading -----------------------------------------------------------------

void SkillLibrary::reload()
{
    m_skills.clear();
    QString builtInRoot;
    for (const QString &root : kResourceRoots) {
        if (QDir(root).exists()) {
            builtInRoot = root;
            break;
        }
    }
    if (!builtInRoot.isEmpty())
        loadFrom(builtInRoot, true);
    // The user's own skills, which override a built-in of the same name.
    const QString userDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/shadow/assistant/skills");
    loadFrom(userDir, false);

    std::sort(m_skills.begin(), m_skills.end(),
              [](const Skill &a, const Skill &b) { return a.name < b.name; });
    Q_EMIT changed();
}

void SkillLibrary::loadFrom(const QString &dir, bool builtIn)
{
    QDirIterator it(dir, {QStringLiteral("*.md")}, QDir::Files);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const Skill s = parse(QString::fromUtf8(f.readAll()), builtIn ? QStringLiteral("built-in") : path, builtIn);
        if (s.name.isEmpty() || s.body.trimmed().isEmpty())
            continue;
        // A user skill replaces a built-in of the same name.
        for (int i = 0; i < m_skills.size(); ++i) {
            if (m_skills.at(i).name == s.name) {
                m_skills.removeAt(i);
                break;
            }
        }
        m_skills.append(s);
    }
}

SkillLibrary::Skill SkillLibrary::parse(const QString &text, const QString &source, bool builtIn)
{
    Skill s;
    s.source = source;
    s.builtIn = builtIn;
    s.mode = QStringLiteral("general");

    QString body = text;
    // Front matter: the block between the first two "---" lines.
    const QRegularExpression fm(QStringLiteral("\\A\\s*---\\s*\\n(.*?)\\n---\\s*\\n"),
                                QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch m = fm.match(text);
    if (m.hasMatch()) {
        body = text.mid(m.capturedEnd());
        for (const QString &raw : m.captured(1).split(u'\n')) {
            const QString line = raw.trimmed();
            if (line.isEmpty() || line.startsWith(u'#'))
                continue;
            const QString key = line.section(u':', 0, 0).trimmed().toLower();
            const QString value = firstLineValue(line);
            if (key == QLatin1String("name"))
                s.name = value.toLower();
            else if (key == QLatin1String("title"))
                s.title = value;
            else if (key == QLatin1String("summary") || key == QLatin1String("description"))
                s.summary = value;
            else if (key == QLatin1String("mode"))
                s.mode = value.toLower();
            else if (key == QLatin1String("kinds"))
                s.kinds = splitList(value);
            else if (key == QLatin1String("triggers"))
                s.triggers = splitList(value);
        }
    }
    if (s.name.isEmpty()) {
        // Fall back to the file name and the first heading.
        s.name = QFileInfo(source).completeBaseName().toLower();
    }
    if (s.title.isEmpty()) {
        const QRegularExpressionMatch h = QRegularExpression(QStringLiteral("^#\\s+(.+)$"),
                                                             QRegularExpression::MultilineOption).match(body);
        s.title = h.hasMatch() ? h.captured(1).trimmed() : s.name;
    }
    s.body = body.trimmed();
    if (s.mode != QLatin1String("pentest") && s.mode != QLatin1String("any"))
        s.mode = QStringLiteral("general");
    return s;
}

// ---- which skills are offered now --------------------------------------------

bool SkillLibrary::availableNow(const Skill &s) const
{
    if (s.mode == QLatin1String("pentest") && m_config->role() != QLatin1String("pentest"))
        return false;
    return true;
}

// ---- matching ----------------------------------------------------------------

double SkillLibrary::score(const Skill &s, const QString &text, const QString &kind) const
{
    double total = 0;
    for (const QString &trigger : s.triggers) {
        if (trigger.isEmpty())
            continue;
        // A whole-word (or whole-phrase) match of a trigger. Longer, more
        // specific triggers ("sql injection") count for more than short ones
        // ("test") so a precise phrase wins over an incidental word.
        const QString pat = QStringLiteral("(?<![\\w-])") + QRegularExpression::escape(trigger)
            + QStringLiteral("(?![\\w-])");
        if (QRegularExpression(pat).match(text).hasMatch()) {
            const int words = trigger.count(u' ') + 1;
            total += 1.0 + 0.5 * (words - 1) + qMin(trigger.size(), 16) / 32.0;
        }
    }
    if (total <= 0)
        return 0;
    // A gentle nudge towards the skills meant for this kind of request, and
    // towards the pentest skills while assessing, so the right one leads when
    // several match.
    if (!kind.isEmpty() && s.kinds.contains(kind))
        total += 0.4;
    if (s.mode == QLatin1String("pentest") && kind == QLatin1String("agents"))
        total += 0.3;
    return total;
}

QStringList SkillLibrary::match(const QString &userText, const QString &kind) const
{
    if (!m_config->skillsEnabled())
        return {};
    const QString text = userText.toLower();
    QList<QPair<double, const Skill *>> scored;
    for (const Skill &s : m_skills) {
        if (!availableNow(s) || !m_config->skillEnabled(s.name))
            continue;
        const double sc = score(s, text, kind);
        if (sc >= kThreshold)
            scored.append({sc, &s});
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto &a, const auto &b) { return a.first > b.first; });

    QStringList chosen;
    int chars = 0;
    for (const auto &pair : scored) {
        if (chosen.size() >= kMaxSkills)
            break;
        const int add = pair.second->body.size();
        if (!chosen.isEmpty() && chars + add > kMaxChars)
            continue; // keep at least the top one even if it is large
        chosen << pair.second->name;
        chars += add;
    }
    return chosen;
}

QStringList SkillLibrary::matchNames(const QString &userText) const
{
    const QString kind = m_config->role() == QLatin1String("pentest") ? QStringLiteral("agents") : QString();
    return match(userText, kind);
}

QString SkillLibrary::render(const QStringList &names) const
{
    if (names.isEmpty())
        return {};
    QStringList parts;
    for (const QString &name : names) {
        const Skill *s = skill(name);
        if (s)
            parts << s->body.trimmed();
    }
    if (parts.isEmpty())
        return {};
    return QStringLiteral(
               "\n\n--- Skill instructions ---\n"
               "The guidance below applies to the user's latest request. Follow it. It does not "
               "override the user, your safety rules, or the need to confirm risky actions.\n\n")
        + parts.join(QStringLiteral("\n\n---\n\n"))
        + QStringLiteral("\n--- End of skill instructions ---");
}

// ---- for Settings and the command line --------------------------------------

QVariantList SkillLibrary::groups() const
{
    auto item = [this](const Skill &s) {
        return QVariantMap{
            {QStringLiteral("name"), s.name},
            {QStringLiteral("title"), s.title},
            {QStringLiteral("summary"), s.summary},
            {QStringLiteral("mode"), s.mode},
            {QStringLiteral("builtin"), s.builtIn},
            {QStringLiteral("enabled"), m_config->skillEnabled(s.name)},
            {QStringLiteral("available"), availableNow(s)},
        };
    };
    QVariantList general, pentest;
    for (const Skill &s : m_skills) {
        if (s.mode == QLatin1String("pentest"))
            pentest.append(item(s));
        else
            general.append(item(s));
    }
    QVariantList out;
    out.append(QVariantMap{{QStringLiteral("group"), QStringLiteral("general")},
                           {QStringLiteral("title"), tr("Coding, debugging and everyday work")},
                           {QStringLiteral("items"), general}});
    if (!pentest.isEmpty())
        out.append(QVariantMap{{QStringLiteral("group"), QStringLiteral("pentest")},
                               {QStringLiteral("title"), tr("Security testing (only in Pentest mode)")},
                               {QStringLiteral("items"), pentest}});
    return out;
}

QVariantList SkillLibrary::describe(const QString &userText, const QString &kind) const
{
    const QString text = userText.toLower();
    const QStringList chosen = userText.isEmpty() ? QStringList() : match(userText, kind);
    QVariantList out;
    for (const Skill &s : m_skills) {
        out.append(QVariantMap{
            {QStringLiteral("name"), s.name},
            {QStringLiteral("title"), s.title},
            {QStringLiteral("mode"), s.mode},
            {QStringLiteral("builtin"), s.builtIn},
            {QStringLiteral("enabled"), m_config->skillEnabled(s.name)},
            {QStringLiteral("available"), availableNow(s)},
            {QStringLiteral("summary"), s.summary},
            {QStringLiteral("triggers"), int(s.triggers.size())},
            {QStringLiteral("bodyChars"), int(s.body.size())},
            {QStringLiteral("score"), userText.isEmpty() ? 0.0 : score(s, text, kind)},
            {QStringLiteral("chosen"), chosen.contains(s.name)},
        });
    }
    return out;
}
