// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QQmlEngine>
#include <QVariantList>

// Compositor side of the Accessibility page (text size and reduced motion are
// bound by the page to the Appearance singleton of org.shadow.ui).
//
// KWin effects — screen magnifier and friends — are switched through KWin's
// D-Bus interface on the session bus
//
//   org.kde.KWin  /Effects  org.kde.kwin.Effects
//     properties  listOfEffects, loadedEffects
//     methods     loadEffect(s) → b, unloadEffect(s), isEffectSupported(s) → b,
//                 reconfigureEffect(s)
//
// which takes effect at once, and are made permanent in
// ~/.config/kwinrc [Plugins] <effect>Enabled=true|false (KConfig), the key KWin
// reads at start. Only effects this KWin really has are offered.
//
// Effects that act on a shortcut (zoom in/out, invert, …) can be triggered
// from the page through org.kde.kglobalaccel /component/kwin invokeShortcut,
// and the keys bound to them are read from the same service.
//
// Pointer size: ~/.config/kcminputrc [Mouse] cursorSize, written with a
// KConfig change notification (KWin follows it immediately for the cursors it
// draws) plus the org.kde.KGlobalSettings.notifyChange(CursorChanged) signal.
// XCURSOR_SIZE in the environment of already running programs is unaffected.
class AccessibilityBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool compositorRunning READ compositorRunning NOTIFY changed)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY changed)
    Q_PROPERTY(bool canPersist READ canPersist CONSTANT)
    Q_PROPERTY(QString compositingType READ compositingType NOTIFY changed)
    Q_PROPERTY(bool softwareRendering READ softwareRendering NOTIFY changed)
    Q_PROPERTY(QVariantList effects READ effects NOTIFY changed)
    Q_PROPERTY(bool shortcutsAvailable READ shortcutsAvailable NOTIFY changed)
    Q_PROPERTY(int colorblindMode READ colorblindMode NOTIFY changed)
    Q_PROPERTY(int cursorSize READ cursorSize NOTIFY cursorChanged)
    Q_PROPERTY(QVariantList cursorSizes READ cursorSizes NOTIFY cursorChanged)
    Q_PROPERTY(QString cursorTheme READ cursorTheme NOTIFY cursorChanged)
    Q_PROPERTY(QString sessionCursorSize READ sessionCursorSize CONSTANT)

public:
    explicit AccessibilityBackend(QObject *parent = nullptr);

    bool ready() const { return m_ready; }
    bool compositorRunning() const { return m_running; }
    QString statusMessage() const { return m_statusMessage; }
    bool canPersist() const;
    // org.kde.kwin.Compositing.compositingType: "qpainter" is KWin's software
    // renderer. Its effects are written for OpenGL; with the software renderer
    // the magnifier is "active" without drawing anything and the pointer
    // marker crashed KWin 6.3.6 when triggered (seen in the nested test
    // session), so nothing is offered for switching on in that mode.
    QString compositingType() const { return m_compositingType; }
    bool softwareRendering() const { return m_compositingType == QLatin1String("qpainter") || m_compositingType == QLatin1String("none"); }
    // Each: id, title, description, icon, supported, loaded, stored (value in
    // kwinrc, true/false/absent → "storedKnown"), actions: [{name, label, keys}].
    QVariantList effects() const;
    bool shortcutsAvailable() const { return m_shortcutsAvailable; }
    int colorblindMode() const { return m_colorblindMode; }
    int cursorSize() const { return m_cursorSize; }
    QVariantList cursorSizes() const;
    QString cursorTheme() const { return m_cursorTheme; }
    QString sessionCursorSize() const { return qEnvironmentVariable("XCURSOR_SIZE"); }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void setEffectEnabled(const QString &id, bool enabled);
    Q_INVOKABLE void trigger(const QString &shortcutName);
    Q_INVOKABLE void setColorblindMode(int mode);
    Q_INVOKABLE void setCursorSize(int size);

    QJsonObject toJson() const;

Q_SIGNALS:
    void changed();
    void cursorChanged();
    void failed(const QString &message);

private:
    void setRunning(bool running);
    void loadCursor();
    void loadStored();
    void fetchEffects();
    void fetchShortcuts();
    bool store(const QString &id, bool enabled);
    void applyEffect(const QString &id, bool enabled, bool reportFailure);

    bool m_ready = false, m_running = false, m_shortcutsAvailable = false;
    QString m_statusMessage, m_compositingType;
    QStringList m_existing, m_loaded;
    QMap<QString, bool> m_supported;
    QMap<QString, bool> m_stored;            // kwinrc [Plugins] <id>Enabled, when present
    QMap<QString, QString> m_shortcutKeys;   // shortcut name → keys
    int m_colorblindMode = 0;
    int m_cursorSize = 24;
    QString m_cursorTheme;
};
