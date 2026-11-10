// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QVariantList>

class QAction;

// Global keyboard shortcuts of the shell, registered with KGlobalAccel (which
// the compositor hosts on Wayland). Users can rebind them; the defaults are
// listed in docs/desktop/shortcuts.md.
//
// The Meta key on its own opens the launcher: that is a compositor feature
// configured in /etc/xdg/kwinrc ([ModifierOnlyShortcuts]) which invokes the
// "launcher" action below over D-Bus.
class Shortcuts : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the shell as the 'shortcuts' context property")
    Q_PROPERTY(QVariantList list READ list CONSTANT)   // [{name, description, keys}]
    Q_PROPERTY(bool available READ available CONSTANT)

public:
    explicit Shortcuts(QObject *parent = nullptr);
    QVariantList list() const;
    bool available() const { return m_available; }

Q_SIGNALS:
    void triggered(const QString &name);

private:
    void add(const char *name, const QString &description, const QList<QKeySequence> &keys);

    QList<QAction *> m_actions;
    bool m_available = false;
};
