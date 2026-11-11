// SPDX-License-Identifier: GPL-3.0-or-later
#include "sshdialog.h"

#include "shells.h"
#include "theme.h"
#include "tokens.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStandardPaths>
#include <QToolButton>

namespace Ssh {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("Ssh", text);
}

// Host name, IPv4 or IPv6 address (optionally with a zone id). Never starts
// with '-', so it cannot be read as an option.
bool validHost(const QString &host)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_:][A-Za-z0-9._:%-]{0,254}$"));
    return re.match(host).hasMatch();
}

bool validUser(const QString &user)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_][A-Za-z0-9._+@\\\\$-]{0,63}$"));
    return re.match(user).hasMatch();
}

// One "[user@]host[:port]" element of a jump host list.
bool validJumpElement(const QString &element, QString *error)
{
    QString rest = element;
    const qsizetype at = rest.lastIndexOf(QLatin1Char('@'));
    if (at >= 0) {
        if (!validUser(rest.left(at))) {
            *error = tr("The user name of jump host “%1” is not valid.").arg(element);
            return false;
        }
        rest = rest.mid(at + 1);
    }

    QString host = rest;
    QString port;
    if (rest.startsWith(QLatin1Char('['))) {
        const qsizetype close = rest.indexOf(QLatin1Char(']'));
        if (close < 0) {
            *error = tr("Jump host “%1” has an unclosed “[”.").arg(element);
            return false;
        }
        host = rest.mid(1, close - 1);
        const QString tail = rest.mid(close + 1);
        if (!tail.isEmpty()) {
            if (!tail.startsWith(QLatin1Char(':')) || tail.size() < 2) {
                *error = tr("Jump host “%1” is not of the form [user@]host[:port].").arg(element);
                return false;
            }
            port = tail.mid(1);
        }
    } else if (rest.count(QLatin1Char(':')) == 1) {
        host = rest.section(QLatin1Char(':'), 0, 0);
        port = rest.section(QLatin1Char(':'), 1, 1);
    } else if (rest.count(QLatin1Char(':')) > 1) {
        *error = tr("Write the IPv6 address of jump host “%1” in [brackets].").arg(element);
        return false;
    }

    if (!validHost(host)) {
        *error = tr("The host name of jump host “%1” is not valid.").arg(element);
        return false;
    }
    if (rest.contains(QLatin1Char(':')) && !rest.startsWith(QLatin1Char('[')) && port.isEmpty()) {
        *error = tr("Jump host “%1” has an empty port.").arg(element);
        return false;
    }
    if (!port.isEmpty()) {
        bool ok = false;
        const int number = port.toInt(&ok);
        if (!ok || number < 1 || number > 65535) {
            *error = tr("The port of jump host “%1” must be between 1 and 65535.").arg(element);
            return false;
        }
    }
    return true;
}

QString normalizedJumpHosts(const QString &jumpHost)
{
    QStringList elements;
    const QStringList parts = jumpHost.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &part : parts)
        elements << part.trimmed();
    return elements.join(QLatin1Char(','));
}

} // namespace

QString program()
{
    return QStandardPaths::findExecutable(QStringLiteral("ssh"));
}

QString expandedIdentityFile(const SshTarget &target)
{
    QString path = target.identityFile.trimmed();
    if (path == QLatin1String("~"))
        path = Shells::homeDirectory();
    else if (path.startsWith(QLatin1String("~/")))
        path = Shells::homeDirectory() + path.mid(1);
    return path;
}

bool validate(const SshTarget &target, QString *error, QString *field)
{
    QString dummyError, dummyField;
    if (!error)
        error = &dummyError;
    if (!field)
        field = &dummyField;

    if (target.host.isEmpty()) {
        *error = tr("Enter the host to connect to.");
        *field = QStringLiteral("host");
        return false;
    }
    if (!validHost(target.host)) {
        *error = tr("The host may contain letters, digits and . - _ : % only, and must not start with “-”.");
        *field = QStringLiteral("host");
        return false;
    }
    if (!target.user.isEmpty() && !validUser(target.user)) {
        *error = tr("The user name contains characters that are not allowed.");
        *field = QStringLiteral("user");
        return false;
    }
    if (target.port < 1 || target.port > 65535) {
        *error = tr("The port must be between 1 and 65535.");
        *field = QStringLiteral("port");
        return false;
    }
    if (!target.identityFile.trimmed().isEmpty()) {
        const QFileInfo info(expandedIdentityFile(target));
        if (!info.isAbsolute()) {
            *error = tr("Give the identity file as an absolute path (or starting with ~/).");
            *field = QStringLiteral("identityFile");
            return false;
        }
        if (!info.isFile() || !info.isReadable()) {
            *error = tr("The identity file does not exist or cannot be read.");
            *field = QStringLiteral("identityFile");
            return false;
        }
    }
    const QStringList jumps = target.jumpHost.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (!target.jumpHost.trimmed().isEmpty() && jumps.isEmpty()) {
        *error = tr("The jump host is not of the form [user@]host[:port].");
        *field = QStringLiteral("jumpHost");
        return false;
    }
    for (const QString &jump : jumps) {
        if (!validJumpElement(jump.trimmed(), error)) {
            *field = QStringLiteral("jumpHost");
            return false;
        }
    }
    return true;
}

QStringList command(const SshTarget &target)
{
    QStringList argv{QStringLiteral("ssh")};
    if (target.port != 22)
        argv << QStringLiteral("-p") << QString::number(target.port);
    if (!target.user.isEmpty())
        argv << QStringLiteral("-l") << target.user;
    if (!target.identityFile.trimmed().isEmpty())
        argv << QStringLiteral("-i") << expandedIdentityFile(target);
    if (!target.jumpHost.trimmed().isEmpty())
        argv << QStringLiteral("-J") << normalizedJumpHosts(target.jumpHost);
    // "--" ends the options: whatever follows is the host name.
    argv << QStringLiteral("--") << target.host;
    return argv;
}

} // namespace Ssh

// ---- dialog ---------------------------------------------------------------------------

SshDialog::SshDialog(QWidget *parent)
    : Ui::Dialog(tr("New SSH Connection"), parent)
{
    setSubtitle(tr("Opens a tab that runs ssh. Passwords and passphrases are asked for in the terminal "
                   "and are never stored."));
    resize(Tokens::WindowMinWidth + Tokens::SpaceXxl, Tokens::WindowMinHeight);

    auto *columns = new QHBoxLayout;
    columns->setSpacing(Tokens::SpaceXl);
    content()->addLayout(columns, 1);

    // ---- saved destinations ---------------------------------------------------
    auto *left = new QVBoxLayout;
    left->setSpacing(Tokens::SpaceSm);
    left->addWidget(Ui::label(tr("SAVED CONNECTIONS"), "overline", this));
    m_list = new QListWidget(this);
    m_list->setAccessibleName(tr("Saved connections"));
    m_list->setFixedWidth(Tokens::SidebarWidth - Tokens::SpaceXl);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setTextElideMode(Qt::ElideRight);
    left->addWidget(m_list, 1);
    m_listEmpty = Ui::label(tr("No saved connections yet. Fill in the form and choose Save."), "caption", this);
    m_listEmpty->setWordWrap(true);
    m_listEmpty->setFixedWidth(Tokens::SidebarWidth - Tokens::SpaceXl);
    left->addWidget(m_listEmpty);
    left->addStretch(0);            // keeps the column at the top while the list is hidden
    columns->addLayout(left);

    // ---- form ------------------------------------------------------------------
    auto *form = new QGridLayout;
    form->setHorizontalSpacing(Tokens::SpaceLg);
    form->setVerticalSpacing(Tokens::SpaceMd);
    form->setColumnStretch(1, 1);
    int row = 0;
    auto addField = [this, form, &row](const QString &title, QWidget *field) {
        QLabel *label = Ui::label(title, "rowTitle", this);
        label->setBuddy(field);
        field->setAccessibleName(title);
        form->addWidget(label, row, 0);
        form->addWidget(field, row, 1);
        ++row;
    };

    m_name = Ui::lineEdit(this);
    m_name->setPlaceholderText(tr("Optional, shown in the list"));
    m_name->setMaxLength(64);
    addField(tr("Name"), m_name);

    m_host = Ui::lineEdit(this);
    m_host->setPlaceholderText(tr("Host name or IP address"));
    m_host->setMaxLength(255);
    addField(tr("Host"), m_host);

    m_user = Ui::lineEdit(this);
    m_user->setPlaceholderText(tr("Same as on this computer"));
    m_user->setMaxLength(64);
    addField(tr("User"), m_user);

    m_port = Ui::spinBox(1, 65535, this);
    m_port->setValue(22);
    m_port->setFixedWidth(Tokens::ControlHeight * 3);
    auto *portHolder = new QWidget(this);
    auto *portLayout = new QHBoxLayout(portHolder);
    portLayout->setContentsMargins(0, 0, 0, 0);
    portLayout->addWidget(m_port);
    portLayout->addStretch(1);
    {
        QLabel *label = Ui::label(tr("Port"), "rowTitle", this);
        label->setBuddy(m_port);
        m_port->setAccessibleName(tr("Port"));
        form->addWidget(label, row, 0);
        form->addWidget(portHolder, row, 1);
        ++row;
    }

    m_identity = Ui::lineEdit(this);
    m_identity->setPlaceholderText(tr("Optional private key, e.g. ~/.ssh/id_ed25519"));
    QToolButton *browse = Ui::iconButton(QStringLiteral("folder-open"), tr("Choose identity file…"), this);
    auto *identityHolder = new QWidget(this);
    auto *identityLayout = new QHBoxLayout(identityHolder);
    identityLayout->setContentsMargins(0, 0, 0, 0);
    identityLayout->setSpacing(Tokens::SpaceXs);
    identityLayout->addWidget(m_identity, 1);
    identityLayout->addWidget(browse);
    {
        QLabel *label = Ui::label(tr("Identity file"), "rowTitle", this);
        label->setBuddy(m_identity);
        m_identity->setAccessibleName(tr("Identity file"));
        form->addWidget(label, row, 0);
        form->addWidget(identityHolder, row, 1);
        ++row;
    }

    m_jump = Ui::lineEdit(this);
    m_jump->setPlaceholderText(tr("Optional, [user@]host[:port]"));
    addField(tr("Jump host"), m_jump);

    m_error = Ui::label(QString(), "error", this);
    m_error->setWordWrap(true);
    m_error->setMinimumHeight(Tokens::ControlHeight);
    m_error->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    form->addWidget(m_error, row++, 1);
    form->setRowStretch(row, 1);
    columns->addLayout(form, 1);

    // ---- footer ----------------------------------------------------------------
    m_deleteButton = addFooterButton(tr("Delete"), "ghost");
    addFooterStretch();
    m_saveButton = addFooterButton(tr("Save"));
    QPushButton *cancel = addFooterButton(tr("Cancel"));
    m_connectButton = addFooterButton(tr("Connect"), "primary");
    m_connectButton->setDefault(true);

    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_connectButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_saveButton, &QPushButton::clicked, this, &SshDialog::saveCurrent);
    connect(m_deleteButton, &QPushButton::clicked, this, &SshDialog::deleteCurrent);
    connect(browse, &QToolButton::clicked, this, &SshDialog::chooseIdentityFile);
    for (QLineEdit *edit : {m_name, m_host, m_user, m_identity, m_jump})
        connect(edit, &QLineEdit::textChanged, this, &SshDialog::validateForm);
    connect(m_port, &QSpinBox::valueChanged, this, &SshDialog::validateForm);
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int index) {
        if (!m_loading && index >= 0 && index < m_targets.size())
            showTarget(m_targets.at(index));
        validateForm();
    });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this] {
        if (m_connectButton->isEnabled())
            accept();
    });

    loadTargets();
    validateForm();
    m_host->setFocus();
}

void SshDialog::loadTargets()
{
    m_loading = true;
    m_targets = Settings::instance()->sshTargets();
    m_list->clear();
    for (const SshTarget &target : std::as_const(m_targets)) {
        QListWidgetItem *item = Ui::listItem(m_list, target.label());
        item->setToolTip(target.destination());
    }
    m_list->setVisible(!m_targets.isEmpty());
    m_listEmpty->setVisible(m_targets.isEmpty());
    m_loading = false;
}

void SshDialog::showTarget(const SshTarget &target)
{
    m_loading = true;
    m_name->setText(target.name);
    m_host->setText(target.host);
    m_user->setText(target.user);
    m_port->setValue(target.port);
    m_identity->setText(target.identityFile);
    m_jump->setText(target.jumpHost);
    m_loading = false;
}

SshTarget SshDialog::formTarget() const
{
    SshTarget target;
    target.name = m_name->text().simplified();
    target.host = m_host->text().trimmed();
    target.user = m_user->text().trimmed();
    target.port = m_port->value();
    target.identityFile = m_identity->text().trimmed();
    target.jumpHost = m_jump->text().trimmed();
    return target;
}

SshTarget SshDialog::target() const
{
    return formTarget();
}

void SshDialog::validateForm()
{
    if (m_loading)
        return;
    const SshTarget target = formTarget();
    QString error, field;
    const bool valid = Ssh::validate(target, &error, &field);
    const bool haveSsh = !Ssh::program().isEmpty();

    // An empty form is not an error yet; it just cannot be used.
    const bool untouched = target.host.isEmpty();
    QString message = untouched ? QString() : error;
    if (!haveSsh)
        message = tr("The ssh program is not installed (Debian package openssh-client).");
    m_error->setText(valid && haveSsh ? QString() : message);

    Ui::setProperty(m_host, "nxInvalid", !untouched && field == QLatin1String("host"));
    Ui::setProperty(m_user, "nxInvalid", field == QLatin1String("user"));
    Ui::setProperty(m_identity, "nxInvalid", field == QLatin1String("identityFile"));
    Ui::setProperty(m_jump, "nxInvalid", field == QLatin1String("jumpHost"));

    m_connectButton->setEnabled(valid && haveSsh);
    m_saveButton->setEnabled(valid);
    m_deleteButton->setEnabled(m_list->currentRow() >= 0);
}

void SshDialog::saveCurrent()
{
    const SshTarget target = formTarget();
    if (!Ssh::validate(target, nullptr))
        return;
    // Saving under the name (or destination) of an existing entry updates it.
    int index = -1;
    for (int i = 0; i < m_targets.size(); ++i) {
        if (m_targets.at(i).label() == target.label())
            index = i;
    }
    if (index >= 0)
        m_targets[index] = target;
    else
        m_targets.append(target);
    Settings::instance()->setSshTargets(m_targets);
    loadTargets();
    m_loading = true;
    m_list->setCurrentRow(index >= 0 ? index : int(m_targets.size()) - 1);
    m_loading = false;
    validateForm();
}

void SshDialog::deleteCurrent()
{
    const int index = m_list->currentRow();
    if (index < 0 || index >= m_targets.size())
        return;
    if (!Ui::confirm(this, tr("Delete this connection?"),
                     tr("“%1” is removed from the list of saved connections.").arg(m_targets.at(index).label()),
                     tr("Delete"), true))
        return;
    m_targets.removeAt(index);
    Settings::instance()->setSshTargets(m_targets);
    loadTargets();
    validateForm();
}

void SshDialog::chooseIdentityFile()
{
    QString start = Shells::homeDirectory() + QStringLiteral("/.ssh");
    if (!QFileInfo(start).isDir())
        start = Shells::homeDirectory();
    QFileDialog dialog(this, tr("Choose Identity File"), start);
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setFilter(QDir::Files | QDir::AllDirs | QDir::Hidden | QDir::NoDotAndDotDot);
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
        return;
    m_identity->setText(dialog.selectedFiles().constFirst());
}
