// SPDX-License-Identifier: GPL-3.0-or-later
#include "polkitagent.h"

#include <PolkitQt1/Subject>
#include <QCoreApplication>

#include <pwd.h>
#include <unistd.h>

namespace {
const int kMaxAttempts = 3;
}

PolkitAgent::PolkitAgent(QObject *parent)
    : PolkitQt1::Agent::Listener(parent)
{
}

PolkitAgent::~PolkitAgent()
{
    if (m_session)
        m_session->cancel();
}

bool PolkitAgent::registerAgent()
{
    const PolkitQt1::UnixSessionSubject subject(QCoreApplication::applicationPid());
    const bool ok = registerListener(subject, QStringLiteral("/org/shadow/PolicyKit1/AuthenticationAgent"));
    if (ok != m_registered) {
        m_registered = ok;
        Q_EMIT registeredChanged();
    }
    return ok;
}

QVariantList PolkitAgent::identities() const
{
    QVariantList out;
    for (PolkitQt1::Identity identity : m_identities) {
        QString name = identity.toString().section(QLatin1Char(':'), 1);
        QString fullName;
        const PolkitQt1::UnixUserIdentity user = identity.toUnixUserIdentity();
        if (user.isValid()) {
            if (const passwd *pw = getpwuid(user.uid())) {
                name = QString::fromLocal8Bit(pw->pw_name);
                fullName = QString::fromLocal8Bit(pw->pw_gecos).section(QLatin1Char(','), 0, 0);
            }
        }
        out << QVariantMap{{QStringLiteral("name"), name}, {QStringLiteral("fullName"), fullName.isEmpty() ? name : fullName}};
    }
    return out;
}

void PolkitAgent::initiateAuthentication(const QString &actionId, const QString &message, const QString &iconName,
                                         const PolkitQt1::Details &, const QString &cookie,
                                         const PolkitQt1::Identity::List &identities,
                                         PolkitQt1::Agent::AsyncResult *result)
{
    if (m_active) {
        // One conversation at a time; polkit queues or the caller retries.
        result->setError(QStringLiteral("Another authentication request is already in progress."));
        result->setCompleted();
        return;
    }
    if (identities.isEmpty()) {
        result->setError(QStringLiteral("No identity is allowed to authorise this action."));
        result->setCompleted();
        return;
    }
    m_actionId = actionId;
    m_message = message;
    m_iconName = iconName;
    m_cookie = cookie;
    m_identities = identities;
    m_result = result;
    m_attempts = 0;
    m_cancelled = false;
    m_errorText.clear();
    m_infoText.clear();

    // Prefer the person at the keyboard when they are among the permitted identities.
    m_identityIndex = 0;
    const uid_t me = getuid();
    for (int i = 0; i < m_identities.size(); ++i) {
        PolkitQt1::Identity identity = m_identities.at(i);
        const PolkitQt1::UnixUserIdentity user = identity.toUnixUserIdentity();
        if (user.isValid() && user.uid() == me) {
            m_identityIndex = i;
            break;
        }
    }

    m_active = true;
    Q_EMIT requestChanged();
    Q_EMIT identityChanged();
    Q_EMIT feedbackChanged();
    Q_EMIT activeChanged();
    startSession();
}

void PolkitAgent::startSession()
{
    if (m_session) {
        m_session->disconnect(this);
        m_session->deleteLater();
    }
    m_prompt.clear();
    m_echo = false;
    Q_EMIT promptChanged();
    setBusy(true);

    m_session = new PolkitQt1::Agent::Session(m_identities.at(m_identityIndex), m_cookie, m_result, this);
    connect(m_session, &PolkitQt1::Agent::Session::request, this, [this](const QString &request, bool echo) {
        m_prompt = request.trimmed();
        m_echo = echo;
        Q_EMIT promptChanged();
        setBusy(false);
    });
    connect(m_session, &PolkitQt1::Agent::Session::showError, this, [this](const QString &text) {
        m_errorText = text;
        Q_EMIT feedbackChanged();
    });
    connect(m_session, &PolkitQt1::Agent::Session::showInfo, this, [this](const QString &text) {
        m_infoText = text;
        Q_EMIT feedbackChanged();
    });
    connect(m_session, &PolkitQt1::Agent::Session::completed, this, [this](bool gainedAuthorization) {
        if (gainedAuthorization || m_cancelled) {
            finish();
            return;
        }
        ++m_attempts;
        if (m_attempts >= kMaxAttempts) {
            finish();
            return;
        }
        m_errorText = tr("Authentication failed. Please try again.");
        Q_EMIT feedbackChanged();
        startSession();
    });
    m_session->initiate();
}

void PolkitAgent::respond(const QString &secret)
{
    if (!m_session || m_busy)
        return;
    m_errorText.clear();
    Q_EMIT feedbackChanged();
    setBusy(true);
    m_session->setResponse(secret);
}

void PolkitAgent::selectIdentity(int index)
{
    if (!m_active || index < 0 || index >= m_identities.size() || index == m_identityIndex)
        return;
    m_identityIndex = index;
    Q_EMIT identityChanged();
    m_errorText.clear();
    Q_EMIT feedbackChanged();
    if (m_session)
        m_session->cancel();
    startSession();
}

void PolkitAgent::cancel()
{
    if (!m_active)
        return;
    m_cancelled = true;
    if (m_session)
        m_session->cancel();
    finish();
}

void PolkitAgent::finish()
{
    if (!m_active)
        return;
    if (m_session) {
        m_session->result()->setCompleted();
        m_session->disconnect(this);
        m_session->deleteLater();
        m_session = nullptr;
    } else if (m_result) {
        m_result->setCompleted();
    }
    m_result = nullptr;
    m_active = false;
    m_cookie.clear();
    setBusy(false);
    Q_EMIT activeChanged();
}

bool PolkitAgent::initiateAuthenticationFinish()
{
    return true;
}

void PolkitAgent::cancelAuthentication()
{
    // polkit withdrew the request (the caller went away or timed out).
    cancel();
}

void PolkitAgent::setBusy(bool busy)
{
    if (busy != m_busy) {
        m_busy = busy;
        Q_EMIT busyChanged();
    }
}
