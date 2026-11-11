// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

namespace StatusModule {

// What every status back-end has in common: it gathers its data
// asynchronously and says honestly whether the service or hardware behind it
// exists.
//
//   ready       the first query has finished (successfully or not); until
//               then nothing is known
//   available   the service/hardware is there and the values are real
//   unavailableReason   why not, for diagnostics (--dump status)
class Backend : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool ready READ isReady NOTIFY readyChanged)
    Q_PROPERTY(bool available READ isAvailable NOTIFY availabilityChanged)
    Q_PROPERTY(QString unavailableReason READ unavailableReason NOTIFY availabilityChanged)

public:
    explicit Backend(QObject *parent = nullptr);

    bool isReady() const { return m_ready; }
    bool isAvailable() const { return m_available; }
    QString unavailableReason() const { return m_available ? QString() : m_reason; }

    // { "available": …, "reason": … (when unavailable), <details()> }
    QJsonObject toJson() const;

Q_SIGNALS:
    void readyChanged();
    void availabilityChanged();
    // Something the user asked for could not be done; 'message' is shown as is.
    void failed(const QString &message);

protected:
    // The back-end's current values for --dump status.
    virtual QJsonObject details() const = 0;

    // Records the outcome of a query; the first call also makes the back-end ready.
    void setAvailable(bool available, const QString &reason = QString());

private:
    bool m_ready = false;
    bool m_available = false;
    QString m_reason;
};

} // namespace StatusModule
