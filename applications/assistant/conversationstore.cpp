// SPDX-License-Identifier: GPL-3.0-or-later
#include "conversationstore.h"

#include "toolregistry.h"

#include <QDir>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QUuid>

// ============================ MessageModel ============================

MessageModel::MessageModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int MessageModel::rowCount(const QModelIndex &) const
{
    return int(m_messages.size());
}

QHash<int, QByteArray> MessageModel::roleNames() const
{
    return {
        {KindRole, "kind"},
        {ContentRole, "content"},
        {StreamingRole, "streaming"},
        {ToolCallsRole, "toolCalls"},
        {HasToolCallsRole, "hasToolCalls"},
        {ToolNameRole, "toolName"},
    };
}

QVariantList MessageModel::toolCallsVariant(const Message &m) const
{
    QVariantList out;
    for (const ToolCallEntry &c : m.toolCalls) {
        out.append(QVariantMap{
            {QStringLiteral("id"), c.id},
            {QStringLiteral("name"), c.name},
            {QStringLiteral("summary"), c.summary},
            {QStringLiteral("preview"), c.preview},
            {QStringLiteral("status"), c.status},
            {QStringLiteral("result"), c.result},
            {QStringLiteral("risky"), c.risky},
            {QStringLiteral("changesSystem"), c.changesSystem},
        });
    }
    return out;
}

QVariant MessageModel::data(const QModelIndex &index, int role) const
{
    if (index.row() < 0 || index.row() >= m_messages.size())
        return {};
    const Message &m = m_messages.at(index.row());
    switch (role) {
    case KindRole: return m.kind;
    case ContentRole: return m.content;
    case StreamingRole: return m.streaming;
    case ToolCallsRole: return toolCallsVariant(m);
    case HasToolCallsRole: return !m.toolCalls.isEmpty();
    case ToolNameRole: return m.toolName;
    }
    return {};
}

int MessageModel::appendUser(const QString &text)
{
    beginInsertRows({}, m_messages.size(), m_messages.size());
    m_messages.append({QStringLiteral("user"), text, false, {}, {}, {}});
    endInsertRows();
    Q_EMIT countChanged();
    return int(m_messages.size()) - 1;
}

int MessageModel::appendAssistantStreaming()
{
    beginInsertRows({}, m_messages.size(), m_messages.size());
    Message m;
    m.kind = QStringLiteral("assistant");
    m.streaming = true;
    m_messages.append(m);
    endInsertRows();
    Q_EMIT countChanged();
    return int(m_messages.size()) - 1;
}

void MessageModel::appendDelta(int row, const QString &text)
{
    if (row < 0 || row >= m_messages.size())
        return;
    m_messages[row].content += text;
    const QModelIndex idx = index(row);
    Q_EMIT dataChanged(idx, idx, {ContentRole});
}

void MessageModel::finishAssistant(int row, const QString &content, const QJsonArray &toolCalls, ToolRegistry *registry)
{
    if (row < 0 || row >= m_messages.size())
        return;
    Message &m = m_messages[row];
    if (!content.isEmpty())
        m.content = content;
    m.streaming = false;
    m.toolCalls.clear();
    for (const QJsonValue &tcv : toolCalls) {
        const QJsonObject tc = tcv.toObject();
        const QJsonObject fn = tc.value(QStringLiteral("function")).toObject();
        ToolCallEntry e;
        e.id = tc.value(QStringLiteral("id")).toString();
        e.name = fn.value(QStringLiteral("name")).toString();
        e.argumentsJson = fn.value(QStringLiteral("arguments")).toString();
        if (registry) {
            ToolInvocation inv{e.id, e.name, QJsonDocument::fromJson(e.argumentsJson.toUtf8()).object()};
            e.summary = registry->summarize(inv);
            e.preview = registry->preview(inv);
            e.risky = registry->toolRisky(e.name);
            e.changesSystem = registry->toolChangesSystem(e.name);
        }
        m.toolCalls.append(e);
    }
    const QModelIndex idx = index(row);
    Q_EMIT dataChanged(idx, idx, {ContentRole, StreamingRole, ToolCallsRole, HasToolCallsRole});
}

int MessageModel::appendToolResult(const QString &toolCallId, const QString &toolName, const QString &result)
{
    beginInsertRows({}, m_messages.size(), m_messages.size());
    Message m;
    m.kind = QStringLiteral("tool");
    m.content = result;
    m.toolCallId = toolCallId;
    m.toolName = toolName;
    m_messages.append(m);
    endInsertRows();
    Q_EMIT countChanged();
    return int(m_messages.size()) - 1;
}

int MessageModel::appendError(const QString &text)
{
    beginInsertRows({}, m_messages.size(), m_messages.size());
    m_messages.append({QStringLiteral("error"), text, false, {}, {}, {}});
    endInsertRows();
    Q_EMIT countChanged();
    return int(m_messages.size()) - 1;
}

void MessageModel::setToolStatus(int row, const QString &callId, const QString &status, const QString &result)
{
    if (row < 0 || row >= m_messages.size())
        return;
    Message &m = m_messages[row];
    bool changed = false;
    for (ToolCallEntry &c : m.toolCalls) {
        if (c.id == callId) {
            c.status = status;
            if (!result.isNull())
                c.result = result;
            changed = true;
        }
    }
    if (changed) {
        const QModelIndex idx = index(row);
        Q_EMIT dataChanged(idx, idx, {ToolCallsRole});
    }
}

bool MessageModel::hasPendingTools() const
{
    for (const Message &m : m_messages)
        for (const ToolCallEntry &c : m.toolCalls)
            if (c.status == QLatin1String("pending"))
                return true;
    return false;
}

void MessageModel::clearAll()
{
    beginResetModel();
    m_messages.clear();
    endResetModel();
    Q_EMIT countChanged();
}

void MessageModel::loadFrom(const QJsonArray &json)
{
    beginResetModel();
    m_messages.clear();
    for (const QJsonValue &v : json) {
        const QJsonObject o = v.toObject();
        Message m;
        m.kind = o.value(QStringLiteral("kind")).toString();
        m.content = o.value(QStringLiteral("content")).toString();
        m.toolCallId = o.value(QStringLiteral("toolCallId")).toString();
        m.toolName = o.value(QStringLiteral("toolName")).toString();
        for (const QJsonValue &cv : o.value(QStringLiteral("toolCalls")).toArray()) {
            const QJsonObject co = cv.toObject();
            ToolCallEntry e;
            e.id = co.value(QStringLiteral("id")).toString();
            e.name = co.value(QStringLiteral("name")).toString();
            e.argumentsJson = co.value(QStringLiteral("argumentsJson")).toString();
            e.summary = co.value(QStringLiteral("summary")).toString();
            e.preview = co.value(QStringLiteral("preview")).toString();
            e.status = co.value(QStringLiteral("status")).toString();
            e.result = co.value(QStringLiteral("result")).toString();
            e.risky = co.value(QStringLiteral("risky")).toBool();
            e.changesSystem = co.value(QStringLiteral("changesSystem")).toBool();
            m.toolCalls.append(e);
        }
        m_messages.append(m);
    }
    endResetModel();
    Q_EMIT countChanged();
}

QJsonArray MessageModel::toJson() const
{
    QJsonArray out;
    for (const Message &m : m_messages) {
        QJsonObject o{
            {QStringLiteral("kind"), m.kind},
            {QStringLiteral("content"), m.content},
        };
        if (!m.toolCallId.isEmpty())
            o.insert(QStringLiteral("toolCallId"), m.toolCallId);
        if (!m.toolName.isEmpty())
            o.insert(QStringLiteral("toolName"), m.toolName);
        if (!m.toolCalls.isEmpty()) {
            QJsonArray calls;
            for (const ToolCallEntry &c : m.toolCalls)
                calls.append(QJsonObject{
                    {QStringLiteral("id"), c.id}, {QStringLiteral("name"), c.name},
                    {QStringLiteral("argumentsJson"), c.argumentsJson}, {QStringLiteral("summary"), c.summary},
                    {QStringLiteral("preview"), c.preview}, {QStringLiteral("status"), c.status},
                    {QStringLiteral("result"), c.result}, {QStringLiteral("risky"), c.risky},
                    {QStringLiteral("changesSystem"), c.changesSystem},
                });
            o.insert(QStringLiteral("toolCalls"), calls);
        }
        out.append(o);
    }
    return out;
}

QJsonArray MessageModel::apiMessages(const QString &systemPrompt) const
{
    QJsonArray out;
    if (!systemPrompt.isEmpty())
        out.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), systemPrompt}});
    for (const Message &m : m_messages) {
        if (m.kind == QLatin1String("error"))
            continue; // local-only
        if (m.kind == QLatin1String("user")) {
            out.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), m.content}});
        } else if (m.kind == QLatin1String("assistant")) {
            QJsonObject o{{QStringLiteral("role"), QStringLiteral("assistant")}, {QStringLiteral("content"), m.content}};
            if (!m.toolCalls.isEmpty()) {
                QJsonArray calls;
                for (const ToolCallEntry &c : m.toolCalls) {
                    // Only include calls that were actually run (not denied) so the
                    // conversation the model sees stays consistent with tool results.
                    if (c.status == QLatin1String("denied"))
                        continue;
                    calls.append(QJsonObject{
                        {QStringLiteral("id"), c.id}, {QStringLiteral("type"), QStringLiteral("function")},
                        {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), c.name},
                                                                 {QStringLiteral("arguments"), c.argumentsJson}}},
                    });
                }
                if (!calls.isEmpty())
                    o.insert(QStringLiteral("tool_calls"), calls);
            }
            out.append(o);
        } else if (m.kind == QLatin1String("tool")) {
            out.append(QJsonObject{
                {QStringLiteral("role"), QStringLiteral("tool")},
                {QStringLiteral("tool_call_id"), m.toolCallId},
                {QStringLiteral("content"), m.content},
            });
        }
    }
    return out;
}

// ============================ ConversationStore ============================

ConversationStore::ConversationStore(QObject *parent)
    : QAbstractListModel(parent)
{
    QDir().mkpath(storageDir());
    load();
}

QString ConversationStore::storageDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

QString ConversationStore::pathFor(const QString &id) const
{
    return storageDir() + QLatin1Char('/') + id + QStringLiteral(".json");
}

int ConversationStore::rowCount(const QModelIndex &) const
{
    return int(m_items.size());
}

QHash<int, QByteArray> ConversationStore::roleNames() const
{
    return {{IdRole, "id"}, {TitleRole, "title"}, {UpdatedRole, "updated"}, {SubtitleRole, "subtitle"}};
}

QVariant ConversationStore::data(const QModelIndex &index, int role) const
{
    if (index.row() < 0 || index.row() >= m_items.size())
        return {};
    const Conversation &c = m_items.at(index.row());
    switch (role) {
    case IdRole: return c.id;
    case TitleRole: return c.title;
    case UpdatedRole: return c.updated;
    case SubtitleRole: return c.updated.toString(QStringLiteral("MMM d, HH:mm"));
    }
    return {};
}

void ConversationStore::sortByUpdated()
{
    std::sort(m_items.begin(), m_items.end(), [](const Conversation &a, const Conversation &b) {
        return a.updated > b.updated;
    });
}

void ConversationStore::load()
{
    beginResetModel();
    m_items.clear();
    const QDir dir(storageDir());
    const QStringList files = dir.entryList({QStringLiteral("*.json")}, QDir::Files);
    for (const QString &file : files) {
        QFile f(dir.filePath(file));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        Conversation c;
        c.id = o.value(QStringLiteral("id")).toString(QFileInfo(file).completeBaseName());
        c.title = o.value(QStringLiteral("title")).toString();
        c.updated = QDateTime::fromString(o.value(QStringLiteral("updated")).toString(), Qt::ISODate);
        if (c.title.isEmpty())
            c.title = QStringLiteral("Conversation");
        if (!c.updated.isValid())
            c.updated = QFileInfo(dir.filePath(file)).lastModified();
        m_items.append(c);
    }
    sortByUpdated();
    endResetModel();
    Q_EMIT countChanged();
}

int ConversationStore::indexOfId(const QString &id) const
{
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items.at(i).id == id)
            return i;
    return -1;
}

QString ConversationStore::titleOf(const QString &id) const
{
    const int i = indexOfId(id);
    return i >= 0 ? m_items.at(i).title : QString();
}

QString ConversationStore::createConversation()
{
    Conversation c;
    c.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    c.title = tr("New conversation");
    c.updated = QDateTime::currentDateTime();
    writeMeta(c, {});
    beginResetModel();
    m_items.prepend(c);
    sortByUpdated();
    endResetModel();
    Q_EMIT countChanged();
    return c.id;
}

void ConversationStore::writeMeta(const Conversation &c, const QJsonArray &messages)
{
    QFile f(pathFor(c.id));
    QJsonObject o{
        {QStringLiteral("id"), c.id},
        {QStringLiteral("title"), c.title},
        {QStringLiteral("updated"), c.updated.toString(Qt::ISODate)},
        {QStringLiteral("messages"), messages},
    };
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
        f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    }
}

void ConversationStore::renameConversation(const QString &id, const QString &title)
{
    const int i = indexOfId(id);
    if (i < 0 || title.trimmed().isEmpty())
        return;
    m_items[i].title = title.trimmed();
    // Preserve messages when rewriting metadata.
    writeMeta(m_items[i], loadMessages(id));
    const QModelIndex idx = index(i);
    Q_EMIT dataChanged(idx, idx, {TitleRole});
}

void ConversationStore::removeConversation(const QString &id)
{
    const int i = indexOfId(id);
    if (i < 0)
        return;
    QFile::remove(pathFor(id));
    beginRemoveRows({}, i, i);
    m_items.removeAt(i);
    endRemoveRows();
    Q_EMIT countChanged();
}

void ConversationStore::touch(const QString &id, const QString &derivedTitle)
{
    const int i = indexOfId(id);
    if (i < 0)
        return;
    m_items[i].updated = QDateTime::currentDateTime();
    if (!derivedTitle.trimmed().isEmpty()
        && (m_items[i].title.isEmpty() || m_items[i].title == tr("New conversation")))
        m_items[i].title = derivedTitle.trimmed().left(60);
    beginResetModel();
    sortByUpdated();
    endResetModel();
}

QJsonArray ConversationStore::loadMessages(const QString &id) const
{
    QFile f(pathFor(id));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("messages")).toArray();
}

void ConversationStore::saveMessages(const QString &id, const QJsonArray &messages)
{
    const int i = indexOfId(id);
    if (i < 0)
        return;
    m_items[i].updated = QDateTime::currentDateTime();
    writeMeta(m_items[i], messages);
}
