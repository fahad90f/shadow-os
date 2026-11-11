// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QQmlEngine>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>

// Region & Language page.
//
// The system locale is read from and written through systemd-localed,
// org.freedesktop.locale1 on the system bus (object /org/freedesktop/locale1):
//
//   property  Locale   as   — "LANG=de_DE.UTF-8", "LC_TIME=en_GB.UTF-8", …
//   method    SetLocale(as locale, b interactive) — replaces the whole set;
//             authorised by polkit (org.freedesktop.locale1.set-locale)
//
// Without the service the page is read-only and shows /etc/locale.conf or
// /etc/default/locale.
//
// Debian's systemd package forbids SetLocale to everybody through D-Bus policy
// (/usr/share/dbus-1/system.d/systemd-localed-read-only.conf: there the
// locale is set with update-locale). Where that file exists, or when the bus
// rejects the call for that reason, the locale is written by the helper below
// instead ("set", which runs update-locale).
//
// Offered locales: the UTF-8 entries of /usr/share/i18n/SUPPORTED (glibc) plus
// whatever "locale -a" reports as generated. A locale that is not generated
// yet is generated first by /usr/libexec/shadow/language-helper, run through
// pkexec (polkit action org.shadow.controlcenter.language.configure), which
// enables its line in /etc/locale.gen and runs locale-gen.
//
// The page works on a selection (language plus the LC_TIME, LC_NUMERIC,
// LC_MONETARY and LC_MEASUREMENT formats) that apply() writes in one step.
// Running programs keep their locale: the change takes effect at next login.
class LanguageBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY stateChanged)
    Q_PROPERTY(QString source READ source NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString busyText READ busyText NOTIFY busyChanged)
    Q_PROPERTY(QVariantList locales READ locales NOTIFY localesChanged)
    Q_PROPERTY(bool localeListComplete READ localeListComplete NOTIFY localesChanged)
    Q_PROPERTY(QString systemLanguage READ systemLanguage NOTIFY stateChanged)
    Q_PROPERTY(QString sessionLanguage READ sessionLanguage CONSTANT)
    Q_PROPERTY(QString language READ language NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList formats READ formats NOTIFY selectionChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY selectionChanged)
    Q_PROPERTY(QStringList toGenerate READ toGenerate NOTIFY selectionChanged)
    Q_PROPERTY(bool helperAvailable READ helperAvailable CONSTANT)
    Q_PROPERTY(bool usesHelperToApply READ usesHelperToApply NOTIFY stateChanged)
    Q_PROPERTY(bool loginPending READ loginPending NOTIFY stateChanged)

public:
    explicit LanguageBackend(QObject *parent = nullptr);

    bool ready() const { return m_ready; }
    bool available() const { return m_available; }
    QString statusMessage() const { return m_statusMessage; }
    QString source() const { return m_source; }
    bool busy() const { return m_busy; }
    QString busyText() const { return m_busyText; }
    // id, name (native), englishName, generated
    QVariantList locales() const { return m_locales; }
    bool localeListComplete() const { return m_supportedRead; }
    QString systemLanguage() const { return m_system.value(QStringLiteral("LANG")); }
    QString sessionLanguage() const;
    QString language() const { return m_selected.value(QStringLiteral("LANG")); }
    // One per format category: category, title, value (selected locale, empty
    // = same as the language), effective, effectiveName, example, generated.
    QVariantList formats() const;
    bool dirty() const;
    QStringList toGenerate() const;
    bool helperAvailable() const;
    // SetLocale of systemd-localed is closed on this system; the helper writes the locale.
    bool usesHelperToApply() const { return m_localedReadOnly; }
    // The system setting differs from what this session was started with.
    bool loginPending() const;

    Q_INVOKABLE QString displayName(const QString &locale) const;
    Q_INVOKABLE bool isGenerated(const QString &locale) const;
    Q_INVOKABLE void selectLanguage(const QString &locale);
    // An empty locale means "same as the language".
    Q_INVOKABLE void selectFormat(const QString &category, const QString &locale);
    Q_INVOKABLE void revert();
    Q_INVOKABLE void apply();
    Q_INVOKABLE void refresh();

    QJsonObject toJson() const;

Q_SIGNALS:
    void stateChanged();
    void busyChanged();
    void localesChanged();
    void selectionChanged();
    void failed(const QString &message);
    void applied(const QString &message);

private Q_SLOTS:
    void onPropertiesChanged(const QString &interface, const QVariantMap &changed, const QStringList &invalidated);

private:
    void loadLocales();
    void readWithoutService(const QString &reason);
    void setSystem(const QStringList &assignments);
    void setBusy(bool busy, const QString &text = QString());
    void sendSetLocale();
    QStringList assignments() const;
    void runHelper(const QStringList &generate, const QStringList &set);
    QString example(const QString &category, const QString &locale) const;

    bool m_ready = false, m_available = false, m_busy = false, m_supportedRead = false;
    bool m_selectionTouched = false;
    bool m_localedReadOnly = false;
    QString m_statusMessage, m_source, m_busyText;
    QVariantList m_locales;
    QStringList m_known;                 // ids offered
    QSet<QString> m_generated;           // normalised names from "locale -a"
    QMap<QString, QString> m_system;     // LANG, LC_* as the system has them
    QMap<QString, QString> m_selected;   // LANG + the four format categories
};
