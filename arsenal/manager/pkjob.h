// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <Transaction>

namespace Arsenal {

// Reachability of the PackageKit daemon (system D-Bus name
// org.freedesktop.PackageKit). Established with a real D-Bus round trip; the
// exact D-Bus error is kept when it fails. Nothing touches PackageKit-Qt until
// available() is true, so the application also runs without a system bus.
class PkService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool checking READ checking NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString backend READ backend NOTIFY changed)
public:
    explicit PkService(QObject *parent = nullptr);

    bool available() const { return m_available; }
    bool checking() const { return m_checking; }
    QString error() const { return m_error; }
    QString backend() const { return m_backend; }

    Q_INVOKABLE void probe();

Q_SIGNALS:
    void changed();
    void probed();

private:
    void fail(const QString &name, const QString &message);

    bool m_available = false;
    bool m_checking = false;
    bool m_watching = false;
    quint64 m_serial = 0;
    QString m_error;
    QString m_backend;
};

// One install or remove operation through PackageKit:
//
//   resolving    package names → PackageKit package ids
//   simulating   what would change (nothing is touched)
//   confirm      the user sees every package to be installed or removed and
//                the download size, and confirms or declines
//   running      the real transaction: status, percentage, cancellation
//   finished     result "success" | "failed" | "cancelled", with the error
//                PackageKit reported
//
// Packages that are not from a signed source are never installed, and a
// removal that would take parts of the SHADOW desktop with it is refused.
class PkJob : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString phase READ phase NOTIFY phaseChanged)
    Q_PROPERTY(bool active READ active NOTIFY phaseChanged)
    Q_PROPERTY(QString kind READ kind NOTIFY phaseChanged)
    Q_PROPERTY(QString title READ title NOTIFY phaseChanged)
    Q_PROPERTY(QString result READ result NOTIFY phaseChanged)
    Q_PROPERTY(QString errorKey READ errorKey NOTIFY phaseChanged)
    Q_PROPERTY(QString errorTitle READ errorTitle NOTIFY phaseChanged)
    Q_PROPERTY(QString errorDetails READ errorDetails NOTIFY phaseChanged)
    Q_PROPERTY(QString note READ note NOTIFY phaseChanged)
    Q_PROPERTY(QVariantMap changes READ changes NOTIFY phaseChanged)
    Q_PROPERTY(QStringList unavailable READ unavailable NOTIFY phaseChanged)
    Q_PROPERTY(QStringList requested READ requested NOTIFY phaseChanged)
    Q_PROPERTY(QString context READ context NOTIFY phaseChanged)
    Q_PROPERTY(int percentage READ percentage NOTIFY progressChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY progressChanged)
    Q_PROPERTY(QString currentItem READ currentItem NOTIFY progressChanged)
    Q_PROPERTY(bool cancellable READ cancellable NOTIFY progressChanged)
    Q_PROPERTY(double downloadRemaining READ downloadRemaining NOTIFY progressChanged)

public:
    explicit PkJob(QObject *parent = nullptr);

    // "idle" | "resolving" | "simulating" | "confirm" | "running" | "finished"
    QString phase() const { return m_phase; }
    bool active() const { return m_phase != QLatin1String("idle") && m_phase != QLatin1String("finished"); }
    QString kind() const { return m_kind; }            // "install" | "remove"
    QString title() const { return m_title; }
    QString result() const { return m_result; }
    QString errorKey() const { return m_errorKey; }
    QString errorTitle() const { return m_errorTitle; }
    QString errorDetails() const { return m_errorDetails; }
    QString note() const { return m_note; }
    // { install: [{name, version}], remove: [...], update: [...], downloadBytes }
    QVariantMap changes() const { return m_changes; }
    // Requested names PackageKit could not find in the configured repositories.
    QStringList unavailable() const { return m_unavailable; }
    // Free-form tag of the caller, e.g. "profile:web" (lets the UI offer a follow-up).
    QString context() const { return m_context; }
    // The package names the user asked for (everything else in 'changes' follows from them).
    QStringList requested() const { return m_names; }
    int percentage() const { return m_percentage; }
    QString statusText() const;
    QString currentItem() const { return m_currentItem; }
    bool cancellable() const;
    double downloadRemaining() const { return double(m_downloadRemaining); }

    // Start an operation. Returns false if another one is still active.
    bool install(const QStringList &names, const QString &title, const QString &context);
    bool remove(const QStringList &names, bool autoRemove, const QString &title, const QString &context);

    Q_INVOKABLE void confirm();
    Q_INVOKABLE void cancel();
    // Forget a finished job (closes the result view).
    Q_INVOKABLE void dismiss();

    static QString errorKeyOf(PackageKit::Transaction::Error error);

Q_SIGNALS:
    void phaseChanged();
    void progressChanged();
    void finished(bool success);

private:
    using T = PackageKit::Transaction;

    void begin(const QString &kind, const QStringList &names, const QString &title, const QString &context);
    void setPhase(const QString &phase);
    void resolve();
    void resolved(T::Exit exit);
    void simulate();
    void simulated(T::Exit exit);
    void run();
    void ran(T::Exit exit);
    void watch(T *transaction);
    void finish(const QString &result);
    void fail(const QString &key, const QString &title, const QString &details);
    void failFromTransaction(T::Exit exit, const QString &fallbackTitle);

    QString m_phase = QStringLiteral("idle");
    QString m_kind;
    QString m_title;
    QString m_context;
    QStringList m_names;
    bool m_autoRemove = false;
    bool m_cancelRequested = false;

    QPointer<T> m_transaction;
    QStringList m_ids;                          // resolved package ids to act on
    QHash<QString, bool> m_installedNames;      // resolve: name → an installed version exists
    QHash<QString, QString> m_availableIds;     // resolve: name → newest available id
    QStringList m_unavailable;
    QVariantList m_install, m_removeList, m_update;
    QStringList m_untrusted;
    qulonglong m_simulatedDownload = 0;
    QVariantMap m_changes;

    T::Status m_status = T::StatusUnknown;
    int m_percentage = -1;
    bool m_allowCancel = false;
    qulonglong m_downloadRemaining = 0;
    QString m_currentItem;

    bool m_hasError = false;
    T::Error m_error = T::ErrorUnknown;
    QString m_errorText;

    QString m_result;
    QString m_errorKey;
    QString m_errorTitle;
    QString m_errorDetails;
    QString m_note;
};

} // namespace Arsenal
