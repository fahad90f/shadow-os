// SPDX-License-Identifier: GPL-3.0-or-later
#include "imageworker.h"

#include <QFile>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr size_t kChunk = 1024 * 1024;

bool writeAll(int fd, const char *data, size_t size)
{
    size_t off = 0;
    while (off < size) {
        const ssize_t n = ::write(fd, data + off, size - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        off += size_t(n);
    }
    return true;
}

bool allZero(const char *data, size_t size)
{
    return size > 0 && data[0] == 0 && std::memcmp(data, data + 1, size - 1) == 0;
}
} // namespace

ImageWorker::ImageWorker(bool restore, int deviceFd, const QString &file, qint64 total, QObject *parent)
    : QThread(parent), m_restore(restore), m_deviceFd(deviceFd), m_file(file), m_total(total)
{
}

ImageWorker::~ImageWorker()
{
    cancel();
    wait();
    if (m_deviceFd >= 0)
        ::close(m_deviceFd);
}

void ImageWorker::run()
{
    std::vector<char> buffer(kChunk);
    const QString partial = m_file + QStringLiteral(".part");
    const int fileFd = m_restore
        ? ::open(QFile::encodeName(m_file).constData(), O_RDONLY | O_CLOEXEC)
        : ::open(QFile::encodeName(partial).constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fileFd < 0) {
        m_error = tr("%1 could not be opened: %2").arg(m_restore ? m_file : partial, QString::fromLocal8Bit(std::strerror(errno)));
        ::close(m_deviceFd);
        m_deviceFd = -1;
        return;
    }
    const int in = m_restore ? fileFd : m_deviceFd;
    const int out = m_restore ? m_deviceFd : fileFd;

    while (!m_cancel.load() && m_done.load() < m_total) {
        const size_t want = size_t(qMin<qint64>(qint64(kChunk), m_total - m_done.load()));
        const ssize_t n = ::read(in, buffer.data(), want);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            m_error = tr("Reading failed after %1 bytes: %2").arg(m_done.load()).arg(QString::fromLocal8Bit(std::strerror(errno)));
            break;
        }
        if (n == 0)
            break;      // source ended early; reported below
        bool ok;
        if (!m_restore && allZero(buffer.data(), size_t(n))) {
            // Keep the image file sparse: holes instead of blocks of zeros.
            ok = ::lseek(out, n, SEEK_CUR) >= 0;
        } else {
            ok = writeAll(out, buffer.data(), size_t(n));
        }
        if (!ok) {
            m_error = tr("Writing failed after %1 bytes: %2").arg(m_done.load()).arg(QString::fromLocal8Bit(std::strerror(errno)));
            break;
        }
        m_done.fetch_add(n);
    }

    if (m_error.isEmpty() && !m_cancel.load() && m_done.load() != m_total)
        m_error = tr("The source ended after %1 of %2 bytes.").arg(m_done.load()).arg(m_total);
    if (m_error.isEmpty() && !m_cancel.load()) {
        if (!m_restore && ::ftruncate(out, m_total) != 0)
            m_error = tr("The image file could not be completed: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        else if (::fsync(out) != 0)
            m_error = tr("The data could not be flushed to the target: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
    }
    ::close(fileFd);
    ::close(m_deviceFd);
    m_deviceFd = -1;

    if (!m_restore) {
        // An incomplete image is never left behind under the final name.
        if (m_error.isEmpty() && !m_cancel.load()) {
            if (::rename(QFile::encodeName(partial).constData(), QFile::encodeName(m_file).constData()) != 0)
                m_error = tr("The finished image could not be renamed to %1: %2").arg(m_file, QString::fromLocal8Bit(std::strerror(errno)));
        } else {
            QFile::remove(partial);
        }
    }
}
