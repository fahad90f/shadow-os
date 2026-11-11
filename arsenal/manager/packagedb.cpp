// SPDX-License-Identifier: GPL-3.0-or-later
#include "packagedb.h"

#include <command.h>

#include <QRegularExpression>
#include <QTimer>

namespace Arsenal {

namespace {

const QString kDpkgFormat = QStringLiteral(
    "${binary:Package}\\t${Version}\\t${db:Status-Abbrev}\\t${Installed-Size}\\t${Essential}\\t${Priority}\\n");

QString lastLines(const QString &text, int count)
{
    const QStringList lines = text.trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    return lines.mid(qMax(0, lines.size() - count)).join(QStringLiteral(" / "));
}

} // namespace

PackageDb::PackageDb(QObject *parent) : QObject(parent) {}

QHash<QString, PackageState> PackageDb::parseDpkgQuery(const QByteArray &output)
{
    QHash<QString, PackageState> states;
    const QList<QByteArray> lines = output.split('\n');
    for (const QByteArray &line : lines) {
        const QList<QByteArray> fields = line.split('\t');
        if (fields.size() < 6)
            continue;
        // "libfoo:amd64" for Multi-Arch: same packages.
        const QString name = QString::fromUtf8(fields.at(0)).section(QLatin1Char(':'), 0, 0);
        const QByteArray status = fields.at(2);
        // Status-Abbrev: desired, current, error — "ii " is properly installed.
        const bool installed = status.size() >= 2 && status.at(1) == 'i';
        PackageState &state = states[name];
        if (state.installed && !installed)
            continue;                   // another architecture of it is installed
        state.installed = installed;
        if (installed) {
            state.installedVersion = QString::fromUtf8(fields.at(1));
            state.installedBytes = fields.at(3).trimmed().toLongLong() * 1024;
        }
        state.essential = fields.at(4).trimmed() == "yes";
        state.priority = QString::fromUtf8(fields.at(5).trimmed());
    }
    return states;
}

void PackageDb::parseAptShow(const QByteArray &output, QHash<QString, PackageState> *states)
{
    const QStringList stanzas = QString::fromUtf8(output).split(QStringLiteral("\n\n"), Qt::SkipEmptyParts);
    for (const QString &stanza : stanzas) {
        QString name, version, homepage, description;
        qint64 size = 0, installedSize = 0;
        bool inDescription = false;
        const QStringList lines = stanza.split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            if (line.startsWith(QLatin1Char(' ')) || line.startsWith(QLatin1Char('\t'))) {
                if (inDescription) {
                    const QString text = line.mid(1);
                    description += (text == QLatin1String(".") ? QStringLiteral("\n\n")
                                    : (description.isEmpty() || description.endsWith(QLatin1Char('\n'))
                                           ? text.trimmed() : QLatin1Char(' ') + text.trimmed()));
                }
                continue;
            }
            inDescription = false;
            const int colon = line.indexOf(QLatin1Char(':'));
            if (colon < 0)
                continue;
            const QString key = line.left(colon);
            const QString value = line.mid(colon + 1).trimmed();
            if (key == QLatin1String("Package"))
                name = value;
            else if (key == QLatin1String("Version"))
                version = value;
            else if (key == QLatin1String("Size"))
                size = value.toLongLong();
            else if (key == QLatin1String("Installed-Size"))
                installedSize = value.toLongLong() * 1024;
            else if (key == QLatin1String("Homepage"))
                homepage = value;
            else if (key == QLatin1String("Description") || key == QLatin1String("Description-en")) {
                inDescription = true;       // the first line is the synopsis; the manifest has its own
                description.clear();
            }
        }
        if (name.isEmpty() || version.isEmpty())
            continue;
        PackageState &state = (*states)[name];
        if (state.available)
            continue;                       // first stanza is the candidate
        state.available = true;
        state.candidateVersion = version;
        state.downloadBytes = size;
        state.candidateInstalledBytes = installedSize;
        state.description = description.trimmed();
        state.homepage = homepage;
    }
}

QStringList PackageDb::parseSimulation(const QByteArray &output)
{
    static const QRegularExpression inst(QStringLiteral("^Inst (\\S+) (?:\\[[^\\]]*\\] )?\\((\\S+) "));
    QStringList planned;
    const QStringList lines = QString::fromUtf8(output).split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QRegularExpressionMatch match = inst.match(line);
        if (match.hasMatch())
            planned << match.captured(1) + QLatin1Char('=') + match.captured(2);
    }
    return planned;
}

void PackageDb::refresh(const QStringList &packages)
{
    const quint64 serial = ++m_serial;
    m_incoming.clear();
    m_pending = 2;

    if (packages.isEmpty()) {
        m_pending = 0;
        QTimer::singleShot(0, this, [this, serial] {
            if (serial != m_serial)
                return;
            m_states.clear();
            m_ready = true;
            Q_EMIT refreshed();
        });
        return;
    }

    // dpkg-query exits 1 when one of the names is unknown to dpkg, but still
    // prints the others — only a missing program or no output at all is an error.
    Shadow::Command::run(this, QStringLiteral("dpkg-query"),
                        QStringList{QStringLiteral("-W"), QStringLiteral("-f"), kDpkgFormat} + packages,
                        [this, serial](const Shadow::CommandResult &result) {
        if (serial != m_serial)
            return;
        if (result.exitCode == -1) {
            m_dpkgError = tr("dpkg-query could not be started, so it is unknown which tools are installed.");
        } else if (result.exitCode == -2) {
            m_dpkgError = tr("dpkg-query did not answer in time.");
        } else if (result.exitCode > 1) {
            m_dpkgError = tr("dpkg-query failed: %1").arg(lastLines(result.errText(), 2));
        } else {
            m_dpkgError.clear();
            const QHash<QString, PackageState> installed = parseDpkgQuery(result.out);
            for (auto it = installed.cbegin(); it != installed.cend(); ++it) {
                PackageState &state = m_incoming[it.key()];
                state.installed = it->installed;
                state.installedVersion = it->installedVersion;
                state.installedBytes = it->installedBytes;
                state.essential = it->essential;
                state.priority = it->priority;
            }
        }
        finishOne();
    }, 30000);

    Shadow::Command::run(this, QStringLiteral("apt-cache"),
                        QStringList{QStringLiteral("show"), QStringLiteral("--no-all-versions")} + packages,
                        [this, serial](const Shadow::CommandResult &result) {
        if (serial != m_serial)
            return;
        if (result.exitCode == -1) {
            m_aptError = tr("apt-cache could not be started, so versions and sizes from the repositories are unknown.");
        } else if (result.exitCode == -2) {
            m_aptError = tr("apt-cache did not answer in time.");
        } else {
            // Exit status 100 only says that some names are unknown; those stay "not available".
            m_aptError.clear();
            parseAptShow(result.out, &m_incoming);
        }
        finishOne();
    }, 60000);
}

void PackageDb::finishOne()
{
    if (--m_pending > 0)
        return;
    m_states = m_incoming;
    m_incoming.clear();
    m_ready = true;
    Q_EMIT refreshed();
}

void PackageDb::clearEstimates()
{
    ++m_estimateSerial;
    m_estimates.clear();
    m_queue.clear();
    m_estimating = false;
}

void PackageDb::estimate(const QString &key, const QStringList &packages)
{
    SizeEstimate pending;
    pending.state = QStringLiteral("pending");
    m_estimates.insert(key, pending);
    m_queue.append({key, packages});
    if (!m_estimating)
        runNextEstimate();
}

void PackageDb::finishEstimate(const QString &key, const SizeEstimate &estimate)
{
    m_estimates.insert(key, estimate);
    m_estimating = false;
    Q_EMIT estimated(key);
    runNextEstimate();
}

void PackageDb::runNextEstimate()
{
    if (m_estimating || m_queue.isEmpty())
        return;
    m_estimating = true;
    const auto [key, packages] = m_queue.takeFirst();
    const quint64 serial = m_estimateSerial;

    if (packages.isEmpty()) {
        QTimer::singleShot(0, this, [this, key, serial] {
            if (serial != m_estimateSerial)
                return;
            SizeEstimate done;
            done.state = QStringLiteral("ready");
            finishEstimate(key, done);
        });
        return;
    }

    const QStringList arguments = QStringList{QStringLiteral("install"), QStringLiteral("--simulate"),
                                              QStringLiteral("-qq")} + packages;
    Shadow::Command::run(this, QStringLiteral("apt-get"), arguments,
                        [this, key, serial](const Shadow::CommandResult &simulation) {
        if (serial != m_estimateSerial)
            return;
        SizeEstimate estimate;
        if (!simulation.ok()) {
            estimate.state = QStringLiteral("error");
            estimate.error = simulation.exitCode == -1 ? tr("apt-get could not be started")
                             : simulation.exitCode == -2 ? tr("apt-get did not answer in time")
                             : lastLines(simulation.errText().isEmpty() ? simulation.outText()
                                                                        : simulation.errText(), 3);
            finishEstimate(key, estimate);
            return;
        }
        const QStringList planned = parseSimulation(simulation.out);
        estimate.packages = planned.size();
        if (planned.isEmpty()) {
            estimate.state = QStringLiteral("ready");
            finishEstimate(key, estimate);
            return;
        }
        Shadow::Command::run(this, QStringLiteral("apt-cache"), QStringList{QStringLiteral("show")} + planned,
                            [this, key, serial, estimate](const Shadow::CommandResult &shown) mutable {
            if (serial != m_estimateSerial)
                return;
            if (shown.exitCode < 0) {
                estimate.state = QStringLiteral("error");
                estimate.error = tr("apt-cache did not answer");
            } else {
                const QList<QByteArray> lines = shown.out.split('\n');
                for (const QByteArray &line : lines) {
                    if (line.startsWith("Size: "))
                        estimate.downloadBytes += line.mid(6).trimmed().toLongLong();
                    else if (line.startsWith("Installed-Size: "))
                        estimate.installedBytes += line.mid(16).trimmed().toLongLong() * 1024;
                }
                estimate.state = QStringLiteral("ready");
            }
            finishEstimate(key, estimate);
        }, 120000);
    }, 180000);
}

} // namespace Arsenal
