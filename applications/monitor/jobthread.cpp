// SPDX-License-Identifier: GPL-3.0-or-later
#include "jobthread.h"

#include <QThread>

JobThread::JobThread(const QString &name, QObject *parent)
    : QObject(parent)
    , m_shared(std::make_shared<Shared>())
    , m_thread(new QThread)
    , m_context(new QObject)
{
    m_shared->owner = this;
    m_thread->setObjectName(name);
    m_context->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_context, &QObject::deleteLater);
    m_thread->start();
}

JobThread::~JobThread()
{
    {
        const QMutexLocker lock(&m_shared->mutex);
        m_shared->owner = nullptr;
    }
    m_thread->quit();
    if (m_thread->wait(1500)) {
        delete m_thread;            // m_context was deleted when the thread finished
        return;
    }
    // The running job sits in a system call that cannot be interrupted (for
    // example statvfs() on a dead network mount). Destroying a running QThread
    // aborts the process, so the thread object is left to the operating
    // system, which reclaims it when the process exits.
}
