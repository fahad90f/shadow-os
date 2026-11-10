// SPDX-License-Identifier: GPL-3.0-or-later
#include "systeminfo.h"

#include <command.h>

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QLibraryInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStorageInfo>
#include <QTextStream>

#include <sys/utsname.h>
#include <unistd.h>

namespace {

QString readFile(const QString &path, qint64 limit = 1 << 20)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.read(limit)).trimmed();
}

// DMI strings from some firmware are placeholders; treat them as unknown.
QString dmi(const QString &name)
{
    const QString v = readFile(QStringLiteral("/sys/class/dmi/id/") + name);
    static const QStringList junk = {
        QStringLiteral("To Be Filled By O.E.M."), QStringLiteral("To be filled by O.E.M."),
        QStringLiteral("Default string"), QStringLiteral("System Product Name"),
        QStringLiteral("System manufacturer"), QStringLiteral("Not Specified"),
        QStringLiteral("None"), QStringLiteral("N/A"),
    };
    return junk.contains(v) ? QString() : v;
}

} // namespace

SystemInfo::SystemInfo(QObject *parent)
    : QObject(parent)
{
    refresh();
}

QString SystemInfo::qtVersion() const
{
    return QString::fromLatin1(qVersion());
}

void SystemInfo::refresh()
{
    readOsRelease();

    struct utsname u {};
    if (uname(&u) == 0) {
        m_kernel = QString::fromLocal8Bit(u.release);
        m_architecture = QString::fromLocal8Bit(u.machine);
        m_hostname = QString::fromLocal8Bit(u.nodename);
    }

    m_desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
    m_sessionType = qEnvironmentVariable("XDG_SESSION_TYPE");

    readCpu();
    readMemory();
    readDmi();
    readFirmware();
    readGpus();
    readFilesystems();
    refreshUptime();
    Q_EMIT changed();
}

void SystemInfo::refreshUptime()
{
    const QString s = readFile(QStringLiteral("/proc/uptime"));
    const qint64 up = qint64(s.section(QLatin1Char(' '), 0, 0).toDouble());
    if (up != m_uptime) {
        m_uptime = up;
        Q_EMIT uptimeChanged();
    }
}

void SystemInfo::readOsRelease()
{
    m_osName.clear(); m_osVersion.clear(); m_osCodename.clear(); m_osBase.clear();
    QFile f(QStringLiteral("/etc/os-release"));
    if (!f.open(QIODevice::ReadOnly))
        f.setFileName(QStringLiteral("/usr/lib/os-release"));
    if (!f.isOpen() && !f.open(QIODevice::ReadOnly))
        return;
    QString idLike;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0 || line.startsWith(QLatin1Char('#')))
            continue;
        const QString key = line.left(eq);
        QString value = line.mid(eq + 1);
        if (value.size() >= 2 && (value.startsWith(QLatin1Char('"')) || value.startsWith(QLatin1Char('\''))))
            value = value.mid(1, value.size() - 2);
        if (key == QLatin1String("NAME")) m_osName = value;
        else if (key == QLatin1String("VERSION_ID")) m_osVersion = value;
        else if (key == QLatin1String("VERSION_CODENAME")) m_osCodename = value;
        else if (key == QLatin1String("ID_LIKE")) idLike = value;
        else if (key == QLatin1String("NEXUS_BASE")) m_osBase = value;
    }
    if (m_osBase.isEmpty() && !idLike.isEmpty())
        m_osBase = idLike;
}

void SystemInfo::readCpu()
{
    m_cpuModel.clear();
    m_cpuThreads = 0;
    QSet<QString> cores;
    QString physical, implementer, part;

    QFile f(QStringLiteral("/proc/cpuinfo"));
    if (f.open(QIODevice::ReadOnly)) {
        // /proc files report size 0, so read until EOF rather than atEnd().
        const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            const int colon = line.indexOf(QLatin1Char(':'));
            if (colon < 0)
                continue;
            const QString key = line.left(colon).trimmed();
            const QString value = line.mid(colon + 1).trimmed();
            if (key == QLatin1String("processor")) {
                ++m_cpuThreads;
            } else if (key == QLatin1String("model name") && m_cpuModel.isEmpty()) {
                m_cpuModel = value;
            } else if (key == QLatin1String("physical id")) {
                physical = value;
            } else if (key == QLatin1String("core id")) {
                cores.insert(physical + QLatin1Char(':') + value);
            } else if (key == QLatin1String("CPU implementer") && implementer.isEmpty()) {
                implementer = value;
            } else if (key == QLatin1String("CPU part") && part.isEmpty()) {
                part = value;
            }
        }
    }
    m_cpuCores = cores.isEmpty() ? m_cpuThreads : int(cores.size());

    if (m_cpuModel.isEmpty()) {
        // AArch64 has no "model name"; lscpu resolves implementer/part to a name.
        const Nexus::CommandResult r = Nexus::Command::runSync(QStringLiteral("lscpu"), {}, 3000);
        if (r.ok()) {
            const QStringList lines = r.outText().split(QLatin1Char('\n'));
            for (const QString &line : lines) {
                if (line.startsWith(QLatin1String("Model name:"))) {
                    m_cpuModel = line.section(QLatin1Char(':'), 1).trimmed();
                    break;
                }
            }
        }
        if (m_cpuModel.isEmpty() && !implementer.isEmpty())
            m_cpuModel = QStringLiteral("ARM implementer %1 part %2").arg(implementer, part);
    }
}

void SystemInfo::readMemory()
{
    m_memoryTotal = m_swapTotal = 0;
    QFile f(QStringLiteral("/proc/meminfo"));
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
    static const QRegularExpression rx(QStringLiteral("^(\\w+):\\s+(\\d+)\\s*kB"));
    for (const QString &line : lines) {
        const auto m = rx.match(line);
        if (!m.hasMatch())
            continue;
        if (m.captured(1) == QLatin1String("MemTotal"))
            m_memoryTotal = m.captured(2).toLongLong() * 1024;
        else if (m.captured(1) == QLatin1String("SwapTotal"))
            m_swapTotal = m.captured(2).toLongLong() * 1024;
    }
}

void SystemInfo::readDmi()
{
    m_systemVendor = dmi(QStringLiteral("sys_vendor"));
    m_systemProduct = dmi(QStringLiteral("product_name"));
    const QString boardVendor = dmi(QStringLiteral("board_vendor"));
    const QString board = dmi(QStringLiteral("board_name"));
    m_boardName = board.isEmpty() ? QString()
        : (boardVendor.isEmpty() || board.startsWith(boardVendor) ? board : boardVendor + QLatin1Char(' ') + board);
    const QString biosVendor = dmi(QStringLiteral("bios_vendor"));
    const QString biosVersion = dmi(QStringLiteral("bios_version"));
    const QString biosDate = dmi(QStringLiteral("bios_date"));
    QStringList fw;
    if (!biosVendor.isEmpty()) fw << biosVendor;
    if (!biosVersion.isEmpty()) fw << biosVersion;
    if (!biosDate.isEmpty()) fw << QLatin1Char('(') + biosDate + QLatin1Char(')');
    m_firmwareVersion = fw.join(QLatin1Char(' '));

    // Device-tree machines (many ARM boards) have no DMI at all.
    if (m_systemProduct.isEmpty()) {
        QString model = readFile(QStringLiteral("/sys/firmware/devicetree/base/model"));
        model.remove(QChar(0));
        m_systemProduct = model;
    }

    const Nexus::CommandResult virt = Nexus::Command::runSync(QStringLiteral("systemd-detect-virt"), {}, 3000);
    const QString v = virt.outText().trimmed();
    m_virtualization = (v.isEmpty() || v == QLatin1String("none")) ? QString() : v;
}

void SystemInfo::readFirmware()
{
    if (!QDir(QStringLiteral("/sys/firmware/efi")).exists()) {
        m_firmwareType = QDir(QStringLiteral("/sys/firmware/devicetree")).exists()
            ? QStringLiteral("Device tree (no UEFI)") : QStringLiteral("Legacy BIOS");
        m_secureBoot = QStringLiteral("Not supported (no UEFI)");
        return;
    }
    const QString bits = readFile(QStringLiteral("/sys/firmware/efi/fw_platform_size"));
    m_firmwareType = bits.isEmpty() ? QStringLiteral("UEFI") : QStringLiteral("UEFI (%1-bit)").arg(bits);

    // EFI variable payload: 4 attribute bytes followed by the 1-byte value.
    QFile sb(QStringLiteral("/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c"));
    if (!sb.open(QIODevice::ReadOnly)) {
        m_secureBoot = QStringLiteral("Unknown (EFI variables not readable)");
        return;
    }
    const QByteArray data = sb.readAll();
    if (data.size() < 5) {
        m_secureBoot = QStringLiteral("Unknown");
        return;
    }
    bool setupMode = false;
    QFile sm(QStringLiteral("/sys/firmware/efi/efivars/SetupMode-8be4df61-93ca-11d2-aa0d-00e098032b8c"));
    if (sm.open(QIODevice::ReadOnly)) {
        const QByteArray d = sm.readAll();
        setupMode = d.size() >= 5 && d.at(4) == 1;
    }
    m_secureBoot = data.at(4) == 1 ? QStringLiteral("Enabled")
        : setupMode ? QStringLiteral("Disabled (setup mode)") : QStringLiteral("Disabled");
}

void SystemInfo::readGpus()
{
    m_gpus.clear();
    // Preferred: lspci resolves vendor/device IDs to names.
    if (Nexus::Command::available(QStringLiteral("lspci"))) {
        const Nexus::CommandResult r = Nexus::Command::runSync(QStringLiteral("lspci"), {QStringLiteral("-mm")}, 5000);
        if (r.ok()) {
            static const QRegularExpression rx(QStringLiteral("\"([^\"]*)\""));
            const QStringList lines = r.outText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            for (const QString &line : lines) {
                QStringList fields;
                auto it = rx.globalMatch(line);
                while (it.hasNext())
                    fields << it.next().captured(1);
                if (fields.size() < 3)
                    continue;
                const QString cls = fields.at(0);
                if (cls.contains(QLatin1String("VGA")) || cls.contains(QLatin1String("3D controller"))
                    || cls.contains(QLatin1String("Display controller")))
                    m_gpus << fields.at(1) + QLatin1Char(' ') + fields.at(2);
            }
        }
    }
    if (!m_gpus.isEmpty())
        return;
    // Fallback: DRM devices by driver name (covers platform/virtio GPUs too).
    const QDir drm(QStringLiteral("/sys/class/drm"));
    const QStringList cards = drm.entryList({QStringLiteral("card?")}, QDir::Dirs);
    for (const QString &card : cards) {
        const QString driver = QFileInfo(drm.filePath(card + QStringLiteral("/device/driver"))).canonicalFilePath()
                                   .section(QLatin1Char('/'), -1);
        if (!driver.isEmpty())
            m_gpus << QStringLiteral("%1 (%2 driver)").arg(card, driver);
    }
}

void SystemInfo::readFilesystems()
{
    m_filesystems.clear();
    const auto volumes = QStorageInfo::mountedVolumes();
    for (const QStorageInfo &v : volumes) {
        if (!v.isValid() || !v.isReady() || v.bytesTotal() <= 0)
            continue;
        const QString fs = QString::fromLatin1(v.fileSystemType());
        static const QSet<QString> pseudo = {
            QStringLiteral("tmpfs"), QStringLiteral("devtmpfs"), QStringLiteral("squashfs"),
            QStringLiteral("efivarfs"), QStringLiteral("ramfs"), QStringLiteral("overlay"),
        };
        const QString mount = v.rootPath();
        if (pseudo.contains(fs) && mount != QLatin1String("/"))
            continue;
        if (mount.startsWith(QLatin1String("/run/")) || mount.startsWith(QLatin1String("/snap/")))
            continue;
        QVariantMap m;
        m.insert(QStringLiteral("mount"), mount);
        m.insert(QStringLiteral("device"), QString::fromLocal8Bit(v.device()));
        m.insert(QStringLiteral("type"), fs);
        m.insert(QStringLiteral("total"), v.bytesTotal());
        m.insert(QStringLiteral("free"), v.bytesAvailable());
        m.insert(QStringLiteral("used"), v.bytesTotal() - v.bytesFree());
        m_filesystems << m;
    }
}

QString SystemInfo::report() const
{
    QString out;
    QTextStream s(&out);
    auto line = [&s](const char *label, const QString &value) {
        s << QString::fromLatin1(label).leftJustified(18) << (value.isEmpty() ? QStringLiteral("unknown") : value) << '\n';
    };
    line("System:", QStringLiteral("%1 %2 (%3)").arg(m_osName, m_osVersion, m_osCodename));
    line("Based on:", m_osBase);
    line("Kernel:", m_kernel);
    line("Architecture:", m_architecture);
    line("Desktop:", m_desktop);
    line("Session:", m_sessionType);
    line("Qt:", qtVersion());
    line("Computer:", QStringList({m_systemVendor, m_systemProduct}).join(QLatin1Char(' ')).trimmed());
    line("Board:", m_boardName);
    line("Firmware:", QStringList({m_firmwareType, m_firmwareVersion}).join(QLatin1Char(' ')).trimmed());
    line("Secure Boot:", m_secureBoot);
    line("Virtualization:", m_virtualization.isEmpty() ? QStringLiteral("none detected") : m_virtualization);
    line("Processor:", QStringLiteral("%1 (%2 cores, %3 threads)").arg(m_cpuModel).arg(m_cpuCores).arg(m_cpuThreads));
    line("Memory:", QStringLiteral("%1 MiB").arg(m_memoryTotal / (1024 * 1024)));
    line("Swap:", QStringLiteral("%1 MiB").arg(m_swapTotal / (1024 * 1024)));
    for (const QString &gpu : m_gpus)
        line("Graphics:", gpu);
    for (const QVariant &v : m_filesystems) {
        const QVariantMap m = v.toMap();
        line("Filesystem:", QStringLiteral("%1 on %2 (%3), %4 MiB of %5 MiB used")
                 .arg(m.value(QStringLiteral("device")).toString(), m.value(QStringLiteral("mount")).toString(),
                      m.value(QStringLiteral("type")).toString())
                 .arg(m.value(QStringLiteral("used")).toLongLong() / (1024 * 1024))
                 .arg(m.value(QStringLiteral("total")).toLongLong() / (1024 * 1024)));
    }
    return out;
}

void SystemInfo::copyToClipboard(const QString &text) const
{
    QGuiApplication::clipboard()->setText(text);
}
