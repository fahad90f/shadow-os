// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "settings.h"
#include "ui.h"

#include <QList>
#include <QStringList>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;

// Validation and command construction for SSH destinations.
//
// The ssh client is started with an argument vector; no shell ever sees the
// values. Every field is still checked against a strict pattern so that a
// value can never be taken for an ssh option (leading '-') or carry
// characters that have no place in it.
namespace Ssh {

// Absolute path of the ssh client, empty if it is not installed.
QString program();

// Check a destination. On failure *error explains what is wrong and *field
// names the offending field: "host", "user", "port", "identityFile", "jumpHost".
bool validate(const SshTarget &target, QString *error, QString *field = nullptr);

// Identity file with a leading "~/" expanded.
QString expandedIdentityFile(const SshTarget &target);

// Argument vector for a destination that passed validate(): "ssh" followed
// by its options and "--" before the host name.
QStringList command(const SshTarget &target);

} // namespace Ssh

// "New SSH Connection": a form for one destination and the list of saved
// destinations. Nothing secret is entered here or stored — passwords and
// passphrases are asked for by ssh inside the terminal.
class SshDialog : public Ui::Dialog
{
    Q_OBJECT
public:
    explicit SshDialog(QWidget *parent = nullptr);

    // The destination to connect to, after exec() returned Accepted.
    SshTarget target() const;

private:
    void loadTargets();
    void showTarget(const SshTarget &target);
    SshTarget formTarget() const;
    void validateForm();
    void saveCurrent();
    void deleteCurrent();
    void chooseIdentityFile();

    QList<SshTarget> m_targets;
    QListWidget *m_list;
    QLabel *m_listEmpty;
    QLineEdit *m_name;
    QLineEdit *m_host;
    QLineEdit *m_user;
    QSpinBox *m_port;
    QLineEdit *m_identity;
    QLineEdit *m_jump;
    QLabel *m_error;
    QPushButton *m_deleteButton;
    QPushButton *m_saveButton;
    QPushButton *m_connectButton;
    bool m_loading = false;
};
