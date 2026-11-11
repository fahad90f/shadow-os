// SPDX-License-Identifier: GPL-3.0-or-later
#include "validation.h"

#include "secret.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QSet>

#include <grp.h>
#include <pwd.h>
#include <unistd.h>
#include <vector>

namespace Validation {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("Validation", text);
}

// answers.py: RESERVED_USERS
const QSet<QString> &reservedUsers()
{
    static const QSet<QString> names = {
        QStringLiteral("root"), QStringLiteral("daemon"), QStringLiteral("bin"), QStringLiteral("sys"),
        QStringLiteral("sync"), QStringLiteral("games"), QStringLiteral("man"), QStringLiteral("lp"),
        QStringLiteral("mail"), QStringLiteral("news"), QStringLiteral("uucp"), QStringLiteral("proxy"),
        QStringLiteral("www-data"), QStringLiteral("backup"), QStringLiteral("list"), QStringLiteral("irc"),
        QStringLiteral("nobody"), QStringLiteral("systemd-network"), QStringLiteral("systemd-timesync"),
        QStringLiteral("messagebus"), QStringLiteral("sddm"), QStringLiteral("polkitd"), QStringLiteral("avahi"),
        QStringLiteral("nexus-live"), QStringLiteral("admin"), QStringLiteral("adm"), QStringLiteral("sudo"),
        QStringLiteral("staff"), QStringLiteral("users"),
    };
    return names;
}

bool hasLineBreak(const QString &text)
{
    return text.contains(QLatin1Char('\n')) || text.contains(QLatin1Char('\r'));
}

} // namespace

QString userNameProblem(const QString &name)
{
    // answers.py: USERNAME_RE = ^[a-z_][a-z0-9_-]{0,31}$
    if (name.isEmpty())
        return tr("Enter a user name.");
    static const QRegularExpression valid(QStringLiteral("\\A[a-z_][a-z0-9_-]{0,31}\\z"));
    if (!valid.match(name).hasMatch()) {
        static const QRegularExpression upper(QStringLiteral("[A-Z]"));
        static const QRegularExpression allowedChars(QStringLiteral("\\A[a-z0-9_-]+\\z"));
        static const QRegularExpression firstChar(QStringLiteral("\\A[a-z_]"));
        if (upper.match(name).hasMatch())
            return tr("User names are written in lower-case letters.");
        if (!allowedChars.match(name).hasMatch())
            return tr("Only the letters a–z, digits, “-” and “_” can be used.");
        if (!firstChar.match(name).hasMatch())
            return tr("The user name must start with a letter.");
        return tr("The user name can have at most 32 characters.");
    }
    if (reservedUsers().contains(name))
        return tr("“%1” is reserved for the system.").arg(name);
    return existingAccountProblem(name);
}

QString existingAccountProblem(const QString &name)
{
    const QByteArray local = name.toLocal8Bit();
    long size = sysconf(_SC_GETPW_R_SIZE_MAX);
    std::vector<char> buffer(size_t(size > 0 ? size : 16384));

    struct passwd pw;
    struct passwd *pwResult = nullptr;
    if (getpwnam_r(local.constData(), &pw, buffer.data(), buffer.size(), &pwResult) == 0 && pwResult
        && (pwResult->pw_uid < 1000 || pwResult->pw_uid == 65534)) {
        return tr("“%1” is a system account.").arg(name);
    }
    struct group gr;
    struct group *grResult = nullptr;
    if (getgrnam_r(local.constData(), &gr, buffer.data(), buffer.size(), &grResult) == 0 && grResult
        && (grResult->gr_gid < 1000 || grResult->gr_gid == 65534)) {
        return tr("“%1” is the name of a system group, so no personal group of that name can be created.").arg(name);
    }
    return QString();
}

QString fullNameProblem(const QString &name)
{
    // answers.py: no ':', ',', '=' or line breaks; at most 120 characters
    static const QRegularExpression forbidden(QStringLiteral("[:,=\\n\\r]"));
    if (forbidden.match(name).hasMatch())
        return tr("The name cannot contain “:”, “,” or “=”.");
    if (Secret::codePoints(name) > 120)
        return tr("The name can have at most 120 characters.");
    return QString();
}

QString hostNameProblem(const QString &name)
{
    // answers.py: HOSTNAME_RE = ^(?=.{1,63}$)[a-z0-9]([a-z0-9-]*[a-z0-9])?$
    if (name.isEmpty())
        return tr("Enter a name for this computer.");
    static const QRegularExpression valid(QStringLiteral("\\A(?=.{1,63}\\z)[a-z0-9]([a-z0-9-]*[a-z0-9])?\\z"));
    if (valid.match(name).hasMatch())
        return QString();
    static const QRegularExpression upper(QStringLiteral("[A-Z]"));
    static const QRegularExpression allowedChars(QStringLiteral("\\A[a-z0-9-]+\\z"));
    if (name.size() > 63)
        return tr("The computer name can have at most 63 characters.");
    if (upper.match(name).hasMatch())
        return tr("Computer names are written in lower-case letters.");
    if (!allowedChars.match(name).hasMatch())
        return tr("Only the letters a–z, digits and “-” can be used.");
    return tr("The computer name cannot start or end with “-”.");
}

QString passwordProblem(const QString &password)
{
    if (password.isEmpty())
        return tr("Enter a password.");
    if (hasLineBreak(password))
        return tr("The password cannot contain line breaks.");
    return QString();
}

QString passphraseProblem(const QString &passphrase)
{
    if (passphrase.isEmpty())
        return tr("Enter a passphrase.");
    if (hasLineBreak(passphrase))
        return tr("The passphrase cannot contain line breaks.");
    if (Secret::codePoints(passphrase) < 8)
        return tr("The passphrase needs at least 8 characters.");
    return QString();
}

QVariantMap passwordFacts(const QString &password, const QStringList &personalWords)
{
    bool lower = false, upper = false, digits = false, symbols = false;
    bool allSame = !password.isEmpty();
    bool ascending = password.size() >= 4, descending = password.size() >= 4;
    for (qsizetype i = 0; i < password.size(); ++i) {
        const QChar c = password.at(i);
        if (c.isDigit())
            digits = true;
        else if (c.isUpper())
            upper = true;
        else if (c.isLetter() || c.isMark() || c.isSurrogate())
            lower = true;       // lower-case letters and letters of scripts without case
        else
            symbols = true;     // punctuation, spaces and everything else
        if (i > 0) {
            const int step = int(c.unicode()) - int(password.at(i - 1).unicode());
            allSame = allSame && step == 0;
            ascending = ascending && step == 1;
            descending = descending && step == -1;
        }
    }
    const int length = Secret::codePoints(password);
    const int classes = int(lower) + int(upper) + int(digits) + int(symbols);

    QStringList remarks;
    if (length > 0) {
        if (length < 8)
            remarks << tr("It has only %1 characters. Use at least 8; 12 or more, or several unrelated words, "
                          "are much harder to guess.").arg(length);
        if (allSame && length > 1)
            remarks << tr("It is a single character repeated.");
        else if (ascending || descending)
            remarks << tr("It is a run of neighbouring characters such as “1234” or “abcd”.");
        else if (classes == 1 && digits)
            remarks << tr("It consists of digits only.");
        else if (classes == 1 && length < 16)
            remarks << tr("It uses one kind of character only. Mixing letters, digits and symbols — or "
                          "making it longer — makes guessing harder.");
        const QString folded = password.toCaseFolded();
        for (const QString &word : personalWords) {
            const QString w = word.trimmed().toCaseFolded();
            if (w.size() >= 3 && folded.contains(w)) {
                remarks << tr("It contains “%1”, which others know or can guess.").arg(word.trimmed());
                break;
            }
        }
    }
    return {
        {QStringLiteral("length"), length},
        {QStringLiteral("lower"), lower},
        {QStringLiteral("upper"), upper},
        {QStringLiteral("digits"), digits},
        {QStringLiteral("symbols"), symbols},
        {QStringLiteral("remarks"), remarks},
    };
}

QString suggestUserName(const QString &fullName)
{
    const QString first = fullName.trimmed().section(QRegularExpression(QStringLiteral("\\s+")), 0, 0);
    // Decompose accented letters (é → e + ´) and keep the ASCII part.
    const QString decomposed = first.normalized(QString::NormalizationForm_KD);
    QString out;
    for (const QChar c : decomposed) {
        const char16_t u = c.toLower().unicode();
        if ((u >= u'a' && u <= u'z') || (u >= u'0' && u <= u'9' && !out.isEmpty()))
            out.append(QChar(u));
        if (out.size() == 32)
            break;
    }
    if (out.isEmpty() || !userNameProblem(out).isEmpty())
        return QString();
    return out;
}

} // namespace Validation
