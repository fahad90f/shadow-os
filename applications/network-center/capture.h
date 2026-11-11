// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "capturestream.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QQmlEngine>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

// Packet capture to files under ~/Captures.
//
// Two back-ends, chosen by what the system allows (see capability):
//
//   dumpcap   (wireshark-common) runs as the user; Debian restricts it to
//             members of the "wireshark" group. Invoked as
//             `dumpcap -q -i IF [-f FILTER] -w -`, it writes pcapng to stdout.
//   tcpdump   through the privileged helper (`network-helper capture …`,
//             polkit action org.nexus.networkcenter.capture), which runs
//             `tcpdump -i IF -U -w - [-- FILTER]` and writes pcap to stdout.
//
// In both cases the capture arrives on a pipe and this (unprivileged) process
// writes the files: it counts packets, enforces the size and time limits and
// rotates the ring buffer at packet boundaries. No privileged process ever
// opens a file in the user's home directory, and the capture ends when this
// application does.
class Capture : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QVariantMap capability READ capability NOTIFY capabilityChanged)
    Q_PROPERTY(bool checking READ checking NOTIFY capabilityChanged)
    Q_PROPERTY(bool wiresharkInstalled READ wiresharkInstalled NOTIFY capabilityChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)       // idle | starting | capturing | stopping
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString interface READ interface NOTIFY stateChanged)
    Q_PROPERTY(QString format READ format NOTIFY stateChanged)
    Q_PROPERTY(qint64 packets READ packets NOTIFY statsChanged)
    Q_PROPERTY(qint64 bytesOnDisk READ bytesOnDisk NOTIFY statsChanged)
    Q_PROPERTY(qint64 bytesCaptured READ bytesCaptured NOTIFY statsChanged)
    Q_PROPERTY(int elapsedSeconds READ elapsedSeconds NOTIFY statsChanged)
    Q_PROPERTY(QString currentFile READ currentFile NOTIFY statsChanged)
    Q_PROPERTY(QStringList sessionFiles READ sessionFiles NOTIFY statsChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(QString messageKind READ messageKind NOTIFY messageChanged)
    Q_PROPERTY(QString filterState READ filterState NOTIFY filterChanged)   // "" | checking | valid | invalid | unavailable
    Q_PROPERTY(QString filterMessage READ filterMessage NOTIFY filterChanged)
    Q_PROPERTY(QString directory READ directory CONSTANT)
    Q_PROPERTY(QVariantList files READ files NOTIFY filesChanged)

public:
    explicit Capture(QObject *parent = nullptr);
    ~Capture() override;

    QVariantMap capability() const { return m_capability; }
    bool checking() const { return m_checking; }
    bool wiresharkInstalled() const { return m_wireshark; }
    QString state() const { return m_state; }
    bool busy() const { return m_state != QLatin1String("idle"); }
    QString interface() const { return m_interface; }
    QString format() const { return m_format; }
    qint64 packets() const { return m_packets; }
    qint64 bytesOnDisk() const;
    qint64 bytesCaptured() const { return m_bytesCaptured; }
    int elapsedSeconds() const;
    QString currentFile() const { return m_fileNames.isEmpty() ? QString() : m_fileNames.constLast(); }
    QStringList sessionFiles() const { return m_fileNames; }
    QString message() const { return m_message; }
    QString messageKind() const { return m_messageKind; }
    QString filterState() const { return m_filterState; }
    QString filterMessage() const { return m_filterMessage; }
    QString directory() const { return m_directory; }
    QVariantList files() const { return m_files; }

    // Work out which back-end can capture and, if none can, exactly why.
    Q_INVOKABLE void refreshCapability();
    // Dry-compile a capture filter (BPF expression) for an interface.
    Q_INVOKABLE void validateFilter(const QString &interface, const QString &expression);
    Q_INVOKABLE void clearFilterState();
    // options: interface, filter, maxFileMegabytes (0 = no limit),
    // maxSeconds (0 = no limit), ring (bool), ringFiles (files kept).
    // Without 'ring' the capture stops at the first limit reached; with it a
    // new file is started there and only the newest 'ringFiles' are kept.
    Q_INVOKABLE void start(const QVariantMap &options);
    Q_INVOKABLE void stop();
    Q_INVOKABLE void refreshFiles();
    Q_INVOKABLE void openInWireshark(const QString &path);
    Q_INVOKABLE void showInFiles(const QString &path);
    Q_INVOKABLE void deleteFile(const QString &path);

    QJsonObject dump() const;

Q_SIGNALS:
    void capabilityChanged();
    void capabilityReady();
    void stateChanged();
    void statsChanged();
    void messageChanged();
    void filterChanged();
    void filesChanged();
    // Feedback for an action on a capture file (toast).
    void notice(const QString &kind, const QString &text);

private:
    void finishCapability(QVariantMap dumpcap, bool dumpcapUsable);
    void compileFilter(const QString &interface, const QString &expression,
                       std::function<void(const QString &state, const QString &message)> done);
    void setFilter(const QString &state, const QString &message);
    void setMessage(const QString &kind, const QString &text);
    void setState(const QString &state);
    void launch();
    void onStreamData();
    void processStream();
    void onProcessFinished(int exitCode, bool failedToStart);
    bool openNextFile();
    void closeFile();
    bool writeUnit(const char *data, qsizetype length);
    void requestStop(const QString &reason, bool discardRest);
    void abortWith(const QString &error);
    void onTick();
    QString insideDirectory(const QString &path) const;

    QVariantMap m_capability;
    bool m_checking = false;
    bool m_wireshark = false;
    int m_capabilitySerial = 0;

    QString m_state = QStringLiteral("idle");
    QString m_message, m_messageKind;
    QString m_filterState, m_filterMessage;
    int m_filterSerial = 0;
    QString m_directory;
    QVariantList m_files;

    // the running (or last) capture
    QString m_backend, m_interface, m_filter, m_format, m_baseName;
    qint64 m_maxFileBytes = 0;
    int m_maxSeconds = 0;
    bool m_ring = false;
    int m_ringFiles = 2;
    QPointer<QProcess> m_process;
    QByteArray m_stderr;
    CaptureStream m_stream;
    QByteArray m_preamble;
    QFile m_file;
    QStringList m_filePaths, m_fileNames;
    QList<qint64> m_fileSizes;
    int m_fileIndex = 0;
    qint64 m_filePackets = 0;
    qint64 m_packets = 0, m_bytesCaptured = 0;
    QElapsedTimer m_clock, m_fileClock;
    qint64 m_finalElapsedMs = 0;
    bool m_rotateDue = false;
    bool m_discard = false;
    QString m_stopReason, m_failure;
    bool m_statsDirty = false;
    QTimer m_tick, m_statsTimer, m_killTimer;
};
