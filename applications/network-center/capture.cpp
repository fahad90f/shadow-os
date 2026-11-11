// SPDX-License-Identifier: GPL-3.0-or-later
#include "capture.h"

#include "netutil.h"

#include <command.h>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QTemporaryFile>
#include <QUrl>

#include <csignal>
#include <cstring>
#include <grp.h>
#include <memory>
#include <pwd.h>
#include <sys/prctl.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <vector>

namespace {

const qint64 kMinFreeBytes = 32LL * 1024 * 1024;

// The same rule the privileged helper enforces (network-helper, FILTER_RE):
// only the characters of the pcap filter language, never a leading '-'.
bool isCaptureFilter(const QString &s)
{
    static const QRegularExpression rx(QStringLiteral("^[A-Za-z0-9 .:/()\\[\\]&|!=<>*+%^,_\\\\-]{0,1024}$"));
    return rx.match(s).hasMatch() && !s.startsWith(QLatin1Char('-')) && !s.contains(QLatin1Char('\n'));
}

QString captureFilterRule()
{
    return QObject::tr("A capture filter may only contain the characters of the pcap filter language (letters, digits, "
                       "spaces and . : / ( ) [ ] & | ! = < > * + % ^ , _ \\ -), at most 1024 of them, and must not begin with “-”.");
}

bool interfaceExists(const QString &name)
{
    return Net::isInterfaceName(name)
        && (name == QLatin1String("any") || QFileInfo::exists(QStringLiteral("/sys/class/net/") + name));
}

// LINKTYPE_ value libpcap uses for an interface, from its ARPHRD_ type.
quint32 linkTypeFor(const QString &interface)
{
    if (interface == QLatin1String("any"))
        return 113;                         // LINKTYPE_LINUX_SLL ("cooked" capture)
    QFile f(QStringLiteral("/sys/class/net/%1/type").arg(interface));
    const int arphrd = f.open(QIODevice::ReadOnly) ? f.readAll().trimmed().toInt() : 1;
    switch (arphrd) {
    case 1:                                 // ARPHRD_ETHER
    case 772:                               // ARPHRD_LOOPBACK
        return 1;                           // LINKTYPE_ETHERNET
    case 65534:                             // ARPHRD_NONE: tun, WireGuard
        return 101;                         // LINKTYPE_RAW
    case 801: return 105;                   // IEEE 802.11
    case 803: return 127;                   // IEEE 802.11 + radiotap
    default: return 113;
    }
}

// True if the file carries the CAP_NET_RAW file capability (dumpcap needs it
// to capture without being root). Layout: struct vfs_cap_data, little-endian.
bool hasNetRawCapability(const QString &path)
{
    unsigned char data[24] = {};
    const ssize_t size = ::getxattr(QFile::encodeName(path).constData(), "security.capability", data, sizeof data);
    if (size < 12)
        return false;
    const quint32 permitted = quint32(data[4]) | quint32(data[5]) << 8 | quint32(data[6]) << 16 | quint32(data[7]) << 24;
    return permitted & (1u << 13);          // CAP_NET_RAW
}

// The first lines of a tool's stderr that say something about the failure.
QString usefulError(const QByteArray &stderrText, int maxLines = 2)
{
    QStringList lines;
    const QStringList raw = Net::cleanMessage(QString::fromUtf8(stderrText)).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (QString line : raw) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('{')) || line.startsWith(QLatin1String("reading from file"))
            || line.startsWith(QLatin1String("Warning:")) || line.contains(QLatin1String("listening on"))
            || line.startsWith(QLatin1String("Capturing on")))
            continue;
        for (const char *prefix : {"tcpdump: ", "dumpcap: "}) {
            if (line.startsWith(QLatin1String(prefix)))
                line = line.mid(int(std::strlen(prefix)));
        }
        lines << line;
        if (lines.size() >= maxLines)
            break;
    }
    return lines.join(QLatin1Char(' '));
}

QString helperError(const QByteArray &stderrText)
{
    const QList<QByteArray> lines = stderrText.split('\n');
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        if (!it->trimmed().startsWith('{'))
            continue;
        const QJsonObject o = QJsonDocument::fromJson(it->trimmed()).object();
        if (o.contains(QLatin1String("error")))
            return o.value(QLatin1String("error")).toString();
    }
    return {};
}

} // namespace

Capture::Capture(QObject *parent)
    : QObject(parent)
    , m_directory(QDir::homePath() + QStringLiteral("/Captures"))
{
    m_tick.setInterval(1000);
    connect(&m_tick, &QTimer::timeout, this, &Capture::onTick);
    m_statsTimer.setInterval(250);
    connect(&m_statsTimer, &QTimer::timeout, this, [this] {
        if (m_statsDirty) {
            m_statsDirty = false;
            Q_EMIT statsChanged();
        }
    });
    m_killTimer.setSingleShot(true);
    m_killTimer.setInterval(8000);
    connect(&m_killTimer, &QTimer::timeout, this, [this] {
        if (m_process)
            m_process->kill();
    });
    refreshFiles();
}

Capture::~Capture()
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->closeWriteChannel();
        m_process->terminate();
        if (!m_process->waitForFinished(1500))
            m_process->kill();
    }
    if (m_file.isOpen())
        m_file.close();
}

// ---- capability ------------------------------------------------------------------

void Capture::refreshCapability()
{
    const int serial = ++m_capabilitySerial;
    m_checking = true;
    Q_EMIT capabilityChanged();
    m_wireshark = !Net::findTool(QStringLiteral("wireshark")).isEmpty();

    QVariantMap dumpcap;
    const QString path = Net::findTool(QStringLiteral("dumpcap"));
    dumpcap.insert(QStringLiteral("path"), path);
    dumpcap.insert(QStringLiteral("installed"), !path.isEmpty());
    auto finishLater = [this, serial](const QVariantMap &d, bool usable) {
        QMetaObject::invokeMethod(this, [this, serial, d, usable] {
            if (serial == m_capabilitySerial)
                finishCapability(d, usable);
        }, Qt::QueuedConnection);
    };
    if (path.isEmpty()) {
        dumpcap.insert(QStringLiteral("problem"), tr("dumpcap is not installed (package wireshark-common)."));
        finishLater(dumpcap, false);
        return;
    }

    // Debian installs dumpcap as root:wireshark, mode 0750, with file
    // capabilities — if the administrator allowed non-root captures at all.
    const bool executable = ::access(QFile::encodeName(path).constData(), X_OK) == 0;
    bool groupExists = false, inGroup = false, listed = false;
    if (const struct group *gr = ::getgrnam("wireshark")) {
        groupExists = true;
        inGroup = ::getegid() == gr->gr_gid;
        const int count = ::getgroups(0, nullptr);
        if (count > 0) {
            std::vector<gid_t> groups(static_cast<size_t>(count));
            const int n = ::getgroups(count, groups.data());
            for (int i = 0; i < n; ++i)
                inGroup = inGroup || groups[static_cast<size_t>(i)] == gr->gr_gid;
        }
        if (const struct passwd *pw = ::getpwuid(::getuid())) {
            for (char **member = gr->gr_mem; member && *member; ++member)
                listed = listed || std::strcmp(*member, pw->pw_name) == 0;
        }
    }
    const bool capabilities = hasNetRawCapability(path);
    const bool root = ::geteuid() == 0;
    dumpcap.insert(QStringLiteral("executable"), executable);
    dumpcap.insert(QStringLiteral("wiresharkGroupExists"), groupExists);
    dumpcap.insert(QStringLiteral("inWiresharkGroup"), inGroup);
    dumpcap.insert(QStringLiteral("listedInWiresharkGroup"), listed);
    dumpcap.insert(QStringLiteral("fileCapabilities"), capabilities);

    if (!executable) {
        QString problem;
        if (listed && !inGroup)
            problem = tr("You were added to the “wireshark” group, but this session started before that. Log out and "
                         "back in to capture with dumpcap.");
        else if (groupExists)
            problem = tr("dumpcap may only be run by members of the “wireshark” group, and your account is not one of them "
                         "(an administrator can add it with “usermod -aG wireshark <user>”; log in again afterwards).");
        else
            problem = tr("dumpcap (%1) is not executable for your account.").arg(path);
        dumpcap.insert(QStringLiteral("problem"), problem);
        finishLater(dumpcap, false);
        return;
    }

    // The decisive test: can dumpcap open a device? Listing the link-layer
    // types of the loopback interface requires exactly that and captures nothing.
    Shadow::Command::run(this, path, {QStringLiteral("-L"), QStringLiteral("-i"), QStringLiteral("lo")},
                        [this, serial, dumpcap, capabilities, root](const Shadow::CommandResult &r) mutable {
        if (serial != m_capabilitySerial)
            return;
        dumpcap.insert(QStringLiteral("probeExitCode"), r.exitCode);
        if (r.ok()) {
            finishCapability(dumpcap, true);
            return;
        }
        const QString detail = usefulError(r.err);
        QString problem;
        if (!capabilities && !root)
            problem = tr("dumpcap is installed without capture privileges, so it cannot open a network interface as your "
                         "user. An administrator can allow it with “dpkg-reconfigure wireshark-common”.");
        else
            problem = tr("dumpcap could not open a network interface.");
        if (!detail.isEmpty())
            problem += QLatin1Char(' ') + tr("Its message: %1").arg(detail);
        dumpcap.insert(QStringLiteral("problem"), problem);
        finishCapability(dumpcap, false);
    }, 8000);
}

void Capture::finishCapability(QVariantMap dumpcap, bool dumpcapUsable)
{
    const bool root = ::geteuid() == 0;
    const QString tcpdump = Net::findTool(QStringLiteral("tcpdump"));
    const QString helper = Net::helperPath();
    const QString pkexec = Net::findTool(QStringLiteral("pkexec"));
    dumpcap.insert(QStringLiteral("usable"), dumpcapUsable);

    QStringList helperProblems;
    if (tcpdump.isEmpty())
        helperProblems << tr("tcpdump is not installed (package tcpdump).");
    if (helper.isEmpty())
        helperProblems << tr("The Network Center helper (%1) is not installed.").arg(QStringLiteral(SHADOW_NETWORK_HELPER));
    if (!root && pkexec.isEmpty())
        helperProblems << tr("pkexec is not installed (package pkexec), so the helper cannot be started with administrator rights.");
    const bool helperUsable = helperProblems.isEmpty();

    QVariantMap tcpdumpInfo;
    tcpdumpInfo.insert(QStringLiteral("path"), tcpdump);
    tcpdumpInfo.insert(QStringLiteral("installed"), !tcpdump.isEmpty());
    tcpdumpInfo.insert(QStringLiteral("helper"), helper);
    tcpdumpInfo.insert(QStringLiteral("helperInstalled"), !helper.isEmpty());
    tcpdumpInfo.insert(QStringLiteral("pkexec"), pkexec);
    tcpdumpInfo.insert(QStringLiteral("usable"), helperUsable);
    tcpdumpInfo.insert(QStringLiteral("problems"), helperProblems);

    QVariantMap cap;
    const QString backend = dumpcapUsable ? QStringLiteral("dumpcap") : helperUsable ? QStringLiteral("tcpdump") : QString();
    cap.insert(QStringLiteral("backend"), backend);
    cap.insert(QStringLiteral("usable"), !backend.isEmpty());
    cap.insert(QStringLiteral("format"), backend == QLatin1String("dumpcap") ? QStringLiteral("pcapng")
                                         : backend == QLatin1String("tcpdump") ? QStringLiteral("pcap") : QString());
    cap.insert(QStringLiteral("needsAuthentication"), backend == QLatin1String("tcpdump") && !root);
    cap.insert(QStringLiteral("runningAsRoot"), root);
    cap.insert(QStringLiteral("dumpcap"), dumpcap);
    cap.insert(QStringLiteral("tcpdump"), tcpdumpInfo);
    cap.insert(QStringLiteral("directory"), m_directory);
    cap.insert(QStringLiteral("wiresharkInstalled"), m_wireshark);

    QString summary;
    QStringList problems;
    const QString dumpcapProblem = dumpcap.value(QStringLiteral("problem")).toString();
    if (backend == QLatin1String("dumpcap")) {
        summary = tr("Captures run without administrator rights through dumpcap and are saved in pcapng format.");
    } else if (backend == QLatin1String("tcpdump")) {
        summary = root ? tr("Captures run through tcpdump and are saved in classic pcap format.")
                       : tr("Captures run through tcpdump with administrator rights (you are asked to authenticate each "
                            "time) and are saved in classic pcap format.");
        problems << dumpcapProblem;
    } else {
        summary = tr("Packet capture is not possible on this system.");
        problems << dumpcapProblem << helperProblems;
    }
    cap.insert(QStringLiteral("summary"), summary);
    cap.insert(QStringLiteral("problems"), problems);

    m_checking = false;
    m_capability = cap;
    Q_EMIT capabilityChanged();
    Q_EMIT capabilityReady();
}

// ---- filter validation ---------------------------------------------------------------

void Capture::setFilter(const QString &state, const QString &message)
{
    if (state == m_filterState && message == m_filterMessage)
        return;
    m_filterState = state;
    m_filterMessage = message;
    Q_EMIT filterChanged();
}

void Capture::clearFilterState()
{
    ++m_filterSerial;
    setFilter(QString(), QString());
}

void Capture::compileFilter(const QString &interface, const QString &expression,
                            std::function<void(const QString &, const QString &)> done)
{
    if (m_capability.value(QStringLiteral("backend")) == QLatin1String("dumpcap")) {
        // -d: print the compiled filter program and exit without capturing.
        const QString dumpcap = m_capability.value(QStringLiteral("dumpcap")).toMap().value(QStringLiteral("path")).toString();
        Shadow::Command::run(this, dumpcap, {QStringLiteral("-i"), interface, QStringLiteral("-f"), expression, QStringLiteral("-d")},
                            [done](const Shadow::CommandResult &r) {
            if (r.ok())
                done(QStringLiteral("valid"), QString());
            else if (r.exitCode < 0)
                done(QStringLiteral("unavailable"), tr("dumpcap could not be run to check the filter."));
            else
                done(QStringLiteral("invalid"), usefulError(r.err));
        }, 8000);
        return;
    }
    const QString tcpdump = Net::findTool(QStringLiteral("tcpdump"));
    if (tcpdump.isEmpty()) {
        done(QStringLiteral("unavailable"), tr("Neither dumpcap nor tcpdump is installed, so the filter cannot be checked."));
        return;
    }
    // tcpdump compiles a filter for the link type of its input. Reading an
    // empty capture file of the interface's link type needs no privilege,
    // unlike opening the interface itself.
    auto file = std::make_shared<QTemporaryFile>(QDir::tempPath() + QStringLiteral("/shadow-netcenter-XXXXXX.pcap"));
    if (!file->open() || file->write(CaptureStream::pcapFileHeader(linkTypeFor(interface))) != 24 || !file->flush()) {
        done(QStringLiteral("unavailable"), tr("A temporary file for checking the filter could not be written."));
        return;
    }
    Shadow::Command::run(this, tcpdump, {QStringLiteral("-d"), QStringLiteral("-r"), file->fileName(), QStringLiteral("--"), expression},
                        [file, done](const Shadow::CommandResult &r) {
        if (r.ok())
            done(QStringLiteral("valid"), QString());
        else if (r.exitCode < 0)
            done(QStringLiteral("unavailable"), tr("tcpdump could not be run to check the filter."));
        else
            done(QStringLiteral("invalid"), usefulError(r.err));
    }, 8000);
}

void Capture::validateFilter(const QString &interface, const QString &expression)
{
    const int serial = ++m_filterSerial;
    const QString filter = expression.trimmed();
    if (filter.isEmpty()) {
        setFilter(QString(), QString());
        return;
    }
    if (!isCaptureFilter(filter)) {
        setFilter(QStringLiteral("invalid"), captureFilterRule());
        return;
    }
    if (!interfaceExists(interface)) {
        setFilter(QStringLiteral("invalid"), tr("Choose an interface first."));
        return;
    }
    setFilter(QStringLiteral("checking"), QString());
    compileFilter(interface, filter, [this, serial](const QString &state, const QString &message) {
        if (serial != m_filterSerial)
            return;
        setFilter(state, state == QLatin1String("valid") ? tr("The filter compiles.") : message);
    });
}

// ---- capture ---------------------------------------------------------------------------

void Capture::setMessage(const QString &kind, const QString &text)
{
    m_messageKind = kind;
    m_message = text;
    Q_EMIT messageChanged();
}

void Capture::setState(const QString &state)
{
    if (state == m_state)
        return;
    m_state = state;
    Q_EMIT stateChanged();
}

qint64 Capture::bytesOnDisk() const
{
    qint64 total = 0;
    for (const qint64 size : m_fileSizes)
        total += size;
    return total;
}

int Capture::elapsedSeconds() const
{
    if (busy() && m_clock.isValid())
        return int(m_clock.elapsed() / 1000);
    return int(m_finalElapsedMs / 1000);
}

void Capture::start(const QVariantMap &options)
{
    if (busy())
        return;
    if (!m_capability.value(QStringLiteral("usable")).toBool()) {
        setMessage(QStringLiteral("danger"), m_capability.value(QStringLiteral("summary"), tr("Capture capability is not known yet.")).toString());
        return;
    }
    const QString interface = options.value(QStringLiteral("interface")).toString();
    if (!interfaceExists(interface)) {
        setMessage(QStringLiteral("danger"), tr("“%1” is not a network interface of this computer.").arg(interface));
        return;
    }
    const QString filter = options.value(QStringLiteral("filter")).toString().trimmed();
    if (!isCaptureFilter(filter)) {
        setMessage(QStringLiteral("danger"), captureFilterRule());
        return;
    }
    const int megabytes = qBound(0, options.value(QStringLiteral("maxFileMegabytes")).toInt(), 100000);
    const int seconds = qBound(0, options.value(QStringLiteral("maxSeconds")).toInt(), 7 * 24 * 3600);
    const bool ring = options.value(QStringLiteral("ring")).toBool();
    const int ringFiles = qBound(2, options.value(QStringLiteral("ringFiles"), 2).toInt(), 1000);
    if (ring && megabytes == 0 && seconds == 0) {
        setMessage(QStringLiteral("danger"), tr("A ring buffer needs a size limit or a time limit per file."));
        return;
    }

    QDir dir(m_directory);
    if (!dir.exists()) {
        if (!QDir().mkpath(m_directory)) {
            setMessage(QStringLiteral("danger"), tr("The folder %1 could not be created.").arg(m_directory));
            return;
        }
        // Captures contain other people's traffic: keep the folder private.
        QFile::setPermissions(m_directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }
    if (QStorageInfo(m_directory).bytesAvailable() < kMinFreeBytes) {
        setMessage(QStringLiteral("danger"), tr("There is less than 32 MiB of free space in %1.").arg(m_directory));
        return;
    }

    m_backend = m_capability.value(QStringLiteral("backend")).toString();
    m_interface = interface;
    m_filter = filter;
    m_format.clear();
    m_maxFileBytes = qint64(megabytes) * 1000 * 1000;
    m_maxSeconds = seconds;
    m_ring = ring;
    m_ringFiles = ringFiles;
    m_baseName = QStringLiteral("capture-%1-%2").arg(interface, QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    m_stderr.clear();
    m_stream.reset();
    m_preamble.clear();
    m_filePaths.clear();
    m_fileNames.clear();
    m_fileSizes.clear();
    m_fileIndex = 0;
    m_filePackets = 0;
    m_packets = 0;
    m_bytesCaptured = 0;
    m_finalElapsedMs = 0;
    m_clock.invalidate();
    m_rotateDue = false;
    m_discard = false;
    m_stopReason.clear();
    m_failure.clear();
    setMessage(QString(), QString());
    setState(QStringLiteral("starting"));
    Q_EMIT statsChanged();

    if (filter.isEmpty()) {
        launch();
        return;
    }
    // Refuse to start (and to ask for authentication) with a filter that does not compile.
    compileFilter(interface, filter, [this](const QString &state, const QString &message) {
        if (m_state != QLatin1String("starting"))
            return;
        if (state == QLatin1String("invalid")) {
            setFilter(state, message);
            setMessage(QStringLiteral("danger"), tr("The capture filter is not valid: %1").arg(message));
            setState(QStringLiteral("idle"));
            return;
        }
        launch();
    });
}

void Capture::launch()
{
    QString program;
    QStringList arguments;
    if (m_backend == QLatin1String("dumpcap")) {
        program = m_capability.value(QStringLiteral("dumpcap")).toMap().value(QStringLiteral("path")).toString();
        arguments << QStringLiteral("-q") << QStringLiteral("-i") << m_interface;
        if (!m_filter.isEmpty())
            arguments << QStringLiteral("-f") << m_filter;
        arguments << QStringLiteral("-w") << QStringLiteral("-");
    } else {
        QStringList helperArguments = {QStringLiteral("capture"), QStringLiteral("--interface"), m_interface};
        if (!m_filter.isEmpty())
            helperArguments << QStringLiteral("--filter") << m_filter;
        QString problem;
        if (!Net::elevatedCommand(Net::helperPath(), helperArguments, &program, &arguments, &problem)) {
            setMessage(QStringLiteral("danger"), problem);
            setState(QStringLiteral("idle"));
            return;
        }
    }

    auto *process = new QProcess(this);
    m_process = process;
    process->setProcessEnvironment(Shadow::Command::parseEnvironment());
    process->setProgram(program);
    process->setArguments(arguments);
    process->setChildProcessModifier([] { ::prctl(PR_SET_PDEATHSIG, SIGTERM); });
    // Standard input stays an open pipe: closing it tells the helper to stop.
    connect(process, &QProcess::readyReadStandardOutput, this, &Capture::onStreamData);
    connect(process, &QProcess::readyReadStandardError, this, [this, process] {
        m_stderr += process->readAllStandardError();
        if (m_stderr.size() > 64 * 1024)
            m_stderr = m_stderr.right(32 * 1024);
    });
    connect(process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        onProcessFinished(status == QProcess::NormalExit ? code : -1, false);
    });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            onProcessFinished(-1, true);
    });
    process->start();
}

bool Capture::openNextFile()
{
    ++m_fileIndex;
    const QString name = m_ring ? QStringLiteral("%1-%2.%3").arg(m_baseName).arg(m_fileIndex, 5, 10, QLatin1Char('0')).arg(m_format)
                                : QStringLiteral("%1.%2").arg(m_baseName, m_format);
    const QString path = m_directory + QLatin1Char('/') + name;
    m_file.setFileName(path);
    // NewOnly: never follow or overwrite something that is already there.
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        abortWith(tr("The capture file %1 could not be created: %2").arg(path, m_file.errorString()));
        return false;
    }
    m_file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    m_filePaths << path;
    m_fileNames << name;
    m_fileSizes << 0;
    m_filePackets = 0;
    m_fileClock.restart();
    m_rotateDue = false;
    if (!writeUnit(m_preamble.constData(), m_preamble.size()))
        return false;
    while (m_ring && m_filePaths.size() > m_ringFiles) {
        QFile::remove(m_filePaths.takeFirst());
        m_fileNames.removeFirst();
        m_fileSizes.removeFirst();
    }
    return true;
}

void Capture::closeFile()
{
    if (!m_file.isOpen())
        return;
    m_file.close();
    // A file that only holds headers is of no use to anyone.
    if (m_filePackets == 0 && !m_filePaths.isEmpty()) {
        QFile::remove(m_filePaths.takeLast());
        m_fileNames.removeLast();
        m_fileSizes.removeLast();
    }
}

bool Capture::writeUnit(const char *data, qsizetype length)
{
    if (m_file.write(data, length) != length) {
        abortWith(tr("Writing to %1 failed: %2").arg(m_file.fileName(), m_file.errorString()));
        return false;
    }
    m_fileSizes.last() += length;
    m_bytesCaptured += length;
    m_statsDirty = true;
    return true;
}

void Capture::onStreamData()
{
    if (!m_process)
        return;
    const QByteArray data = m_process->readAllStandardOutput();
    if (m_discard || data.isEmpty())
        return;
    m_stream.feed(data);
    processStream();
}

void Capture::processStream()
{
    CaptureStream::Unit unit;
    while (!m_discard && m_stream.next(&unit)) {
        const char *bytes = m_stream.bytes(unit);
        if (unit.kind == CaptureStream::Kind::Preamble) {
            if (unit.sectionStart)
                m_preamble.clear();
            m_preamble.append(bytes, unit.length);
            if (m_state == QLatin1String("starting")) {
                // The first bytes of the stream: the capture is running.
                m_format = m_stream.format() == CaptureStream::Format::PcapNg ? QStringLiteral("pcapng") : QStringLiteral("pcap");
                m_clock.start();
                m_tick.start();
                m_statsTimer.start();
                setState(QStringLiteral("capturing"));
            }
            if (!m_file.isOpen()) {
                if (!openNextFile())
                    return;
            } else if (!writeUnit(bytes, unit.length)) {
                return;
            }
            continue;
        }
        if (!m_file.isOpen())
            continue;       // cannot happen in a well-formed stream
        if (unit.kind == CaptureStream::Kind::Packet) {
            const bool full = m_filePackets > 0
                && ((m_maxFileBytes > 0 && m_fileSizes.constLast() + unit.length > m_maxFileBytes) || m_rotateDue);
            if (full) {
                if (!m_ring) {
                    requestStop(tr("the size limit was reached"), true);
                    return;
                }
                m_file.close();
                if (!openNextFile())
                    return;
            }
            if (!writeUnit(bytes, unit.length))
                return;
            ++m_packets;
            ++m_filePackets;
        } else if (!writeUnit(bytes, unit.length)) {
            return;
        }
    }
    if (!m_stream.error().isEmpty() && !m_discard)
        abortWith(m_stream.error());
    m_stream.compact();
}

void Capture::onTick()
{
    m_statsDirty = true;
    if (m_state != QLatin1String("capturing"))
        return;
    if (m_file.isOpen())
        m_file.flush();
    refreshFiles();     // the growing file(s) of this capture
    if (m_maxSeconds > 0 && m_fileClock.isValid() && m_fileClock.elapsed() >= qint64(m_maxSeconds) * 1000) {
        if (m_ring)
            m_rotateDue = true;         // continue in a new file at the next packet
        else
            requestStop(tr("the time limit was reached"), true);
    }
    if (m_state == QLatin1String("capturing") && QStorageInfo(m_directory).bytesAvailable() < kMinFreeBytes)
        requestStop(tr("less than 32 MiB of disk space is left"), true);
}

void Capture::requestStop(const QString &reason, bool discardRest)
{
    if (m_state == QLatin1String("idle") || m_state == QLatin1String("stopping"))
        return;
    m_stopReason = reason;
    if (discardRest)
        m_discard = true;
    setState(QStringLiteral("stopping"));
    if (!m_process)
        return;
    if (m_backend == QLatin1String("dumpcap"))
        m_process->terminate();         // dumpcap finishes the file and exits on SIGTERM
    else
        m_process->closeWriteChannel(); // end-of-file on the helper's stdin
    m_killTimer.start();
}

void Capture::stop()
{
    requestStop(tr("stopped by you"), false);
}

void Capture::abortWith(const QString &error)
{
    if (m_failure.isEmpty())
        m_failure = error;
    requestStop(QString(), true);
}

void Capture::onProcessFinished(int exitCode, bool failedToStart)
{
    if (!m_process)
        return;
    QProcess *process = m_process;
    m_process = nullptr;
    if (!failedToStart) {
        m_stderr += process->readAllStandardError();
        const QByteArray rest = process->readAllStandardOutput();
        if (!m_discard && !rest.isEmpty()) {
            m_stream.feed(rest);
            processStream();
        }
    }
    process->deleteLater();
    m_tick.stop();
    m_statsTimer.stop();
    m_killTimer.stop();
    m_finalElapsedMs = m_clock.isValid() ? m_clock.elapsed() : 0;
    const bool streamStarted = !m_format.isEmpty();
    closeFile();

    const QString toolError = usefulError(m_stderr);
    QString dropped;
    static const QRegularExpression tcpdumpDrops(QStringLiteral("(\\d+) packets? dropped by kernel"));
    static const QRegularExpression dumpcapDrops(QStringLiteral("received/dropped on interface [^:]*: \\d+/(\\d+)"));
    for (const QRegularExpression &rx : {tcpdumpDrops, dumpcapDrops}) {
        const auto match = rx.match(QString::fromUtf8(m_stderr));
        if (match.hasMatch() && match.captured(1).toLongLong() > 0)
            dropped = tr(" The kernel dropped %1 packets because they arrived faster than they could be read.").arg(match.captured(1));
    }

    if (!m_failure.isEmpty()) {
        setMessage(QStringLiteral("danger"), tr("The capture was aborted: %1").arg(m_failure));
    } else if (!streamStarted) {
        QString reason;
        if (failedToStart)
            reason = tr("the capture program could not be started");
        else if (m_backend != QLatin1String("dumpcap") && !Net::pkexecFailure(exitCode, QString::fromUtf8(m_stderr)).isEmpty())
            reason = Net::pkexecFailure(exitCode, QString::fromUtf8(m_stderr));
        else if (!helperError(m_stderr).isEmpty())
            reason = helperError(m_stderr);
        else if (!toolError.isEmpty())
            reason = toolError;
        else if (!m_stopReason.isEmpty())
            reason = m_stopReason;
        else
            reason = tr("the capture program ended with exit code %1").arg(exitCode);
        setMessage(m_stopReason.isEmpty() ? QStringLiteral("danger") : QStringLiteral("info"),
                   tr("The capture on %1 did not start: %2").arg(m_interface, reason));
    } else if (m_packets == 0) {
        setMessage(QStringLiteral("info"),
                   tr("No packets were captured on %1 in %2 s%3; no file was kept.")
                       .arg(m_interface).arg(m_finalElapsedMs / 1000)
                       .arg(m_filter.isEmpty() ? QString() : tr(" with the filter “%1”").arg(m_filter)));
    } else {
        const QString why = !m_stopReason.isEmpty() ? m_stopReason
            : toolError.isEmpty() ? tr("the capture program ended") : tr("the capture program ended: %1").arg(toolError);
        const QLocale locale;
        QString text;
        if (m_fileNames.size() == 1 && m_fileIndex == 1) {
            text = tr("Captured %1 packets (%2) on %3 into %4 — %5.")
                       .arg(locale.toString(m_packets), locale.formattedDataSize(bytesOnDisk()), m_interface, m_fileNames.constFirst(), why);
        } else {
            // Ring buffer: older files may already have been replaced.
            text = tr("Captured %1 packets (%2) on %3 — %4. The newest %5 are kept in %n file(s), the last one being %6.",
                      nullptr, int(m_fileNames.size()))
                       .arg(locale.toString(m_packets), locale.formattedDataSize(m_bytesCaptured), m_interface, why,
                            locale.formattedDataSize(bytesOnDisk()), m_fileNames.constLast());
        }
        setMessage(QStringLiteral("success"), text + dropped);
    }
    setState(QStringLiteral("idle"));
    Q_EMIT statsChanged();
    refreshFiles();
}

// ---- capture files -----------------------------------------------------------------------

void Capture::refreshFiles()
{
    QVariantList files;
    const QDir dir(m_directory);
    const QFileInfoList entries = dir.entryInfoList({QStringLiteral("*.pcap"), QStringLiteral("*.pcapng"), QStringLiteral("*.cap")},
                                                    QDir::Files | QDir::NoSymLinks, QDir::Time);
    for (const QFileInfo &info : entries) {
        if (files.size() >= 500)
            break;
        QVariantMap m;
        m.insert(QStringLiteral("name"), info.fileName());
        m.insert(QStringLiteral("path"), info.absoluteFilePath());
        m.insert(QStringLiteral("size"), info.size());
        m.insert(QStringLiteral("modified"), info.lastModified().toString(Qt::ISODate));
        m.insert(QStringLiteral("modifiedText"), QLocale().toString(info.lastModified(), QLocale::ShortFormat));
        m.insert(QStringLiteral("active"), busy() && m_filePaths.contains(info.absoluteFilePath()));
        files << m;
    }
    if (files != m_files) {
        m_files = files;
        Q_EMIT filesChanged();
    }
}

QString Capture::insideDirectory(const QString &path) const
{
    // Only regular files directly inside the capture folder are acted upon.
    const QFileInfo info(path);
    const QString directory = QFileInfo(m_directory).canonicalFilePath();
    if (directory.isEmpty() || info.isSymLink() || !info.isFile())
        return {};
    const QString canonical = info.canonicalFilePath();
    return QFileInfo(canonical).path() == directory ? canonical : QString();
}

void Capture::openInWireshark(const QString &path)
{
    const QString file = insideDirectory(path);
    const QString wireshark = Net::findTool(QStringLiteral("wireshark"));
    if (file.isEmpty()) {
        Q_EMIT notice(QStringLiteral("danger"), tr("That capture file no longer exists."));
    } else if (wireshark.isEmpty()) {
        Q_EMIT notice(QStringLiteral("danger"), tr("Wireshark is not installed (package wireshark)."));
    } else if (!QProcess::startDetached(wireshark, {QStringLiteral("-r"), file})) {
        Q_EMIT notice(QStringLiteral("danger"), tr("Wireshark could not be started."));
    }
}

void Capture::showInFiles(const QString &path)
{
    const QString file = insideDirectory(path);
    const QString target = file.isEmpty() ? m_directory : file;
    if (!QFileInfo::exists(m_directory)) {
        Q_EMIT notice(QStringLiteral("info"), tr("The folder %1 does not exist yet; it is created by the first capture.").arg(m_directory));
        return;
    }
    auto openFolder = [this] {
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(m_directory)))
            Q_EMIT notice(QStringLiteral("danger"), tr("No file manager is available to show %1.").arg(m_directory));
    };
    // The freedesktop file manager interface selects the file in its folder.
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        openFolder();
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.FileManager1"),
                                                       QStringLiteral("/org/freedesktop/FileManager1"),
                                                       QStringLiteral("org.freedesktop.FileManager1"),
                                                       file.isEmpty() ? QStringLiteral("ShowFolders") : QStringLiteral("ShowItems"));
    call << QStringList{QUrl::fromLocalFile(target).toString()} << QString();
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call, 5000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [openFolder](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<> reply = *w;
        if (reply.isError())
            openFolder();
    });
}

void Capture::deleteFile(const QString &path)
{
    const QString file = insideDirectory(path);
    if (file.isEmpty()) {
        Q_EMIT notice(QStringLiteral("danger"), tr("That capture file no longer exists."));
    } else if (busy() && m_filePaths.contains(file)) {
        Q_EMIT notice(QStringLiteral("warning"), tr("This file belongs to the running capture."));
    } else if (!QFile::remove(file)) {
        Q_EMIT notice(QStringLiteral("danger"), tr("%1 could not be deleted.").arg(QFileInfo(file).fileName()));
    } else {
        Q_EMIT notice(QStringLiteral("success"), tr("%1 was deleted.").arg(QFileInfo(file).fileName()));
    }
    refreshFiles();
}

QJsonObject Capture::dump() const
{
    QJsonObject o = QJsonObject::fromVariantMap(m_capability);
    o.insert(QStringLiteral("files"), QJsonArray::fromVariantList(m_files));
    return o;
}
