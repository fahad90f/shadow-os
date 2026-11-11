// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu.h"

#include "sysfs.h"

#include <command.h>

#include <cmath>

using namespace Qt::StringLiterals;

namespace {

QVariant optional(bool available, const QVariant &value)
{
    return available ? value : QVariant::fromValue(nullptr);
}

double round1(double v)
{
    return std::round(v * 10.0) / 10.0;
}

bool isCardName(const QString &entry)
{
    // "card0", not the connector entries "card0-HDMI-A-1".
    if (!entry.startsWith("card"_L1) || entry.size() < 5)
        return false;
    for (qsizetype i = 4; i < entry.size(); ++i) {
        if (!entry.at(i).isDigit())
            return false;
    }
    return true;
}

// "Navi 21 [Radeon RX 6800/6800 XT / 6900 XT]" → "Radeon RX 6800/6800 XT / 6900 XT"
QString marketingName(const QString &name)
{
    const qsizetype open = name.lastIndexOf(u'[');
    const qsizetype close = name.lastIndexOf(u']');
    if (open >= 0 && close > open + 1)
        return name.mid(open + 1, close - open - 1).trimmed();
    return name;
}

// Product name of a PCI device from the PCI ID database (the file lspci
// uses). 'vendor' and 'device' are sysfs values such as "0x1002".
QString pciProductName(QString vendor, QString device)
{
    vendor = vendor.mid(2).toLower();
    device = device.mid(2).toLower();
    if (vendor.size() != 4 || device.size() != 4)
        return {};

    QByteArray db;
    for (const QString &path : {u"/usr/share/misc/pci.ids"_s, u"/usr/share/hwdata/pci.ids"_s}) {
        db = Sys::readFile(path, 32 << 20);
        if (!db.isEmpty())
            break;
    }

    // Vendor lines start in column 0 ("1002  Advanced Micro Devices, Inc. [AMD/ATI]"),
    // the vendor's devices follow, indented by one tab ("\t73bf  Navi 21 [...]").
    QString vendorName, deviceName;
    qsizetype pos = db.indexOf('\n' + vendor.toLatin1() + "  ");
    if (pos >= 0) {
        ++pos;
        qsizetype eol = db.indexOf('\n', pos);
        if (eol < 0)
            eol = db.size();
        vendorName = QString::fromUtf8(db.mid(pos + 6, eol - pos - 6)).trimmed();
        const QByteArray deviceKey = '\t' + device.toLatin1() + "  ";
        for (pos = eol + 1; pos < db.size(); pos = eol + 1) {
            eol = db.indexOf('\n', pos);
            if (eol < 0)
                eol = db.size();
            const QByteArrayView line(db.constData() + pos, eol - pos);
            if (line.isEmpty() || line.at(0) == '#')
                continue;
            if (line.at(0) != '\t')
                break;              // next vendor
            if (line.startsWith(deviceKey)) {
                deviceName = QString::fromUtf8(line.mid(deviceKey.size())).trimmed();
                break;
            }
        }
    }

    QString brand = vendor == "1002"_L1 ? u"AMD"_s
        : vendor == "10de"_L1 ? u"NVIDIA"_s
        : vendor == "8086"_L1 ? u"Intel"_s
        : marketingName(vendorName);
    if (brand.isEmpty() && deviceName.isEmpty())
        return {};
    if (deviceName.isEmpty())
        return brand;
    const QString product = marketingName(deviceName);
    return brand.isEmpty() || product.startsWith(brand, Qt::CaseInsensitive) ? product : brand + u' ' + product;
}

} // namespace

QVariantMap GpuInfo::toVariant() const
{
    return {
        {u"card"_s, card},
        {u"name"_s, name},
        {u"driver"_s, driver},
        {u"pciSlot"_s, pciSlot},
        {u"source"_s, source},
        {u"suspended"_s, suspended},
        {u"note"_s, note},
        {u"utilizationAvailable"_s, utilizationAvailable},
        {u"utilizationPercent"_s, optional(utilizationAvailable, round1(utilization))},
        {u"temperatureAvailable"_s, temperatureAvailable},
        {u"temperatureCelsius"_s, optional(temperatureAvailable, round1(temperature))},
        {u"memoryAvailable"_s, memoryAvailable},
        {u"memoryUsed"_s, optional(memoryAvailable, memoryUsed)},
        {u"memoryTotal"_s, optional(memoryAvailable, memoryTotal)},
        {u"powerWatts"_s, optional(powerAvailable, round1(powerWatts))},
        {u"fanRpm"_s, optional(fanAvailable, fanRpm)},
    };
}

void GpuSource::discover(const QStringList &entries)
{
    m_cards.clear();
    for (const QString &entry : entries) {
        Card c;
        c.card = entry;
        c.devicePath = u"/sys/class/drm/"_s + entry + u"/device"_s;
        c.driver = Sys::linkName(c.devicePath + u"/driver"_s);

        const QString vendor = Sys::readText(c.devicePath + u"/vendor"_s);
        const QString device = Sys::readText(c.devicePath + u"/device"_s);
        if (vendor.startsWith("0x"_L1) && device.startsWith("0x"_L1)) {
            c.pciSlot = Sys::linkName(c.devicePath);
            c.name = pciProductName(vendor, device);
        }
        if (c.name.isEmpty()) {
            // Platform devices (most ARM SoCs): the first device-tree "compatible" string.
            const QByteArray compatible = Sys::readFile(c.devicePath + u"/of_node/compatible"_s, 512);
            c.name = QString::fromLatin1(compatible.left(compatible.indexOf('\0')));
        }
        if (c.name.isEmpty())
            c.name = c.driver.isEmpty() ? tr("Graphics device") : tr("Graphics device (%1 driver)").arg(c.driver);
        m_cards << c;
    }
}

QList<GpuInfo> GpuSource::sample()
{
    QStringList entries;
    const QStringList drm = Sys::listDirectory(u"/sys/class/drm"_s);
    for (const QString &entry : drm) {
        if (isCardName(entry))
            entries << entry;
    }
    if (!m_discovered || entries != m_entries) {
        discover(entries);
        m_entries = entries;
        m_discovered = true;
    }

    QList<GpuInfo> gpus;
    for (const Card &c : std::as_const(m_cards)) {
        GpuInfo g;
        g.card = c.card;
        g.name = c.name;
        g.driver = c.driver;
        g.pciSlot = c.pciSlot;

        // Reading the sensors of a runtime-suspended GPU would power it up
        // again (and keep a laptop's discrete GPU awake for as long as the
        // monitor runs), so a sleeping device is left alone.
        const QString runtime = Sys::readText(c.devicePath + u"/power/runtime_status"_s);
        g.suspended = runtime == "suspended"_L1 || runtime == "suspending"_L1;

        if (!g.suspended && c.driver != "nvidia"_L1) {
            if (const auto busy = Sys::readInteger(c.devicePath + u"/gpu_busy_percent"_s)) {
                g.utilizationAvailable = true;
                g.utilization = qBound(0.0, double(*busy), 100.0);
                g.source = u"sysfs"_s;
            }
            const auto used = Sys::readInteger(c.devicePath + u"/mem_info_vram_used"_s);
            const auto total = Sys::readInteger(c.devicePath + u"/mem_info_vram_total"_s);
            if (used && total && *total > 0) {
                g.memoryAvailable = true;
                g.memoryUsed = *used;
                g.memoryTotal = *total;
                g.source = u"sysfs"_s;
            }
            const QStringList hwmon = Sys::listDirectory(c.devicePath + u"/hwmon"_s);
            if (!hwmon.isEmpty()) {
                const QString base = c.devicePath + u"/hwmon/"_s + hwmon.first() + u'/';
                if (const auto temp = Sys::readInteger(base + u"temp1_input"_s)) {
                    g.temperatureAvailable = true;
                    g.temperature = double(*temp) / 1000.0;
                    g.source = u"sysfs"_s;
                }
                auto power = Sys::readInteger(base + u"power1_average"_s);
                if (!power)
                    power = Sys::readInteger(base + u"power1_input"_s);
                if (power) {
                    g.powerAvailable = true;
                    g.powerWatts = double(*power) / 1e6;
                }
                if (const auto fan = Sys::readInteger(base + u"fan1_input"_s)) {
                    g.fanAvailable = true;
                    g.fanRpm = int(*fan);
                }
            }
        }
        gpus << g;
    }

    queryNvidia(gpus);

    for (GpuInfo &g : gpus) {
        if (g.utilizationAvailable || !g.note.isEmpty())
            continue;
        if (g.suspended)
            g.note = tr("Powered down to save energy (runtime suspended)");
        else if (g.driver.isEmpty())
            g.note = tr("Utilisation is not reported by the driver");
        else
            g.note = tr("Utilisation is not reported by the %1 driver").arg(g.driver);
    }
    return gpus;
}

// The proprietary NVIDIA driver exposes nothing useful in sysfs; its figures
// come from nvidia-smi, if that tool is installed. The devices the driver
// handles are listed in /proc/driver/nvidia/gpus/<pci slot>/.
void GpuSource::queryNvidia(QList<GpuInfo> &gpus)
{
    const QStringList pciSlots = Sys::listDirectory(u"/proc/driver/nvidia/gpus"_s);
    if (pciSlots.isEmpty())
        return;

    auto find = [&gpus](const QString &slot) -> GpuInfo * {
        for (GpuInfo &g : gpus) {
            if (g.pciSlot.compare(slot, Qt::CaseInsensitive) == 0)
                return &g;
        }
        return nullptr;
    };

    bool anyAwake = false;
    for (const QString &slot : pciSlots) {
        const QString pci = u"/sys/bus/pci/devices/"_s + slot.toLower();
        if (!find(slot)) {
            // Driver loaded without its DRM part: no /sys/class/drm node.
            GpuInfo g;
            g.pciSlot = slot.toLower();
            g.name = pciProductName(Sys::readText(pci + u"/vendor"_s), Sys::readText(pci + u"/device"_s));
            if (g.name.isEmpty())
                g.name = tr("NVIDIA graphics device");
            gpus << g;
        }
        GpuInfo *g = find(slot);
        g->driver = u"nvidia"_s;
        const QString runtime = Sys::readText(pci + u"/power/runtime_status"_s);
        g->suspended = runtime == "suspended"_L1 || runtime == "suspending"_L1;
        anyAwake = anyAwake || !g->suspended;
    }

    if (m_nvidiaSmi == 0)
        m_nvidiaSmi = Shadow::Command::available(u"nvidia-smi"_s) ? 1 : -1;
    QString failure;
    if (m_nvidiaSmi < 0) {
        failure = tr("nvidia-smi is not installed, so the NVIDIA driver's figures cannot be read");
    } else if (anyAwake) {
        const Shadow::CommandResult r = Shadow::Command::runSync(u"nvidia-smi"_s, {
            u"--query-gpu=pci.bus_id,utilization.gpu,temperature.gpu,memory.used,memory.total,power.draw"_s,
            u"--format=csv,noheader,nounits"_s,
        }, 4000);
        if (!r.ok()) {
            const QString text = (r.errText().trimmed().isEmpty() ? r.outText() : r.errText()).trimmed();
            failure = r.exitCode == -2 ? tr("nvidia-smi did not answer in time")
                                       : tr("nvidia-smi failed: %1").arg(text.section(u'\n', 0, 0));
        } else {
            // "00000000:01:00.0, 3, 45, 512, 10240, 35.12" — unsupported fields read "[N/A]".
            const QStringList lines = r.outText().split(u'\n', Qt::SkipEmptyParts);
            for (const QString &line : lines) {
                const QStringList f = line.split(u',');
                if (f.size() < 6)
                    continue;
                GpuInfo *g = find(f.at(0).trimmed().right(12));
                if (!g || g->suspended)
                    continue;
                g->source = u"nvidia-smi"_s;
                bool ok = false;
                double v = f.at(1).trimmed().toDouble(&ok);
                if (ok) {
                    g->utilizationAvailable = true;
                    g->utilization = qBound(0.0, v, 100.0);
                }
                v = f.at(2).trimmed().toDouble(&ok);
                if (ok) {
                    g->temperatureAvailable = true;
                    g->temperature = v;
                }
                bool okTotal = false;
                const double usedMib = f.at(3).trimmed().toDouble(&ok);
                const double totalMib = f.at(4).trimmed().toDouble(&okTotal);
                if (ok && okTotal && totalMib > 0) {
                    g->memoryAvailable = true;
                    g->memoryUsed = qint64(usedMib * 1048576.0);
                    g->memoryTotal = qint64(totalMib * 1048576.0);
                }
                v = f.at(5).trimmed().toDouble(&ok);
                if (ok) {
                    g->powerAvailable = true;
                    g->powerWatts = v;
                }
            }
        }
    }

    for (const QString &slot : pciSlots) {
        GpuInfo *g = find(slot);
        if (g->utilizationAvailable || g->suspended)
            continue;
        g->note = failure.isEmpty() ? tr("Utilisation is not reported by nvidia-smi for this device") : failure;
    }
}
