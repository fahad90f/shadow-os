// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QStringList>
#include <QVariantList>

// Read-only facts about the machine and the installed system.
//
// Every value is read from the kernel or the system at refresh() time:
//   /etc/os-release, uname(2), /proc/cpuinfo, /proc/meminfo, /proc/uptime,
//   /sys/class/dmi/id, /sys/firmware/efi, /sys/class/drm, lspci (optional).
// A value that cannot be determined is an empty string — the UI shows "—"
// for it rather than a guess.
class SystemInfo : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString osName READ osName NOTIFY changed)
    Q_PROPERTY(QString osVersion READ osVersion NOTIFY changed)
    Q_PROPERTY(QString osCodename READ osCodename NOTIFY changed)
    Q_PROPERTY(QString osBase READ osBase NOTIFY changed)
    Q_PROPERTY(QString kernel READ kernel NOTIFY changed)
    Q_PROPERTY(QString architecture READ architecture NOTIFY changed)
    Q_PROPERTY(QString hostname READ hostname NOTIFY changed)
    Q_PROPERTY(QString desktop READ desktop NOTIFY changed)
    Q_PROPERTY(QString sessionType READ sessionType NOTIFY changed)
    Q_PROPERTY(QString qtVersion READ qtVersion CONSTANT)
    Q_PROPERTY(QString cpuModel READ cpuModel NOTIFY changed)
    Q_PROPERTY(int cpuThreads READ cpuThreads NOTIFY changed)
    Q_PROPERTY(int cpuCores READ cpuCores NOTIFY changed)
    Q_PROPERTY(qint64 memoryTotal READ memoryTotal NOTIFY changed)
    Q_PROPERTY(qint64 swapTotal READ swapTotal NOTIFY changed)
    Q_PROPERTY(QStringList gpus READ gpus NOTIFY changed)
    Q_PROPERTY(QString systemVendor READ systemVendor NOTIFY changed)
    Q_PROPERTY(QString systemProduct READ systemProduct NOTIFY changed)
    Q_PROPERTY(QString boardName READ boardName NOTIFY changed)
    Q_PROPERTY(QString firmwareVersion READ firmwareVersion NOTIFY changed)
    Q_PROPERTY(QString firmwareType READ firmwareType NOTIFY changed)
    Q_PROPERTY(QString secureBoot READ secureBoot NOTIFY changed)
    Q_PROPERTY(QString virtualization READ virtualization NOTIFY changed)
    Q_PROPERTY(qint64 uptimeSeconds READ uptimeSeconds NOTIFY uptimeChanged)
    Q_PROPERTY(QVariantList filesystems READ filesystems NOTIFY changed)

public:
    explicit SystemInfo(QObject *parent = nullptr);

    QString osName() const { return m_osName; }
    QString osVersion() const { return m_osVersion; }
    QString osCodename() const { return m_osCodename; }
    QString osBase() const { return m_osBase; }
    QString kernel() const { return m_kernel; }
    QString architecture() const { return m_architecture; }
    QString hostname() const { return m_hostname; }
    QString desktop() const { return m_desktop; }
    QString sessionType() const { return m_sessionType; }
    QString qtVersion() const;
    QString cpuModel() const { return m_cpuModel; }
    int cpuThreads() const { return m_cpuThreads; }
    int cpuCores() const { return m_cpuCores; }
    qint64 memoryTotal() const { return m_memoryTotal; }
    qint64 swapTotal() const { return m_swapTotal; }
    QStringList gpus() const { return m_gpus; }
    QString systemVendor() const { return m_systemVendor; }
    QString systemProduct() const { return m_systemProduct; }
    QString boardName() const { return m_boardName; }
    QString firmwareVersion() const { return m_firmwareVersion; }
    QString firmwareType() const { return m_firmwareType; }
    QString secureBoot() const { return m_secureBoot; }
    QString virtualization() const { return m_virtualization; }
    qint64 uptimeSeconds() const { return m_uptime; }
    QVariantList filesystems() const { return m_filesystems; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void refreshUptime();
    // Plain-text report of everything shown, for bug reports.
    Q_INVOKABLE QString report() const;
    Q_INVOKABLE void copyToClipboard(const QString &text) const;

Q_SIGNALS:
    void changed();
    void uptimeChanged();

private:
    void readOsRelease();
    void readCpu();
    void readMemory();
    void readDmi();
    void readFirmware();
    void readGpus();
    void readFilesystems();

    QString m_osName, m_osVersion, m_osCodename, m_osBase, m_kernel, m_architecture, m_hostname;
    QString m_desktop, m_sessionType, m_cpuModel;
    int m_cpuThreads = 0, m_cpuCores = 0;
    qint64 m_memoryTotal = 0, m_swapTotal = 0, m_uptime = 0;
    QStringList m_gpus;
    QString m_systemVendor, m_systemProduct, m_boardName, m_firmwareVersion, m_firmwareType, m_secureBoot;
    QString m_virtualization;
    QVariantList m_filesystems;
};
