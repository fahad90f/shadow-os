// SPDX-License-Identifier: GPL-3.0-or-later
#include "terminaltab.h"

#include "tokens.h"

#include <QJsonArray>
#include <QShowEvent>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

#include <climits>

namespace {

// Guards against a damaged settings file asking for an absurd layout.
constexpr int kMaxRestoreDepth = 8;
constexpr int kMaxPanesPerTab = 16;
const char kSavedSizes[] = "nxSavedSizes";

void collectPanes(QWidget *node, QList<TerminalPane *> *out)
{
    if (auto *pane = qobject_cast<TerminalPane *>(node)) {
        out->append(pane);
    } else if (auto *splitter = qobject_cast<QSplitter *>(node)) {
        for (int i = 0; i < splitter->count(); ++i)
            collectPanes(splitter->widget(i), out);
    }
}

QList<int> toIntList(const QVariant &value)
{
    QList<int> list;
    const QVariantList items = value.toList();
    for (const QVariant &item : items)
        list.append(item.toInt());
    return list;
}

} // namespace

TerminalTab::TerminalTab(QWidget *parent)
    : QWidget(parent)
    , m_layout(new QVBoxLayout(this))
{
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);
}

TerminalPane *TerminalTab::createPane(const PaneSpec &spec)
{
    auto *pane = new TerminalPane(spec, this);
    connect(pane, &TerminalPane::activated, this, &TerminalTab::onPaneActivated);
    connect(pane, &TerminalPane::titleChanged, this, [this](TerminalPane *p) {
        if (p == m_active)
            Q_EMIT titleChanged();
    });
    connect(pane, &TerminalPane::finished, this, &TerminalTab::closePane);
    connect(pane, &TerminalPane::closeRequested, this, &TerminalTab::closePane);
    connect(pane, &TerminalPane::bellRang, this, &TerminalTab::onPaneBell);
    connect(pane, &TerminalPane::contextMenuRequested, this, &TerminalTab::contextMenuRequested);
    connect(pane, &TerminalPane::notice, this, &TerminalTab::notice);
    connect(pane, &TerminalPane::zoomChanged, this, &TerminalTab::zoomChanged);
    return pane;
}

QSplitter *TerminalTab::createSplitter(Qt::Orientation orientation)
{
    auto *splitter = new QSplitter(orientation, this);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(Tokens::SpaceXs);
    splitter->setOpaqueResize(true);
    return splitter;
}

TerminalPane *TerminalTab::setFirstPane(const PaneSpec &spec)
{
    Q_ASSERT(panes().isEmpty());
    TerminalPane *pane = createPane(spec);
    m_layout->addWidget(pane);
    m_active = pane;
    updatePaneFrames();
    pane->start();
    Q_EMIT paneCountChanged();
    Q_EMIT activePaneChanged(pane);
    Q_EMIT titleChanged();
    return pane;
}

TerminalPane *TerminalTab::split(TerminalPane *pane, Qt::Orientation orientation, const PaneSpec &spec)
{
    if (!pane || !panes().contains(pane))
        return nullptr;

    TerminalPane *fresh = createPane(spec);
    auto *parentSplitter = qobject_cast<QSplitter *>(pane->parentWidget());

    if (parentSplitter && parentSplitter->orientation() == orientation) {
        // Same direction as the surrounding split: the pane shares its space
        // with a new sibling.
        const int index = parentSplitter->indexOf(pane);
        QList<int> sizes = parentSplitter->sizes();
        parentSplitter->insertWidget(index + 1, fresh);
        const int half = sizes.value(index) / 2;
        if (index < sizes.size()) {
            sizes[index] -= half;
            sizes.insert(index + 1, half);
            parentSplitter->setSizes(sizes);
        }
    } else {
        // Otherwise the pane is replaced by a splitter holding it and the new one.
        QSplitter *splitter = createSplitter(orientation);
        // The splitter takes over the pane's place and size at once, so the
        // halves are computed from the real extent and not from the size of
        // a widget that has not been laid out yet.
        const QSize paneSize = pane->size();
        splitter->resize(paneSize);
        if (parentSplitter) {
            const int index = parentSplitter->indexOf(pane);
            const QList<int> sizes = parentSplitter->sizes();
            parentSplitter->insertWidget(index, splitter);
            splitter->addWidget(pane);
            splitter->addWidget(fresh);
            parentSplitter->setSizes(sizes);
        } else {
            m_layout->removeWidget(pane);
            splitter->addWidget(pane);
            splitter->addWidget(fresh);
            m_layout->addWidget(splitter);
        }
        const auto halve = [splitter](int extent) {
            const int usable = qMax(2, extent - splitter->handleWidth());
            splitter->setSizes({usable / 2, usable - usable / 2});
        };
        halve(orientation == Qt::Horizontal ? paneSize.width() : paneSize.height());
        splitter->show();
        pane->show();
        // Once more when the layout has settled (the first pass can still be
        // skewed by the minimum sizes of widgets that were not shown yet).
        QTimer::singleShot(0, splitter, [splitter, halve] {
            halve(splitter->orientation() == Qt::Horizontal ? splitter->width() : splitter->height());
        });
    }
    fresh->show();

    updatePaneFrames();
    fresh->start();
    Q_EMIT paneCountChanged();
    return fresh;
}

void TerminalTab::closePane(TerminalPane *pane)
{
    const QList<TerminalPane *> before = panes();
    const qsizetype position = before.indexOf(pane);
    if (position < 0)
        return;

    // Focus moves to the pane before the closed one (or the next, for the first).
    TerminalPane *successor = nullptr;
    if (before.size() > 1)
        successor = before.at(position > 0 ? position - 1 : 1);
    const bool wasActive = pane == m_active;

    auto *parentSplitter = qobject_cast<QSplitter *>(pane->parentWidget());
    if (!parentSplitter)
        m_layout->removeWidget(pane);
    pane->dispose();

    if (parentSplitter && parentSplitter->count() == 1) {
        // A splitter left with one child gives its place to that child.
        QWidget *remaining = parentSplitter->widget(0);
        if (auto *grand = qobject_cast<QSplitter *>(parentSplitter->parentWidget())) {
            const int index = grand->indexOf(parentSplitter);
            const QList<int> sizes = grand->sizes();
            grand->insertWidget(index, remaining);
            parentSplitter->hide();
            parentSplitter->setParent(nullptr);
            grand->setSizes(sizes);
        } else {
            m_layout->removeWidget(parentSplitter);
            m_layout->addWidget(remaining);
            parentSplitter->hide();
            parentSplitter->setParent(nullptr);
        }
        remaining->show();
        parentSplitter->deleteLater();
    }

    if (before.size() == 1) {
        m_active.clear();
        Q_EMIT paneCountChanged();
        Q_EMIT emptied();
        return;
    }

    if (wasActive || !m_active) {
        m_active = successor;
        updatePaneFrames();
        if (successor) {
            successor->focusTerminal();
            Q_EMIT activePaneChanged(successor);
            Q_EMIT titleChanged();
        }
    } else {
        updatePaneFrames();
    }
    Q_EMIT paneCountChanged();
}

QList<TerminalPane *> TerminalTab::panes() const
{
    QList<TerminalPane *> list;
    if (QLayoutItem *item = m_layout->itemAt(0))
        collectPanes(item->widget(), &list);
    return list;
}

TerminalPane *TerminalTab::activePane() const
{
    if (m_active)
        return m_active;
    const QList<TerminalPane *> all = panes();
    return all.isEmpty() ? nullptr : all.first();
}

void TerminalTab::setActivePane(TerminalPane *pane)
{
    if (!pane || pane == m_active)
        return;
    m_active = pane;
    updatePaneFrames();
    Q_EMIT activePaneChanged(pane);
    Q_EMIT titleChanged();
}

void TerminalTab::onPaneActivated(TerminalPane *pane)
{
    setActivePane(pane);
}

void TerminalTab::updatePaneFrames()
{
    const QList<TerminalPane *> all = panes();
    for (TerminalPane *pane : all) {
        pane->setSingle(all.size() == 1);
        pane->setActive(pane == m_active);
    }
}

TerminalPane *TerminalTab::neighbour(Direction direction) const
{
    const TerminalPane *from = activePane();
    if (!from)
        return nullptr;
    const QRect a(from->mapTo(this, QPoint(0, 0)), from->size());

    TerminalPane *best = nullptr;
    int bestDistance = INT_MAX;
    int bestOverlap = 0;
    const QList<TerminalPane *> all = panes();
    for (TerminalPane *pane : all) {
        if (pane == from)
            continue;
        const QRect b(pane->mapTo(this, QPoint(0, 0)), pane->size());
        const int overlapY = qMin(a.bottom(), b.bottom()) - qMax(a.top(), b.top());
        const int overlapX = qMin(a.right(), b.right()) - qMax(a.left(), b.left());
        int distance = -1;
        int overlap = 0;
        switch (direction) {
        case Direction::Left:
            distance = a.left() - b.right();
            overlap = overlapY;
            break;
        case Direction::Right:
            distance = b.left() - a.right();
            overlap = overlapY;
            break;
        case Direction::Up:
            distance = a.top() - b.bottom();
            overlap = overlapX;
            break;
        case Direction::Down:
            distance = b.top() - a.bottom();
            overlap = overlapX;
            break;
        }
        // Only panes that lie in that direction and share an edge range.
        if (distance < 0 || overlap <= 0)
            continue;
        if (distance < bestDistance || (distance == bestDistance && overlap > bestOverlap)) {
            best = pane;
            bestDistance = distance;
            bestOverlap = overlap;
        }
    }
    return best;
}

bool TerminalTab::focusNeighbour(Direction direction)
{
    TerminalPane *target = neighbour(direction);
    if (!target)
        return false;
    target->focusTerminal();
    setActivePane(target);
    return true;
}

void TerminalTab::setCustomTitle(const QString &title)
{
    const QString clean = title.simplified();
    if (clean == m_customTitle)
        return;
    m_customTitle = clean;
    Q_EMIT titleChanged();
}

QString TerminalTab::title() const
{
    if (!m_customTitle.isEmpty())
        return m_customTitle;
    const TerminalPane *pane = activePane();
    const QString fromPane = pane ? pane->title() : QString();
    return fromPane.isEmpty() ? tr("Terminal") : fromPane;
}

QString TerminalTab::toolTipText() const
{
    const TerminalPane *pane = activePane();
    if (!pane)
        return title();
    QStringList lines{title()};
    if (pane->isRunning()) {
        const QString process = pane->busyDescription();
        if (!process.isEmpty())
            lines << tr("Process: %1").arg(process);
    } else {
        lines << (pane->hasStarted() ? tr("The program has ended") : tr("Not running"));
    }
    lines << tr("Directory: %1").arg(pane->displayDirectory());
    lines << tr("Profile: %1").arg(pane->profile().name);
    const int count = paneCount();
    if (count > 1)
        lines << tr("%n panes", nullptr, count);
    return lines.join(QLatin1Char('\n'));
}

bool TerminalTab::isSsh() const
{
    const TerminalPane *pane = activePane();
    return pane && pane->spec().kind == PaneSpec::Kind::Ssh;
}

void TerminalTab::onPaneBell(TerminalPane *)
{
    // Only a tab the user is not looking at needs a mark.
    if (isVisible() || m_bell)
        return;
    m_bell = true;
    Q_EMIT bellChanged();
}

void TerminalTab::clearBell()
{
    if (!m_bell)
        return;
    m_bell = false;
    Q_EMIT bellChanged();
}

// ---- session ----------------------------------------------------------------------

QJsonObject TerminalTab::saveNode(const QWidget *node) const
{
    if (const auto *splitter = qobject_cast<const QSplitter *>(node)) {
        QJsonArray children;
        for (int i = 0; i < splitter->count(); ++i)
            children.append(saveNode(splitter->widget(i)));
        // A restored splitter that was never shown has no real sizes yet;
        // keep the ones it was restored with.
        QList<int> sizes = splitter->sizes();
        const bool laidOut = std::any_of(sizes.cbegin(), sizes.cend(), [](int s) { return s > 0; });
        if (!laidOut && splitter->property(kSavedSizes).isValid())
            sizes = toIntList(splitter->property(kSavedSizes));
        QJsonArray sizeArray;
        for (const int s : std::as_const(sizes))
            sizeArray.append(s);
        return QJsonObject{
            {QStringLiteral("type"), QStringLiteral("split")},
            {QStringLiteral("orientation"), splitter->orientation() == Qt::Horizontal
                                                ? QStringLiteral("horizontal") : QStringLiteral("vertical")},
            {QStringLiteral("sizes"), sizeArray},
            {QStringLiteral("children"), children},
        };
    }
    if (const auto *pane = qobject_cast<const TerminalPane *>(node)) {
        return QJsonObject{
            {QStringLiteral("type"), QStringLiteral("pane")},
            {QStringLiteral("profile"), pane->spec().profileId},
            {QStringLiteral("directory"), pane->workingDirectory()},
            {QStringLiteral("active"), pane == m_active},
        };
    }
    return QJsonObject();
}

QJsonObject TerminalTab::saveLayout() const
{
    QJsonObject tab;
    tab.insert(QStringLiteral("title"), m_customTitle);
    if (const QLayoutItem *item = m_layout->itemAt(0))
        tab.insert(QStringLiteral("layout"), saveNode(item->widget()));
    return tab;
}

QWidget *TerminalTab::restoreNode(const QJsonObject &node,
                                  const std::function<PaneSpec(const QJsonObject &)> &makeSpec,
                                  TerminalPane **active, int depth, int *budget)
{
    const QString type = node.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("pane")) {
        if (*budget <= 0)
            return nullptr;
        --*budget;
        TerminalPane *pane = createPane(makeSpec(node));
        if (node.value(QStringLiteral("active")).toBool())
            *active = pane;
        return pane;
    }
    if (type != QLatin1String("split") || depth >= kMaxRestoreDepth)
        return nullptr;

    const QJsonArray children = node.value(QStringLiteral("children")).toArray();
    const Qt::Orientation orientation = node.value(QStringLiteral("orientation")).toString()
            == QLatin1String("vertical") ? Qt::Vertical : Qt::Horizontal;
    QSplitter *splitter = createSplitter(orientation);
    for (const QJsonValue &child : children) {
        if (QWidget *widget = restoreNode(child.toObject(), makeSpec, active, depth + 1, budget))
            splitter->addWidget(widget);
    }
    if (splitter->count() == 0) {
        delete splitter;
        return nullptr;
    }
    if (splitter->count() == 1) {
        QWidget *only = splitter->widget(0);
        only->setParent(this);
        delete splitter;
        return only;
    }

    QVariantList sizes;
    int total = 0;
    const QJsonArray saved = node.value(QStringLiteral("sizes")).toArray();
    for (const QJsonValue &value : saved) {
        const int size = qMax(0, value.toInt());
        sizes.append(size);
        total += size;
    }
    if (sizes.size() == splitter->count() && total > 0) {
        splitter->setProperty(kSavedSizes, sizes);
        splitter->setSizes(toIntList(sizes));
    }
    return splitter;
}

bool TerminalTab::restoreLayout(const QJsonObject &tab,
                                const std::function<PaneSpec(const QJsonObject &)> &makeSpec)
{
    Q_ASSERT(panes().isEmpty());
    TerminalPane *active = nullptr;
    int budget = kMaxPanesPerTab;
    QWidget *root = restoreNode(tab.value(QStringLiteral("layout")).toObject(), makeSpec, &active, 0, &budget);
    if (!root)
        return false;
    m_layout->addWidget(root);
    m_customTitle = tab.value(QStringLiteral("title")).toString().simplified();

    const QList<TerminalPane *> all = panes();
    m_active = active ? active : all.first();
    updatePaneFrames();
    for (TerminalPane *pane : all)
        pane->start();
    Q_EMIT paneCountChanged();
    Q_EMIT activePaneChanged(m_active);
    Q_EMIT titleChanged();
    return true;
}

void TerminalTab::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // Sizes of a restored layout are applied again once the tab has its real
    // size; before that a splitter can only keep proportions approximately.
    QTimer::singleShot(0, this, [this] {
        const QList<QSplitter *> splitters = findChildren<QSplitter *>();
        for (QSplitter *splitter : splitters) {
            const QVariant saved = splitter->property(kSavedSizes);
            if (!saved.isValid())
                continue;
            const QList<int> sizes = toIntList(saved);
            if (sizes.size() == splitter->count())
                splitter->setSizes(sizes);
            splitter->setProperty(kSavedSizes, QVariant());
        }
    });
}
