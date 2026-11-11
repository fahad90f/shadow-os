// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QElapsedTimer>
#include <QString>
#include <QThread>

#include <atomic>

// Copies a block device into an image file, or an image file onto a block
// device, in a worker thread. The device is an open file descriptor handed
// out by UDisks (Block.OpenForBackup / Block.OpenForRestore), so this
// process never opens a device node itself.
class ImageWorker : public QThread
{
    Q_OBJECT
public:
    ImageWorker(bool restore, int deviceFd, const QString &file, qint64 total, QObject *parent = nullptr);
    ~ImageWorker() override;

    bool restore() const { return m_restore; }
    QString file() const { return m_file; }
    qint64 total() const { return m_total; }
    qint64 done() const { return m_done.load(); }
    bool cancelled() const { return m_cancel.load(); }
    QString error() const { return m_error; }      // valid after finished()
    void cancel() { m_cancel.store(true); }

    QString operation;      // label for the report
    QString blockPath;
    QString device;
    QElapsedTimer clock;

protected:
    void run() override;

private:
    bool m_restore;
    int m_deviceFd;
    QString m_file;
    qint64 m_total;
    std::atomic<qint64> m_done{0};
    std::atomic<bool> m_cancel{false};
    QString m_error;
};
