// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QMutex>
#include <QObject>
#include <QString>

#include <memory>

class QThread;

// Runs blocking jobs on a private thread, one at a time, and hands each
// result back on the owner's thread.
//
// Everything the monitor reads can stall for reasons outside its control —
// an embedded-controller sensor, statvfs() on a network mount whose server
// went away, a name-service look-up — so none of it runs on the UI thread.
// A job that is still running when the next one is due simply makes that
// request a no-op; samples never pile up.
class JobThread : public QObject
{
    Q_OBJECT

public:
    explicit JobThread(const QString &name, QObject *parent = nullptr);
    ~JobThread() override;

    bool busy() const { return m_busy; }

    // Run job() on the worker thread, then done(result) on this object's
    // thread. Returns false, doing nothing, while the previous job is running.
    // Both callables are copied; they must not capture anything that dies
    // before the job ends (capture shared state by std::shared_ptr).
    template <typename Job, typename Done>
    bool run(Job job, Done done)
    {
        if (m_busy)
            return false;
        m_busy = true;
        const std::shared_ptr<Shared> shared = m_shared;
        QMetaObject::invokeMethod(m_context, [shared, job, done] {
            const auto result = job();
            const QMutexLocker lock(&shared->mutex);
            JobThread *owner = shared->owner;
            if (!owner)
                return;             // the owner is gone; nobody wants the result
            QMetaObject::invokeMethod(owner, [owner, done, result] {
                owner->m_busy = false;
                done(result);
            }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
        return true;
    }

private:
    struct Shared
    {
        QMutex mutex;
        JobThread *owner = nullptr;
    };

    std::shared_ptr<Shared> m_shared;
    QThread *m_thread = nullptr;
    QObject *m_context = nullptr;   // lives in m_thread; jobs are posted to it
    bool m_busy = false;
};
