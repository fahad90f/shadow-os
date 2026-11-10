// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <PolkitQt1/Agent/Listener>
#include <PolkitQt1/Agent/Session>
#include <PolkitQt1/Details>
#include <PolkitQt1/Identity>
#include <QPointer>
#include <QQmlEngine>
#include <QVariantList>

// polkit authentication agent of the NEXUS HORIZON session.
//
// When a privileged action needs authorisation, polkit asks the session's
// agent to authenticate one of the permitted identities. The agent only
// relays the PAM conversation between polkit's helper and the dialog
// (AuthDialog.qml): the secret goes straight to polkit's setuid helper and is
// neither stored nor logged here.
class PolkitAgent : public PolkitQt1::Agent::Listener
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the shell as the 'polkitAgent' context property")
    Q_PROPERTY(bool registered READ registered NOTIFY registeredChanged)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(QString message READ message NOTIFY requestChanged)
    Q_PROPERTY(QString actionId READ actionId NOTIFY requestChanged)
    Q_PROPERTY(QString iconName READ iconName NOTIFY requestChanged)
    Q_PROPERTY(QVariantList identities READ identities NOTIFY requestChanged)   // [{name, fullName}]
    Q_PROPERTY(int identityIndex READ identityIndex NOTIFY identityChanged)
    Q_PROPERTY(QString prompt READ prompt NOTIFY promptChanged)
    Q_PROPERTY(bool echo READ echo NOTIFY promptChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY feedbackChanged)
    Q_PROPERTY(QString infoText READ infoText NOTIFY feedbackChanged)

public:
    explicit PolkitAgent(QObject *parent = nullptr);
    ~PolkitAgent() override;

    // Register as the authentication agent of this login session. Fails when
    // there is no logind session or another agent is already registered.
    bool registerAgent();

    bool registered() const { return m_registered; }
    bool active() const { return m_active; }
    QString message() const { return m_message; }
    QString actionId() const { return m_actionId; }
    QString iconName() const { return m_iconName; }
    QVariantList identities() const;
    int identityIndex() const { return m_identityIndex; }
    QString prompt() const { return m_prompt; }
    bool echo() const { return m_echo; }
    bool busy() const { return m_busy; }
    QString errorText() const { return m_errorText; }
    QString infoText() const { return m_infoText; }

    Q_INVOKABLE void respond(const QString &secret);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void selectIdentity(int index);

public Q_SLOTS:
    void initiateAuthentication(const QString &actionId, const QString &message, const QString &iconName,
                                const PolkitQt1::Details &details, const QString &cookie,
                                const PolkitQt1::Identity::List &identities,
                                PolkitQt1::Agent::AsyncResult *result) override;
    bool initiateAuthenticationFinish() override;
    void cancelAuthentication() override;

Q_SIGNALS:
    void registeredChanged();
    void activeChanged();
    void requestChanged();
    void identityChanged();
    void promptChanged();
    void busyChanged();
    void feedbackChanged();

private:
    void startSession();
    void finish();
    void setBusy(bool busy);

    bool m_registered = false, m_active = false, m_echo = false, m_busy = false, m_cancelled = false;
    QString m_message, m_actionId, m_iconName, m_cookie, m_prompt, m_errorText, m_infoText;
    PolkitQt1::Identity::List m_identities;
    int m_identityIndex = 0, m_attempts = 0;
    PolkitQt1::Agent::AsyncResult *m_result = nullptr;
    QPointer<PolkitQt1::Agent::Session> m_session;
};
