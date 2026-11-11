// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QVariantList>

// The languages the installed system can be set up with.
//
// Source: /usr/share/i18n/SUPPORTED (package locales) of the running live
// system — the same file the engine checks an answer file against, and the
// same list the copied system generates its locales from. Only UTF-8 locales
// are offered. Names shown to the user come from Qt's CLDR data (QLocale); a
// locale Qt does not know is listed under its code.
//
// A locale is "selectable" when the engine accepts its name. The current
// engine (answers.py, parse()) appends ".UTF-8" to every name that has
// neither that suffix nor an @modifier and then looks the result up in
// SUPPORTED, so UTF-8 locales that glibc lists without the suffix (hi_IN,
// vi_VN, fa_IR, …) are rejected by it. They are kept in a second list so the
// search can say so instead of pretending the language does not exist.
class Locales : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    // [{ code, native, english, search }] sorted by English name
    Q_PROPERTY(QVariantList all READ all CONSTANT)
    Q_PROPERTY(int count READ count CONSTANT)
    Q_PROPERTY(int unsupportedCount READ unsupportedCount CONSTANT)
    Q_PROPERTY(QString sourceFile READ sourceFile CONSTANT)
    // Empty when the list could be read; otherwise why it is unavailable.
    Q_PROPERTY(QString problem READ problem CONSTANT)
    // The locale of the running session if it can be installed, else en_US.UTF-8.
    Q_PROPERTY(QString defaultCode READ defaultCode CONSTANT)

public:
    explicit Locales(QObject *parent = nullptr);

    QVariantList all() const { return m_all; }
    int count() const { return int(m_all.size()); }
    int unsupportedCount() const { return int(m_unsupported.size()); }
    QString sourceFile() const { return m_sourceFile; }
    QString problem() const { return m_problem; }
    QString defaultCode() const { return m_default; }

    Q_INVOKABLE QVariantList filtered(const QString &needle) const;
    // Locales that exist on this system but that the engine does not accept.
    Q_INVOKABLE QVariantList unsupportedMatching(const QString &needle) const;
    Q_INVOKABLE bool contains(const QString &code) const;
    Q_INVOKABLE QString displayName(const QString &code) const;
    // Two-letter territory of a locale code ("de_DE.UTF-8" → "DE"), or empty.
    Q_INVOKABLE static QString territoryOf(const QString &code);

    QJsonObject toJson() const;

private:
    QVariantList m_all;
    QVariantList m_unsupported;
    QString m_sourceFile;
    QString m_problem;
    QString m_default;
};
