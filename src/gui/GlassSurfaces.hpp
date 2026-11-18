#pragma once
#include "ThemeManager.hpp"
#include <QFrame>
#include <QLinearGradient>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QWidget>

namespace tsui {
class GlassStyle final {
  public:
    static bool enabled() noexcept {
        return enabled_;
    }

    static void setEnabled(bool value) noexcept {
        enabled_ = value;
    }

  private:
    inline static bool enabled_ = true;
};

class GlassBackdrop final : public QWidget {
  public:
    using QWidget::QWidget;

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), ThemeManager::color(Role::Window));
    }
};

class FrostedHeader final : public QWidget {
  public:
    using QWidget::QWidget;

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const auto base = ThemeManager::color(Role::Sidebar);
        const auto border = ThemeManager::color(Role::Border);
        const bool glass = GlassStyle::enabled() && ThemeManager::current() != Theme::HighContrast;
        if (glass) {
            QLinearGradient film(0, 0, 0, height());
            QColor top = base.lighter(ThemeManager::isLight() ? 102 : 111);
            QColor bottom = base;
            top.setAlpha(238);
            bottom.setAlpha(248);
            film.setColorAt(0, top);
            film.setColorAt(1, bottom);
            painter.fillRect(rect(), film);
            painter.setPen(QPen(QColor(255, 255, 255, ThemeManager::isLight() ? 105 : 20), 1));
            painter.drawLine(0, 0, width(), 0);
        } else {
            painter.fillRect(rect(), base);
        }
        painter.setPen(border);
        painter.drawLine(0, height() - 1, width(), height() - 1);
    }
};

class GlassPopupSurface final : public QFrame {
  public:
    using QFrame::QFrame;

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const auto base = ThemeManager::color(Role::Surface);
        const bool glass = GlassStyle::enabled() && ThemeManager::current() != Theme::HighContrast;
        QPainterPath path;
        path.addRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 2, 2);
        if (glass) {
            QLinearGradient film(0, 0, 0, height());
            QColor top = base.lighter(ThemeManager::isLight() ? 101 : 115);
            QColor bottom = base;
            top.setAlpha(242);
            bottom.setAlpha(252);
            film.setColorAt(0, top);
            film.setColorAt(1, bottom);
            painter.fillPath(path, film);
        } else {
            painter.fillPath(path, base);
        }
        painter.setPen(QPen(ThemeManager::color(Role::Border), 1));
        painter.drawPath(path);
    }
};
}
