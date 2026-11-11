// SPDX-License-Identifier: GPL-3.0-or-later
#include "locales.h"

#include <QCollator>
#include <QFile>
#include <QJsonArray>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace {

const QString kSupported = QStringLiteral("/usr/share/i18n/SUPPORTED");

struct Names
{
    QString native;
    QString english;
};

QString capitalised(QString text)
{
    if (!text.isEmpty())
        text[0] = text.at(0).toUpper();
    return text;
}

QString modifierText(const QString &modifier)
{
    if (modifier == QLatin1String("latin"))
        return Locales::tr("Latin script");
    if (modifier == QLatin1String("cyrillic"))
        return Locales::tr("Cyrillic script");
    if (modifier == QLatin1String("devanagari"))
        return Locales::tr("Devanagari script");
    if (modifier == QLatin1String("valencia"))
        return Locales::tr("Valencian");
    return modifier;
}

// "de_DE.UTF-8" → Deutsch (Deutschland) / German (Germany)
Names describe(const QString &code)
{
    const QString modifier = code.section(QLatin1Char('@'), 1, 1);
    const QString base = code.section(QLatin1Char('@'), 0, 0).section(QLatin1Char('.'), 0, 0);
    const QString langCode = base.section(QLatin1Char('_'), 0, 0);
    const QString terrCode = base.section(QLatin1Char('_'), 1, 1);

    const QLocale::Language language = QLocale::codeToLanguage(langCode);
    const QLocale::Territory territory = terrCode.isEmpty() ? QLocale::AnyTerritory : QLocale::codeToTerritory(terrCode);
    QLocale::Script script = QLocale::AnyScript;
    if (modifier == QLatin1String("latin"))
        script = QLocale::LatinScript;
    else if (modifier == QLatin1String("cyrillic"))
        script = QLocale::CyrillicScript;
    else if (modifier == QLatin1String("devanagari"))
        script = QLocale::DevanagariScript;

    const bool languageKnown = language != QLocale::AnyLanguage && language != QLocale::C;
    const bool territoryKnown = territory != QLocale::AnyTerritory;

    QStringList englishDetail;
    if (territoryKnown)
        englishDetail << QLocale::territoryToString(territory);
    else if (!terrCode.isEmpty())
        englishDetail << terrCode;
    if (!modifier.isEmpty())
        englishDetail << modifierText(modifier);

    Names names;
    names.english = languageKnown ? QLocale::languageToString(language) : langCode;
    if (!englishDetail.isEmpty())
        names.english += QStringLiteral(" (%1)").arg(englishDetail.join(QStringLiteral(", ")));

    // Native names only when Qt really has this language/territory pair; a
    // QLocale constructed for an unknown pair silently becomes another locale.
    if (languageKnown) {
        const QLocale locale(language, script, territory);
        const bool exact = locale.language() == language && (!territoryKnown || locale.territory() == territory)
            && (script == QLocale::AnyScript || locale.script() == script);
        if (exact && !locale.nativeLanguageName().isEmpty()) {
            QStringList nativeDetail;
            if (territoryKnown && !locale.nativeTerritoryName().isEmpty())
                nativeDetail << locale.nativeTerritoryName();
            if (!modifier.isEmpty() && script == QLocale::AnyScript)
                nativeDetail << modifierText(modifier);
            names.native = capitalised(locale.nativeLanguageName());
            if (!nativeDetail.isEmpty())
                names.native += QStringLiteral(" (%1)").arg(nativeDetail.join(QStringLiteral(", ")));
        }
    }
    if (names.native.isEmpty())
        names.native = names.english;
    return names;
}

QVariantMap entry(const QString &code)
{
    const Names names = describe(code);
    return {
        {QStringLiteral("code"), code},
        {QStringLiteral("native"), names.native},
        {QStringLiteral("english"), names.english},
        {QStringLiteral("search"), (names.native + QLatin1Char(' ') + names.english + QLatin1Char(' ') + code).toCaseFolded()},
    };
}

QVariantList matching(const QVariantList &list, const QString &needle)
{
    const QString folded = needle.trimmed().toCaseFolded();
    if (folded.isEmpty())
        return list;
    QVariantList out;
    for (const QVariant &item : list) {
        if (item.toMap().value(QStringLiteral("search")).toString().contains(folded))
            out.append(item);
    }
    return out;
}

} // namespace

Locales::Locales(QObject *parent)
    : QObject(parent)
    , m_sourceFile(kSupported)
{
    QFile file(kSupported);
    if (!file.open(QIODevice::ReadOnly)) {
        m_problem = tr("The list of system locales (%1) cannot be read: %2. It belongs to the package “locales”.")
                        .arg(kSupported, file.errorString());
        return;
    }

    // answers.py: LOCALE_RE
    static const QRegularExpression engineName(QStringLiteral("\\A[a-z]{2,3}(_[A-Z]{2})?(\\.UTF-8)?(@[a-z]+)?\\z"));
    const QString utf8Suffix = QStringLiteral(".UTF-8");

    QSet<QString> known;            // every name in the file (what the engine checks against)
    QStringList utf8Names;          // names whose character set is UTF-8
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const QStringList fields = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (fields.isEmpty())
            continue;
        known.insert(fields.at(0));
        if (fields.size() >= 2 && fields.at(1) == QLatin1String("UTF-8"))
            utf8Names.append(fields.at(0));
    }

    for (const QString &name : std::as_const(utf8Names)) {
        if (!engineName.match(name).hasMatch()) {
            m_unsupported.append(entry(name));
            continue;
        }
        // What parse() turns the name into before looking it up.
        const QString looked = (name.endsWith(utf8Suffix) || name.contains(QLatin1Char('@'))) ? name : name + utf8Suffix;
        if (known.contains(looked))
            m_all.append(entry(name));
        else
            m_unsupported.append(entry(name));
    }

    QCollator collator(QLocale(QLocale::English));
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    const auto byEnglish = [&collator](const QVariant &a, const QVariant &b) {
        const QVariantMap ma = a.toMap(), mb = b.toMap();
        const int c = collator.compare(ma.value(QStringLiteral("english")).toString(),
                                       mb.value(QStringLiteral("english")).toString());
        return c != 0 ? c < 0 : ma.value(QStringLiteral("code")).toString() < mb.value(QStringLiteral("code")).toString();
    };
    std::sort(m_all.begin(), m_all.end(), byEnglish);
    std::sort(m_unsupported.begin(), m_unsupported.end(), byEnglish);

    if (m_all.isEmpty()) {
        m_problem = tr("%1 lists no UTF-8 locale the installer can use.").arg(kSupported);
        return;
    }

    // Default: the language of the running session, if it can be installed.
    const QString session = QLocale::system().name();       // "de_DE", or "C"
    for (const QString &candidate : {session + utf8Suffix, QStringLiteral("en_US.UTF-8")}) {
        if (contains(candidate)) {
            m_default = candidate;
            break;
        }
    }
    if (m_default.isEmpty())
        m_default = m_all.constFirst().toMap().value(QStringLiteral("code")).toString();
}

QVariantList Locales::filtered(const QString &needle) const
{
    return matching(m_all, needle);
}

QVariantList Locales::unsupportedMatching(const QString &needle) const
{
    if (needle.trimmed().isEmpty())
        return {};
    return matching(m_unsupported, needle);
}

bool Locales::contains(const QString &code) const
{
    for (const QVariant &item : m_all) {
        if (item.toMap().value(QStringLiteral("code")).toString() == code)
            return true;
    }
    return false;
}

QString Locales::displayName(const QString &code) const
{
    for (const QVariant &item : m_all) {
        const QVariantMap map = item.toMap();
        if (map.value(QStringLiteral("code")).toString() == code)
            return map.value(QStringLiteral("english")).toString();
    }
    return code;
}

QString Locales::territoryOf(const QString &code)
{
    const QString base = code.section(QLatin1Char('@'), 0, 0).section(QLatin1Char('.'), 0, 0);
    return base.section(QLatin1Char('_'), 1, 1);
}

QJsonObject Locales::toJson() const
{
    const auto codes = [](const QVariantList &list) {
        QJsonArray out;
        for (const QVariant &item : list) {
            const QVariantMap map = item.toMap();
            out.append(QJsonObject{
                {QStringLiteral("code"), map.value(QStringLiteral("code")).toString()},
                {QStringLiteral("native"), map.value(QStringLiteral("native")).toString()},
                {QStringLiteral("english"), map.value(QStringLiteral("english")).toString()},
            });
        }
        return out;
    };
    return {
        {QStringLiteral("source"), m_sourceFile},
        {QStringLiteral("problem"), m_problem},
        {QStringLiteral("default"), m_default},
        {QStringLiteral("selectable"), codes(m_all)},
        {QStringLiteral("notAcceptedByEngine"), codes(m_unsupported)},
    };
}
