// SPDX-License-Identifier: GPL-3.0-or-later
#include "interaction.h"

#include <KFileUtils>
#include <KIO/Global>
#include <KIO/Job>
#include <KIO/WorkerBase>
#include <KJob>

#include <QCoreApplication>
#include <QDateTime>
#include <QLocale>
#include <QUrl>

using namespace Qt::StringLiterals;

namespace {

QString displayPath(const QUrl &url)
{
    return url.isLocalFile() ? url.toLocalFile() : url.toDisplayString(QUrl::RemovePassword);
}

QString displayDir(const QUrl &url)
{
    return displayPath(url.adjusted(QUrl::RemoveFilename | QUrl::StripTrailingSlash));
}

QString timeText(const QDateTime &t)
{
    return t.isValid() ? QLocale().toString(t.toLocalTime(), QLocale::ShortFormat) : QString();
}

} // namespace

// ---- Interaction -------------------------------------------------------------

Interaction::Interaction(QObject *parent)
    : QObject(parent)
{
}

Interaction *Interaction::instance()
{
    static Interaction *self = new Interaction(qApp);
    return self;
}

Interaction *Interaction::create(QQmlEngine *, QJSEngine *)
{
    Interaction *self = instance();
    QQmlEngine::setObjectOwnership(self, QQmlEngine::CppOwnership);
    return self;
}

QVariantMap Interaction::current() const
{
    return m_queue.isEmpty() ? QVariantMap() : m_queue.first().map;
}

void Interaction::ask(QObject *owner, QVariantMap request, Callback callback)
{
    Request r;
    r.id = m_nextId++;
    r.owner = owner;
    request.insert(u"id"_s, r.id);
    r.map = request;
    r.callback = std::move(callback);
    m_queue.append(r);
    if (m_queue.size() == 1)
        Q_EMIT currentChanged();
}

void Interaction::forget(QObject *owner)
{
    if (m_queue.isEmpty())
        return;
    const int currentId = m_queue.first().id;
    m_queue.removeIf([owner](const Request &r) { return r.owner == owner || r.owner.isNull(); });
    if (m_queue.isEmpty() || m_queue.first().id != currentId)
        Q_EMIT currentChanged();
}

void Interaction::answer(int id, const QString &action, bool applyToAll, const QString &text)
{
    if (m_queue.isEmpty() || m_queue.first().id != id)
        return;
    const Request r = m_queue.takeFirst();
    // Show the next question (or close the dialog) before the job continues:
    // the callback may immediately queue a follow-up question.
    Q_EMIT currentChanged();
    if (r.owner && r.callback)
        r.callback(action, applyToAll, text);
}

void Interaction::reportError(const QString &title, const QString &message)
{
    Q_EMIT errorOccurred(title, message);
}

void Interaction::reportNotice(const QString &message, const QString &kind)
{
    Q_EMIT notice(message, kind);
}

// ---- AskUserHandler ----------------------------------------------------------

AskUserHandler::AskUserHandler(QObject *parent)
    : KIO::AskUserActionInterface(parent)
{
}

AskUserHandler::~AskUserHandler()
{
    Interaction::instance()->forget(this);
}

void AskUserHandler::askUserRename(KJob *job, const QString &title, const QUrl &src, const QUrl &dest,
                                   KIO::RenameDialog_Options options, KIO::filesize_t sizeSrc,
                                   KIO::filesize_t sizeDest, const QDateTime &ctimeSrc, const QDateTime &ctimeDest,
                                   const QDateTime &mtimeSrc, const QDateTime &mtimeDest)
{
    Q_UNUSED(ctimeSrc)
    Q_UNUSED(ctimeDest)
    const bool sameFile = options & KIO::RenameDialog_OverwriteItself;
    const QUrl destDir = dest.adjusted(QUrl::RemoveFilename);
    const QVariantMap request{
        {u"kind"_s, u"conflict"_s},
        {u"title"_s, title},
        {u"sourceName"_s, src.fileName()},
        {u"sourcePath"_s, displayPath(src)},
        {u"destName"_s, dest.fileName()},
        {u"destDir"_s, displayDir(dest)},
        {u"sourceSize"_s, sizeSrc == KIO::filesize_t(-1) ? -1.0 : double(sizeSrc)},
        {u"destSize"_s, sizeDest == KIO::filesize_t(-1) ? -1.0 : double(sizeDest)},
        {u"sourceTime"_s, timeText(mtimeSrc)},
        {u"destTime"_s, timeText(mtimeDest)},
        {u"sourceIsDir"_s, bool(options & KIO::RenameDialog_SourceIsDirectory)},
        {u"destIsDir"_s, bool(options & KIO::RenameDialog_DestIsDirectory)},
        {u"multiple"_s, bool(options & KIO::RenameDialog_MultipleItems)},
        {u"canOverwrite"_s, bool(options & KIO::RenameDialog_Overwrite) && !sameFile},
        {u"canSkip"_s, bool(options & KIO::RenameDialog_Skip)},
        {u"canRename"_s, !(options & KIO::RenameDialog_NoRename)},
        {u"sameFile"_s, sameFile},
        {u"suggestedName"_s, KFileUtils::suggestName(destDir, dest.fileName())},
    };
    const QPointer<KJob> guard(job);
    Interaction::instance()->ask(this, request, [this, guard, dest](const QString &action, bool all, const QString &text) {
        KIO::RenameDialog_Result result = KIO::Result_Cancel;
        QUrl newUrl;
        if (action == "overwrite"_L1) {
            result = all ? KIO::Result_OverwriteAll : KIO::Result_Overwrite;
        } else if (action == "skip"_L1) {
            result = all ? KIO::Result_AutoSkip : KIO::Result_Skip;
        } else if (action == "keepBoth"_L1) {
            if (all) {
                result = KIO::Result_AutoRename;
            } else {
                result = KIO::Result_Rename;
                newUrl = dest.adjusted(QUrl::RemoveFilename);
                newUrl.setPath(newUrl.path() + KFileUtils::suggestName(newUrl, dest.fileName()));
            }
        } else if (action == "rename"_L1 && !text.isEmpty() && !text.contains(u'/')) {
            result = KIO::Result_Rename;
            newUrl = dest.adjusted(QUrl::RemoveFilename);
            newUrl.setPath(newUrl.path() + text);
        }
        if (guard) {
            if (result == KIO::Result_Cancel)
                guard->setProperty(Interaction::cancelledProperty, true);
            Q_EMIT askUserRenameResult(result, newUrl, guard.data());
        }
    });
}

void AskUserHandler::askUserSkip(KJob *job, KIO::SkipDialog_Options options, const QString &errorText)
{
    const QVariantMap request{
        {u"kind"_s, u"skip"_s},
        {u"message"_s, errorText},
        {u"multiple"_s, bool(options & KIO::SkipDialog_MultipleItems)},
        {u"canRetry"_s, !(options & KIO::SkipDialog_Hide_Retry)},
    };
    const QPointer<KJob> guard(job);
    Interaction::instance()->ask(this, request, [this, guard](const QString &action, bool all, const QString &) {
        KIO::SkipDialog_Result result = KIO::Result_Cancel;
        if (action == "skip"_L1)
            result = all ? KIO::Result_AutoSkip : KIO::Result_Skip;
        else if (action == "retry"_L1)
            result = KIO::Result_Retry;
        if (guard) {
            if (result == KIO::Result_Cancel)
                guard->setProperty(Interaction::cancelledProperty, true);
            Q_EMIT askUserSkipResult(result, guard.data());
        }
    });
}

void AskUserHandler::askUserDelete(const QList<QUrl> &urls, DeletionType deletionType,
                                   ConfirmationType confirmationType, QWidget *parent)
{
    Q_UNUSED(confirmationType)
    QStringList names;
    for (const QUrl &u : urls) {
        if (names.size() == 6)
            break;
        names << (u.fileName().isEmpty() ? displayPath(u) : u.fileName());
    }
    const QVariantMap request{
        {u"kind"_s, u"delete"_s},
        {u"mode"_s, deletionType == Trash ? u"trash"_s : deletionType == EmptyTrash ? u"emptyTrash"_s : u"delete"_s},
        {u"count"_s, int(urls.size())},
        {u"names"_s, names},
    };
    Interaction::instance()->ask(this, request, [this, urls, deletionType, parent](const QString &action, bool, const QString &) {
        Q_EMIT askUserDeleteResult(action == "confirm"_L1, urls, deletionType, parent);
    });
}

void AskUserHandler::requestUserMessageBox(MessageDialogType type, const QString &text, const QString &title,
                                           const QString &primaryActionText, const QString &secondatyActionText,
                                           const QString &primaryActionIconName,
                                           const QString &secondatyActionIconName, const QString &dontAskAgainName,
                                           const QString &details, QWidget *parent)
{
    Q_UNUSED(primaryActionIconName)
    Q_UNUSED(secondatyActionIconName)
    Q_UNUSED(dontAskAgainName)
    Q_UNUSED(parent)
    const bool twoActions = type == QuestionTwoActions || type == QuestionTwoActionsCancel
        || type == WarningTwoActions || type == WarningTwoActionsCancel;
    const bool hasCancel = type == QuestionTwoActionsCancel || type == WarningTwoActionsCancel
        || type == WarningContinueCancel;
    const bool plain = type == Information || type == Error;
    const QVariantMap request{
        {u"kind"_s, u"message"_s},
        {u"tone"_s, type == Error ? u"danger"_s
                        : (type == QuestionTwoActions || type == QuestionTwoActionsCancel || type == Information)
                              ? u"info"_s : u"warning"_s},
        {u"title"_s, title},
        {u"text"_s, text},
        {u"details"_s, details},
        {u"primaryText"_s, plain ? tr("OK")
                               : !primaryActionText.isEmpty() ? primaryActionText
                               : type == WarningContinueCancel ? tr("Continue") : tr("Yes")},
        {u"secondaryText"_s, twoActions ? (secondatyActionText.isEmpty() ? tr("No") : secondatyActionText) : QString()},
        {u"hasCancel"_s, hasCancel},
    };
    Interaction::instance()->ask(this, request, [this, type, plain](const QString &action, bool, const QString &) {
        int code = KIO::WorkerBase::Cancel;
        if (plain)
            code = KIO::WorkerBase::Ok;
        else if (action == "primary"_L1)
            code = type == WarningContinueCancel ? KIO::WorkerBase::Continue : KIO::WorkerBase::PrimaryAction;
        else if (action == "secondary"_L1)
            code = KIO::WorkerBase::SecondaryAction;
        Q_EMIT messageBoxResult(code);
    });
}

void AskUserHandler::askIgnoreSslErrors(const QVariantMap &sslErrorData, QWidget *parent)
{
    Q_UNUSED(parent)
    // An untrusted certificate is never overridden from here: accepting it
    // would have to be remembered per host, which only the system trust store
    // does reliably. The connection is refused and the reason is shown.
    const QString host = sslErrorData.value(u"hostname"_s).toString();
    Interaction::instance()->reportError(
        tr("The connection to %1 is not trusted").arg(host.isEmpty() ? tr("the server") : host),
        tr("The server's TLS certificate could not be verified, so the connection was refused. "
           "Add the certificate authority to the system trust store to connect."));
    Q_EMIT askIgnoreSslErrorsResult(0);
}

// ---- JobUiDelegate -----------------------------------------------------------

JobUiDelegate::JobUiDelegate(KJobUiDelegate::Flags flags)
    : KJobUiDelegate(flags)
{
    // Found by KIO through KIO::delegateExtension<>(), which looks at the
    // direct children of the job's delegate.
    new AskUserHandler(this);
}

void JobUiDelegate::showErrorMessage()
{
    const KJob *j = job();
    if (!j || j->error() == KJob::NoError || j->error() == KJob::KilledJobError)
        return;
    Interaction::instance()->reportError(tr("The operation failed"), j->errorString());
}

KJobUiDelegate *JobUiDelegateFactory::createDelegate() const
{
    return new JobUiDelegate;
}

KJobUiDelegate *JobUiDelegateFactory::createDelegate(KJobUiDelegate::Flags flags, QWidget *window) const
{
    Q_UNUSED(window)
    return new JobUiDelegate(flags);
}

void JobUiDelegateFactory::install()
{
    static JobUiDelegateFactory factory;
    KIO::setDefaultJobUiDelegateFactory(&factory);
}
