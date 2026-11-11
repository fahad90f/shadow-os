// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QtConcurrentRun>

#include <type_traits>

namespace ShadowPk {

// Run a blocking function on the global thread pool and deliver its result to
// 'done' on the context object's thread. Nothing is delivered if the context
// is destroyed first.
template <typename Function, typename Done>
void runAsync(QObject *context, Function function, Done done)
{
    using Result = std::invoke_result_t<Function>;
    auto *watcher = new QFutureWatcher<Result>(context);
    QObject::connect(watcher, &QFutureWatcher<Result>::finished, context, [watcher, done]() mutable {
        done(watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run(std::move(function)));
}

} // namespace ShadowPk
