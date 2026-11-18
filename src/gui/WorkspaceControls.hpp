#pragma once
#include <QAccessibleWidget>
#include <QApplication>
#include <QCursor>
#include <QEnterEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>

inline QAccessibleInterface* workspaceButtonAccessibility(const QString&, QObject*);

class WorkspaceToolButton : public QToolButton {
  public:
    explicit WorkspaceToolButton(QWidget* parent = nullptr) : QToolButton(parent) {
        static const bool registered = []() {
            QAccessible::installFactory(workspaceButtonAccessibility);
            return true;
        }();
        (void)registered;
        setFocusPolicy(Qt::StrongFocus);
    }

    virtual void activate() {
        if (menu())
            menu()->popup(mapToGlobal(QPoint(0, height())));
        else
            click();
    }
};

class WorkspaceButtonAccessible final : public QAccessibleWidget {
  public:
    explicit WorkspaceButtonAccessible(WorkspaceToolButton* button)
        : QAccessibleWidget(button, QAccessible::Button) {
    }

    QStringList actionNames() const override {
        auto result = QAccessibleWidget::actionNames();
        result << QAccessibleActionInterface::pressAction();
        if (static_cast<WorkspaceToolButton*>(widget())->menu())
            result << QAccessibleActionInterface::showMenuAction();
        result.removeDuplicates();
        return result;
    }

    void doAction(const QString& name) override {
        if (name == QAccessibleActionInterface::pressAction() ||
            name == QAccessibleActionInterface::showMenuAction()) {
            const QPointer<WorkspaceToolButton> button = static_cast<WorkspaceToolButton*>(widget());
            QTimer::singleShot(0, button, [button] {
                if (!button || !button->isEnabled())
                    return;
                button->activate();
            });
            return;
        }
        QAccessibleWidget::doAction(name);
    }

    QAccessible::State state() const override {
        auto result = QAccessibleWidget::state();
        const auto* button = static_cast<WorkspaceToolButton*>(widget());
        result.checkable = button->isCheckable();
        result.checked = button->isChecked();
        result.hasPopup = button->menu() != nullptr;
        return result;
    }
};

inline QAccessibleInterface* workspaceButtonAccessibility(const QString&, QObject* object) {
    if (auto* button = dynamic_cast<WorkspaceToolButton*>(object))
        return new WorkspaceButtonAccessible(button);
    return nullptr;
}

class HoverToolButton final : public WorkspaceToolButton {
    QTimer hover_, watch_, dismiss_;
    bool hoverOpened_ = false;

  public:
    explicit HoverToolButton(QWidget* parent = nullptr) : WorkspaceToolButton(parent) {
        setPopupMode(QToolButton::DelayedPopup);
        setAutoRaise(true);
        setMouseTracking(true);
        hover_.setSingleShot(true);
        hover_.setInterval(220);
        watch_.setInterval(50);
        dismiss_.setSingleShot(true);
        dismiss_.setInterval(200);
        connect(this, &QToolButton::clicked, this, [this] { activate(); });
        connect(this, &QToolButton::pressed, this, [this] {
            hover_.stop();
            watch_.stop();
            dismiss_.stop();
            hoverOpened_ = false;
        });
        connect(&hover_, &QTimer::timeout, this, [this] {
            if (underMouse() && isEnabled() && menu() && !menu()->isVisible()) {
                if (auto* popup = QApplication::activePopupWidget())
                    popup->hide();
                hoverOpened_ = true;
                menu()->popup(mapToGlobal(QPoint(0, height())));
                watch_.start();
            }
        });
        connect(&watch_, &QTimer::timeout, this, [this] {
            if (!menu() || !menu()->isVisible()) {
                watch_.stop();
                dismiss_.stop();
                hoverOpened_ = false;
                return;
            }
            if (!hoverOpened_)
                return;
            const auto position = QCursor::pos();
            auto* popup = QApplication::activePopupWidget();
            const bool nested = popup && popup != menu() && menu()->isAncestorOf(popup);
            const bool inside = rect().contains(mapFromGlobal(position)) ||
                                menu()->rect().contains(menu()->mapFromGlobal(position)) || nested;
            if (inside)
                dismiss_.stop();
            else if (!dismiss_.isActive())
                dismiss_.start();
        });
        connect(&dismiss_, &QTimer::timeout, this, [this] {
            if (menu() && hoverOpened_)
                menu()->hide();
        });
    }

    void activate() override {
        hover_.stop();
        watch_.stop();
        dismiss_.stop();
        hoverOpened_ = false;
        if (menu())
            menu()->popup(mapToGlobal(QPoint(0, height())));
    }

  protected:
    void enterEvent(QEnterEvent* event) override {
        hover_.start();
        QToolButton::enterEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        hover_.stop();
        QToolButton::leaveEvent(event);
    }

    void hideEvent(QHideEvent* event) override {
        hover_.stop();
        QToolButton::hideEvent(event);
    }
};

class DocumentTabBar final : public QTabBar {
  public:
    using QTabBar::QTabBar;

  protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::MiddleButton) {
            const int index = tabAt(event->position().toPoint());
            if (index >= 0)
                emit tabCloseRequested(index);
            event->accept();
            return;
        }
        QTabBar::mouseReleaseEvent(event);
    }
};
