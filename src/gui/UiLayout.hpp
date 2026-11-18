#pragma once
#include "ThemeManager.hpp"
#include <QGridLayout>
#include <QLayout>
#include <QList>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSplitter>
#include <algorithm>

namespace tsui {

class FlowLayout final : public QLayout {
    QList<QLayoutItem*> items_;

    int arrange(const QRect& rect, bool measure) const {
        const auto m = contentsMargins();
        const QRect area = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
        int x = area.x(), y = area.y(), lineHeight = 0;
        const int gap = std::max(0, spacing());
        for (auto* item : items_) {
            if (item->isEmpty())
                continue;
            const QSize hint = item->sizeHint();
            const int width = std::min(hint.width(), area.width());
            const int height = item->hasHeightForWidth() ? item->heightForWidth(width) : hint.height();
            if (x > area.x() && x + width > area.right() + 1) {
                x = area.x();
                y += lineHeight + gap;
                lineHeight = 0;
            }
            if (!measure)
                item->setGeometry(QRect(x, y, width, height));
            x += width + gap;
            lineHeight = std::max(lineHeight, height);
        }
        return y + lineHeight - rect.y() + m.bottom();
    }

  public:
    explicit FlowLayout(QWidget* parent = nullptr) : QLayout(parent) {
        setContentsMargins(0, 0, 0, 0);
        setSpacing(8);
    }

    ~FlowLayout() override {
        while (auto* item = takeAt(0))
            delete item;
    }

    void addItem(QLayoutItem* item) override {
        items_.append(item);
    }

    int count() const override {
        return items_.size();
    }

    QLayoutItem* itemAt(int index) const override {
        return items_.value(index);
    }

    QLayoutItem* takeAt(int index) override {
        return index >= 0 && index < items_.size() ? items_.takeAt(index) : nullptr;
    }

    Qt::Orientations expandingDirections() const override {
        return {};
    }

    bool hasHeightForWidth() const override {
        return true;
    }

    int heightForWidth(int width) const override {
        return arrange(QRect(0, 0, width, 0), true);
    }

    QSize minimumSize() const override {
        QSize size;
        for (auto* item : items_)
            if (!item->isEmpty())
                size = size.expandedTo(item->minimumSize());
        const auto m = contentsMargins();
        return size + QSize(m.left() + m.right(), m.top() + m.bottom());
    }

    QSize sizeHint() const override {
        QSize size;
        for (auto* item : items_)
            if (!item->isEmpty()) {
                size.rwidth() += item->sizeHint().width() + spacing();
                size.setHeight(std::max(size.height(), item->sizeHint().height()));
            }
        return size.expandedTo(minimumSize());
    }

    void setGeometry(const QRect& rect) override {
        QLayout::setGeometry(rect);
        arrange(rect, false);
    }

    void addStretch(int = 0) {
    }

    void addSpacing(int) {
    }
};

class MetricGrid final : public QWidget {
    QGridLayout* grid_ = new QGridLayout(this);
    QList<QWidget*> cards_;
    int columns_ = 0;

    void arrange() {
        const int columns = width() < qRound(740 * ThemeManager::scale()) ? 2 : 4;
        if (columns == columns_)
            return;
        columns_ = columns;
        for (auto* card : cards_)
            grid_->removeWidget(card);
        for (int i = 0; i < cards_.size(); ++i)
            grid_->addWidget(cards_[i], i / columns, i % columns);
        for (int i = 0; i < 4; ++i)
            grid_->setColumnStretch(i, i < columns ? 1 : 0);
    }

  public:
    MetricGrid() {
        grid_->setContentsMargins(0, 0, 0, 0);
        grid_->setSpacing(12);
    }

    void addWidget(QWidget* widget) {
        cards_.append(widget);
        columns_ = 0;
        arrange();
    }

    void setSpacing(int spacing) {
        grid_->setSpacing(spacing);
    }

  protected:
    void resizeEvent(QResizeEvent* event) override {
        arrange();
        QWidget::resizeEvent(event);
    }
};

inline QScrollArea* scrollPage(QWidget* page, const QString& name) {
    auto* scroll = new QScrollArea;
    scroll->setObjectName(name);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    page->setObjectName(name + "Content");
    page->setMinimumWidth(540);
    page->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    if (!page->layout()) {
        auto* container = new QWidget;
        auto* layout = new QVBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(page);
        page = container;
    }
    page->layout()->setSizeConstraint(QLayout::SetMinimumSize);
    scroll->setWidget(page);
    return scroll;
}

class AdaptiveSplitter final : public QSplitter {
  public:
    explicit AdaptiveSplitter(QWidget* parent = nullptr) : QSplitter(Qt::Horizontal, parent) {
        setChildrenCollapsible(false);
        setHandleWidth(8);
    }

  protected:
    void resizeEvent(QResizeEvent* event) override {
        const auto desired = width() < qRound(850 * ThemeManager::scale()) ? Qt::Vertical : Qt::Horizontal;
        if (orientation() != desired) {
            setOrientation(desired);
            setSizes(desired == Qt::Vertical ? QList<int>{440, 300} : QList<int>{700, 340});
        }
        QSplitter::resizeEvent(event);
    }
};
}
