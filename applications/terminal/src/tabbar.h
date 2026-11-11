// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QTabBar>

class QLineEdit;

// The tab strip: pill-shaped tabs with an icon and a close button, reorder by
// dragging, rename in place on double-click, close with the middle button.
class TabBar : public QTabBar
{
    Q_OBJECT
public:
    explicit TabBar(QWidget *parent = nullptr);

    // Insert a tab with its close button; returns the index.
    int insertTerminalTab(int index, const QString &title);
    // Icon of a tab by symbolic name ("terminal", "server", "bell").
    void setTabGlyph(int index, const QString &iconName);
    // Start renaming a tab in place.
    void editTab(int index);
    bool isEditing() const;

Q_SIGNALS:
    void closeRequested(int index);
    // The user renamed a tab; an empty title means "automatic again".
    void renameRequested(int index, const QString &title);
    // Renaming ended, with or without a change.
    void editingFinished();
    void newTabRequested();
    void tabMenuRequested(int index, const QPoint &globalPos);

protected:
    QSize tabSizeHint(int index) const override;
    QSize minimumTabSizeHint(int index) const override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void tabLayoutChange() override;

private:
    void finishEditing(bool accept);

    QLineEdit *m_editor;
    int m_editingIndex = -1;
    QString m_editingOriginal;
};
