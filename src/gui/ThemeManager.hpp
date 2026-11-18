#pragma once
#include <QColor>
#include <QFont>
#include <QString>

namespace tsui {
enum class Theme { Darcula, Light, HighContrast };
enum class Role {
    Window,
    Sidebar,
    Surface,
    Raised,
    Border,
    Text,
    Muted,
    Accent,
    AccentSoft,
    Editor,
    Selection,
    Good,
    Warning,
    Error,
    Grid,
    CurrentLine,
    BarText
};

class ThemeManager {
  public:
    static double scale() noexcept;
    static void setScale(double value);
    static bool isLight() noexcept;
    static QFont font(int pixels, QFont::Weight weight = QFont::Normal);
    static Theme current() noexcept;
    static QColor color(Role role);
    static void apply(Theme theme);
    static QString displayName(Theme theme);
    static Theme fromKey(const QString& key);
    static QString key(Theme theme);
};
}
