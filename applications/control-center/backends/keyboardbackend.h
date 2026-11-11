// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractItemModel>
#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QStringList>
#include <QVariantList>

namespace NetHw {
class RowModel;
}

// Keyboard page: layouts, layout-switching / Compose / Caps Lock options and
// key repeat for the KWin Wayland session.
//
// Sources and sinks
//   /usr/share/X11/xkb/rules/evdev.lst (base.lst as fallback)
//                          catalogue of layouts, variants and xkb options
//   ~/.config/kxkbrc [Layout]   LayoutList, VariantList, Use, Options,
//                          ResetOldOptions — read by KWin; written through
//                          KConfig. KWin only applies Options when
//                          ResetOldOptions is true, so both are written together.
//   ~/.config/kcminputrc [Keyboard]   RepeatDelay (ms), RepeatRate (per second),
//                          written with KConfig::Notify (KWin watches the file)
//   session bus            signal org.kde.keyboard.reloadConfig on /Layouts makes
//                          KWin re-read kxkbrc (verified against KWin 6.3: the
//                          org.kde.KWin /KWin reconfigure method alone does not
//                          reload layouts — it is called as well, for key repeat);
//                          org.kde.KeyboardLayouts on org.kde.keyboard /Layouts
//                          reports the layouts KWin really uses
//   system bus             org.freedesktop.locale1 (X11Layout, X11Variant) — the
//                          system default that "localectl status" prints; when
//                          systemd-localed cannot be reached, /etc/default/keyboard
//
//   session bus            org.kde.kglobalaccel (KGlobalAccel, hosted by KWin on
//                          Wayland): /kglobalaccel allComponents() and
//                          allShortcutInfos() of every /component/<name> — the
//                          global (desktop-wide) shortcuts and the keys they
//                          are bound to right now. The list is read-only: this
//                          page does not change shortcuts.
//
// shortcuts rows: key, component, componentName, action, name, keys (text,
//   several bindings separated by ", "), assigned, custom (keys differ from the
//   action's default)
//
// Without KConfig (NEXUS_HAVE_KCONFIG undefined) nothing can be read or
// written and the page shows the "unsupported-build" state.
class KeyboardBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY stateChanged)
    Q_PROPERTY(bool compositorRunning READ compositorRunning NOTIFY compositorChanged)

    Q_PROPERTY(QAbstractItemModel *layouts READ layouts CONSTANT)
    Q_PROPERTY(int layoutCount READ layoutCount NOTIFY layoutsChanged)
    Q_PROPERTY(int maxLayouts READ maxLayouts CONSTANT)
    Q_PROPERTY(QVariantList availableLayouts READ availableLayouts NOTIFY catalogChanged)
    Q_PROPERTY(QStringList activeLayouts READ activeLayouts NOTIFY activeLayoutsChanged)
    Q_PROPERTY(int activeLayoutIndex READ activeLayoutIndex NOTIFY activeLayoutsChanged)
    Q_PROPERTY(QString systemLayout READ systemLayout NOTIFY systemLayoutChanged)
    Q_PROPERTY(QString systemLayoutSource READ systemLayoutSource NOTIFY systemLayoutChanged)

    Q_PROPERTY(QVariantList switchOptions READ switchOptions NOTIFY catalogChanged)
    Q_PROPERTY(QVariantList composeOptions READ composeOptions NOTIFY catalogChanged)
    Q_PROPERTY(QVariantList capsOptions READ capsOptions NOTIFY catalogChanged)
    Q_PROPERTY(QString switchOption READ switchOption NOTIFY optionsChanged)
    Q_PROPERTY(QString composeOption READ composeOption NOTIFY optionsChanged)
    Q_PROPERTY(QString capsOption READ capsOption NOTIFY optionsChanged)
    Q_PROPERTY(QStringList otherOptions READ otherOptions NOTIFY optionsChanged)

    Q_PROPERTY(int repeatDelay READ repeatDelay NOTIFY repeatChanged)
    Q_PROPERTY(int repeatRate READ repeatRate NOTIFY repeatChanged)
    Q_PROPERTY(bool repeatConfigured READ repeatConfigured NOTIFY repeatChanged)

    // Global shortcuts registered with KGlobalAccel (read-only).
    Q_PROPERTY(QAbstractItemModel *shortcuts READ shortcuts CONSTANT)
    // "loading", "ok", "no-session-bus", "no-service" or "failed".
    Q_PROPERTY(QString shortcutsState READ shortcutsState NOTIFY shortcutsChanged)
    Q_PROPERTY(QString shortcutsStatus READ shortcutsStatus NOTIFY shortcutsChanged)
    // Rows shown (after the filter) / actions that have a key / all actions.
    Q_PROPERTY(int shortcutCount READ shortcutCount NOTIFY shortcutsChanged)
    Q_PROPERTY(int assignedShortcutCount READ assignedShortcutCount NOTIFY shortcutsChanged)
    Q_PROPERTY(int totalShortcutCount READ totalShortcutCount NOTIFY shortcutsChanged)
    Q_PROPERTY(QString shortcutFilter READ shortcutFilter WRITE setShortcutFilter NOTIFY shortcutFilterChanged)
    Q_PROPERTY(bool showUnassignedShortcuts READ showUnassignedShortcuts WRITE setShowUnassignedShortcuts NOTIFY shortcutFilterChanged)

public:
    explicit KeyboardBackend(QObject *parent = nullptr);

    bool ready() const { return m_ready; }
    bool available() const { return m_state == QLatin1String("ok"); }
    QString state() const { return m_state; }
    QString statusMessage() const { return m_statusMessage; }
    bool compositorRunning() const { return m_compositorRunning; }

    QAbstractItemModel *layouts() const;
    int layoutCount() const { return int(m_layoutList.size()); }
    int maxLayouts() const { return 4; }       // XKB supports four groups
    QVariantList availableLayouts() const { return m_availableLayouts; }
    QStringList activeLayouts() const { return m_activeLayouts; }
    int activeLayoutIndex() const { return m_activeLayoutIndex; }
    QString systemLayout() const { return m_systemLayout; }
    QString systemLayoutSource() const { return m_systemLayoutSource; }

    QVariantList switchOptions() const { return m_switchOptions; }
    QVariantList composeOptions() const { return m_composeOptions; }
    QVariantList capsOptions() const { return m_capsOptions; }
    QString switchOption() const { return optionWithPrefix(QStringLiteral("grp:")); }
    QString composeOption() const { return optionWithPrefix(QStringLiteral("compose:")); }
    QString capsOption() const { return optionWithPrefix(QStringLiteral("caps:")); }
    QStringList otherOptions() const;

    int repeatDelay() const { return m_repeatDelay; }
    int repeatRate() const { return m_repeatRate; }
    bool repeatConfigured() const { return m_repeatConfigured; }

    QAbstractItemModel *shortcuts() const;
    QString shortcutsState() const { return m_shortcutsState; }
    QString shortcutsStatus() const { return m_shortcutsStatus; }
    int shortcutCount() const;
    int assignedShortcutCount() const { return m_assignedShortcuts; }
    int totalShortcutCount() const { return int(m_allShortcuts.size()); }
    QString shortcutFilter() const { return m_shortcutFilter; }
    void setShortcutFilter(const QString &filter);
    bool showUnassignedShortcuts() const { return m_showUnassigned; }
    void setShowUnassignedShortcuts(bool show);
    Q_INVOKABLE void refreshShortcuts();

    // Variants of a layout: [{ variant, name }], the default variant first.
    Q_INVOKABLE QVariantList variantsFor(const QString &layout) const;
    Q_INVOKABLE QString layoutName(const QString &layout, const QString &variant) const;

    // Each returns false (after emitting failed()) if the change was refused.
    Q_INVOKABLE bool addLayout(const QString &layout, const QString &variant);
    Q_INVOKABLE bool removeLayout(int index);
    Q_INVOKABLE bool moveLayout(int from, int to);
    // 'option' is a full xkb option ("grp:alt_shift_toggle") or empty for none.
    Q_INVOKABLE bool setSwitchOption(const QString &option);
    Q_INVOKABLE bool setComposeOption(const QString &option);
    Q_INVOKABLE bool setCapsOption(const QString &option);
    Q_INVOKABLE bool setRepeat(int delayMs, int ratePerSecond);

    Q_INVOKABLE void refresh();
    QJsonObject toJson() const;

Q_SIGNALS:
    void readyChanged();
    void stateChanged();
    void compositorChanged();
    void layoutsChanged();
    void catalogChanged();
    void activeLayoutsChanged();
    void systemLayoutChanged();
    void optionsChanged();
    void repeatChanged();
    void shortcutsChanged();
    void shortcutFilterChanged();
    void failed(const QString &message);

private Q_SLOTS:
    void queryActiveLayouts();

private:
    void loadCatalog();
    void loadConfig();
    void querySystemLayout();
    void readDefaultKeyboardFile();
    void setCompositorRunning(bool running);
    bool saveLayoutConfig();
    void notifyCompositor();
    void rebuildLayoutModel();
    QString optionWithPrefix(const QString &prefix) const;
    bool setOptionWithPrefix(const QString &prefix, const QString &option, const QVariantList &allowed);
    void markStep();
    void setShortcutsState(const QString &state, const QString &status);
    void finishShortcuts(quint64 generation, const QVariantList &rows);
    void publishShortcuts();

    bool m_ready = false;
    int m_pendingSteps = 0;
    QString m_state;
    QString m_statusMessage;
    bool m_compositorRunning = false;

    // catalogue
    QString m_catalogFile;
    QVariantList m_availableLayouts;                    // [{ layout, name }]
    QHash<QString, QString> m_layoutNames;             // layout → description
    QHash<QString, QList<QPair<QString, QString>>> m_variants;   // layout → [(variant, description)]
    QVariantList m_switchOptions, m_composeOptions, m_capsOptions;

    // configuration
    QStringList m_layoutList;
    QStringList m_variantList;
    QStringList m_options;
    int m_repeatDelay = 600;
    int m_repeatRate = 25;
    bool m_repeatConfigured = false;

    // live state
    QStringList m_activeLayouts;
    int m_activeLayoutIndex = -1;
    QString m_systemLayout;
    QString m_systemLayoutSource;

    NetHw::RowModel *m_layoutModel = nullptr;

    // global shortcuts
    NetHw::RowModel *m_shortcutModel = nullptr;
    QVariantList m_allShortcuts;
    QString m_shortcutsState = QStringLiteral("loading");
    QString m_shortcutsStatus;
    QString m_shortcutFilter;
    bool m_showUnassigned = false;
    int m_assignedShortcuts = 0;
    quint64 m_shortcutGeneration = 0;
    bool m_shortcutsLoadedOnce = false;
};
