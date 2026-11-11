// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QToolButton;

// The "find in scrollback" bar floating over the terminals. It only holds the search
// criteria and reports what the user asked for; the window runs the search on
// the active terminal and tells the bar the outcome.
class SearchBar : public QWidget
{
    Q_OBJECT
public:
    enum class Status { Idle, Found, NotFound, InvalidPattern };

    explicit SearchBar(QWidget *parent = nullptr);

    QString pattern() const;
    bool caseSensitive() const;
    bool regularExpression() const;

    // Show the bar and put the cursor into the search field.
    void activate();
    void setStatus(Status status);

Q_SIGNALS:
    // The pattern or an option changed: search again from the current match.
    void criteriaChanged();
    void nextRequested();
    void previousRequested();
    void closed();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QLineEdit *m_edit;
    QToolButton *m_caseButton;
    QToolButton *m_regexButton;
    QToolButton *m_previousButton;
    QToolButton *m_nextButton;
    QLabel *m_status;
};
