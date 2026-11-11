// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "terminalpane.h"

#include <QJsonObject>
#include <QList>
#include <QPointer>
#include <QWidget>

#include <functional>

class QSplitter;
class QVBoxLayout;

// The content of one tab: one terminal pane, or a tree of splitters with a
// pane in every leaf.
class TerminalTab : public QWidget
{
    Q_OBJECT
public:
    enum class Direction { Left, Right, Up, Down };

    explicit TerminalTab(QWidget *parent = nullptr);

    // Create the first pane of an empty tab.
    TerminalPane *setFirstPane(const PaneSpec &spec);
    // Split 'pane' and put a new pane to its right (Qt::Horizontal) or below
    // it (Qt::Vertical). Returns the new pane.
    TerminalPane *split(TerminalPane *pane, Qt::Orientation orientation, const PaneSpec &spec);
    // Remove a pane. The last pane leaving emits emptied().
    void closePane(TerminalPane *pane);

    QList<TerminalPane *> panes() const;
    int paneCount() const { return int(panes().size()); }
    TerminalPane *activePane() const;
    void setActivePane(TerminalPane *pane);
    // The pane next to the active one in a direction, by position on screen.
    TerminalPane *neighbour(Direction direction) const;
    bool focusNeighbour(Direction direction);

    // Name given by the user; empty means "follow the terminal".
    QString customTitle() const { return m_customTitle; }
    void setCustomTitle(const QString &title);
    QString title() const;
    QString toolTipText() const;
    // A bell rang in this tab while it was not the current one.
    bool hasBell() const { return m_bell; }
    void clearBell();
    bool isSsh() const;

    // ---- session ---------------------------------------------------------
    // The split layout with profile and working directory of every pane.
    // Never contains commands or terminal contents.
    QJsonObject saveLayout() const;
    // Rebuild a saved layout in this (empty) tab. 'makeSpec' turns a saved
    // pane description into what to start. Returns false if 'layout' is not
    // a valid description.
    bool restoreLayout(const QJsonObject &layout,
                       const std::function<PaneSpec(const QJsonObject &)> &makeSpec);

Q_SIGNALS:
    void activePaneChanged(TerminalPane *pane);
    void titleChanged();
    void paneCountChanged();
    void emptied();
    void bellChanged();
    void contextMenuRequested(TerminalPane *pane, const QPoint &globalPos, const QPoint &terminalPos);
    void notice(const QString &text, bool isError);
    void zoomChanged(TerminalPane *pane);

protected:
    void showEvent(QShowEvent *event) override;

private:
    TerminalPane *createPane(const PaneSpec &spec);
    QSplitter *createSplitter(Qt::Orientation orientation);
    QWidget *restoreNode(const QJsonObject &node, const std::function<PaneSpec(const QJsonObject &)> &makeSpec,
                         TerminalPane **active, int depth, int *budget);
    QJsonObject saveNode(const QWidget *node) const;
    void updatePaneFrames();
    void onPaneActivated(TerminalPane *pane);
    void onPaneBell(TerminalPane *pane);

    QVBoxLayout *m_layout;
    QPointer<TerminalPane> m_active;
    QString m_customTitle;
    bool m_bell = false;
};
