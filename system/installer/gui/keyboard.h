// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QVariantList>

// Keyboard layouts for the installed system, and — in a live session — the
// layout of the running session.
//
// Catalogue: /usr/share/X11/xkb/rules/evdev.lst (package xkb-data), sections
// "! layout" and "! variant". These are the names the engine writes to
// /etc/default/keyboard, the X11 keyboard file and kxkbrc of the new system.
//
// Running session: KWin publishes the layouts it really uses on the session
// bus (org.kde.keyboard, /Layouts, org.kde.KeyboardLayouts: getLayoutsList,
// getLayout, signals layoutListChanged/layoutChanged). KWin reads its layouts
// from ~/.config/kxkbrc [Layout] and re-reads the file when the signal
// org.kde.keyboard.reloadConfig is emitted on /Layouts — the same mechanism
// the Control Center's keyboard page uses.
//
// applyToSession() therefore writes kxkbrc, emits that signal and then ASKS
// KWin which layout it uses. "Applied" is only reported when KWin's answer
// names the requested layout. It is offered in a live session only (the live
// user's settings are thrown away at shutdown; an installed system's are not
// this program's to change), and only when KWin answers on the bus and the
// build has KConfig. In every other case the wizard says that typing uses the
// session's current layout.
class Keyboard : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QVariantList layouts READ layouts CONSTANT)              // [{ code, name, search }]
    Q_PROPERTY(QString catalogFile READ catalogFile CONSTANT)
    Q_PROPERTY(QString problem READ problem CONSTANT)                   // empty, or why there is no catalogue

    // What KWin reports. Empty list: not known (no KWin on the session bus).
    Q_PROPERTY(bool sessionKnown READ sessionKnown NOTIFY sessionChanged)
    Q_PROPERTY(QString sessionLayoutCode READ sessionLayoutCode NOTIFY sessionChanged)
    Q_PROPERTY(QString sessionLayoutName READ sessionLayoutName NOTIFY sessionChanged)

    Q_PROPERTY(bool canApply READ canApply NOTIFY sessionChanged)
    Q_PROPERTY(QString cannotApplyReason READ cannotApplyReason NOTIFY sessionChanged)
    // "" | "applying" | "applied" | "failed"
    Q_PROPERTY(QString applyState READ applyState NOTIFY applyStateChanged)
    Q_PROPERTY(QString applyMessage READ applyMessage NOTIFY applyStateChanged)

public:
    explicit Keyboard(QObject *parent = nullptr);

    QVariantList layouts() const { return m_layouts; }
    QString catalogFile() const { return m_catalogFile; }
    QString problem() const { return m_problem; }

    bool sessionKnown() const { return !m_sessionCodes.isEmpty(); }
    QString sessionLayoutCode() const;
    QString sessionLayoutName() const;
    bool canApply() const;
    QString cannotApplyReason() const;
    QString applyState() const { return m_applyState; }
    QString applyMessage() const { return m_applyMessage; }

    Q_INVOKABLE QVariantList filteredLayouts(const QString &needle) const;
    // [{ code, name }]; the first entry (code "") is the layout's default variant.
    Q_INVOKABLE QVariantList variantsFor(const QString &layout) const;
    Q_INVOKABLE bool hasLayout(const QString &layout) const { return m_layoutNames.contains(layout); }
    Q_INVOKABLE bool hasVariant(const QString &layout, const QString &variant) const;
    Q_INVOKABLE QString describe(const QString &layout, const QString &variant) const;
    // A layout that commonly goes with a locale ("de_DE.UTF-8" → { layout: "de", variant: "" });
    // an empty map when the catalogue has no obvious match. A suggestion, nothing more.
    Q_INVOKABLE QVariantMap suggestionForLocale(const QString &localeCode) const;
    // True when KWin reports 'layout' (and, where it can be told, 'variant') as the layout in use.
    Q_INVOKABLE bool sessionMatches(const QString &layout, const QString &variant) const;

    // Make the running live session use this layout. Returns false (and sets
    // applyState "failed") if that is not possible; the result of a started
    // attempt arrives through applyStateChanged().
    Q_INVOKABLE bool applyToSession(const QString &layout, const QString &variant);

    QJsonObject toJson() const;

Q_SIGNALS:
    void sessionChanged();
    void applyStateChanged();

private Q_SLOTS:
    void querySession();

private:
    void loadCatalog();
    void setApplyState(const QString &state, const QString &message);
    void checkApplied(int attempt);

    QString m_catalogFile;
    QString m_problem;
    QVariantList m_layouts;
    QHash<QString, QString> m_layoutNames;                          // layout → description
    QHash<QString, QList<QPair<QString, QString>>> m_variants;      // layout → [(variant, description)]

    bool m_busAvailable = false;
    QStringList m_sessionCodes;         // KWin's short names
    QStringList m_sessionNames;         // KWin's long names
    int m_sessionIndex = 0;

    QString m_applyState;
    QString m_applyMessage;
    QString m_pendingLayout;
    QString m_pendingVariant;
    quint64 m_applySerial = 0;
};
