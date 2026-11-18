#include "ThemeManager.hpp"
#include "GlassSurfaces.hpp"
#include <QApplication>
#include <QFontDatabase>
#include <QPalette>
#include <QRegularExpression>
#include <QSettings>
#include <QStyleFactory>
#include <algorithm>
#include <cmath>
#include <utility>

namespace tsui {
namespace {
Theme selected = Theme::Darcula;
double uiScale = 1.0;

struct Palette {
    const char *window, *sidebar, *surface, *raised, *border, *text, *muted, *accent, *accentSoft, *editor,
        *selection, *good, *warning, *error, *grid, *currentLine, *barText;
};

const Palette& palette() {
    static const Palette dark = {"#2B2D30", "#25262B", "#303136", "#37393E", "#41434A", "#DCE0E5",
                                 "#9CA1AA", "#85A7F8", "#303E60", "#1E1F22", "#334A64", "#65B58E",
                                 "#DBAD66", "#E27C83", "#3B3D43", "#30343C", "#142033"};
    static const Palette light = {"#F5F6F8", "#ECEEF2", "#FFFFFF", "#F3F4F6", "#D7DAE0", "#24272D",
                                  "#68717E", "#326FB3", "#E5EDF8", "#FFFFFF", "#D9E9FC", "#2B8C69",
                                  "#AD782B", "#BE4C59", "#E5E7EB", "#EDF3FB", "#16283C"};
    static const Palette contrast = {"#101216", "#16181D", "#1B1E23", "#24272D", "#858C9A", "#FFFFFF",
                                     "#D6DBE2", "#93C5FD", "#263F5B", "#101216", "#305A85", "#A0F0BE",
                                     "#FFE08A", "#FF9CA4", "#505760", "#263646", "#081421"};
    return selected == Theme::Light ? light : selected == Theme::HighContrast ? contrast : dark;
}

QString stylesheet() {
    QString sheet = QString::fromUtf8(R"QSS(
* {font-family:"${sans}";font-size:13px;}
QMainWindow {background:${window};color:${text};}
QWidget#root,QWidget#workbench,QStackedWidget#workspacePages,QStackedWidget#workspacePages>QWidget {background:transparent;color:${text};}
QScrollArea,QScrollArea>QWidget>QWidget {background:transparent;border:0;}
QWidget#sidebar {background:${sidebar};border-right:1px solid ${border};}
QWidget#ideHeader {background:transparent;border:0;}
QToolBar#workspaceToolbar,QToolBar#workspaceRail {background:${sidebar};border:0;spacing:0;padding:0;}
QToolBar#workspaceRail {border-right:1px solid ${border};}
QToolBar::handle {width:0;height:0;}
QWidget#editorBreadcrumb {background:${editor};border-bottom:1px solid ${border};}
QLabel#applicationMark {background:transparent;border:0;padding:0;}
QLabel#projectName {font-weight:600;}
QPushButton[class="toolbarIcon"],QToolButton {background:transparent;border:1px solid transparent;padding:3px;min-height:22px;}
QPushButton[class="toolbarIcon"]:hover,QToolButton:hover {background:${raised};border:1px solid ${border};}
QToolButton:checked {background:${selection};border-color:${border};}
QToolButton::menu-indicator {image:none;width:0;height:0;}
QTabBar#documentTabs {background:${sidebar};border-bottom:1px solid ${border};}
QTabBar#documentTabs::tab {background:${sidebar};padding:9px 16px;margin:5px 3px 0 3px;min-height:18px;border:0;border-top-left-radius:8px;border-top-right-radius:8px;}
QTabBar#documentTabs::close-button {image:url(:/branding/close.svg);width:12px;height:12px;padding:2px;border-radius:5px;}
QTabBar#documentTabs::close-button:hover {background:${raised};}
QTabBar#documentTabs::tab:selected {background:${editor};border-bottom:2px solid ${accent};}
QTreeView {background:${sidebar};show-decoration-selected:1;}
QTreeView::item {min-height:25px;padding:2px 8px;border-radius:5px;margin:1px 5px;}
QFrame#sourceInspection {background:${sidebar};}

QLabel {color:${text};background:transparent;border:0;}
QWidget#emptyEditor {background:${editor};}
QLabel[class="welcomeTitle"] {font-size:28px;font-weight:650;color:${text};padding:12px;}
QLabel[class="welcomeDescription"] {font-size:14px;color:${muted};}
QLabel#inlineThreadHint {background:${surface};border:1px solid ${border};border-radius:8px;margin:5px 10px;}
QAbstractItemView#codeCompletionPopup {background:${surface};border:1px solid ${border};border-radius:8px;padding:5px;}
QAbstractItemView#codeCompletionPopup::item {padding:5px 12px;min-height:22px;border-radius:4px;}
QAbstractItemView#codeCompletionPopup::item:selected {background:${selection};}
QLabel[class="brand"] {font-size:16px;font-weight:700;}
QLabel[class="tag"] {font-size:11px;font-weight:600;color:${muted};}
QLabel[class="pageTitle"] {font-size:21px;font-weight:700;}
QLabel[class="description"],QLabel[class="heroDesc"] {color:${muted};font-size:14px;}
QLabel[class="caption"],QLabel[class="metricTitle"] {color:${muted};font-size:12px;}
QLabel[class="metricValue"] {font-size:24px;font-weight:650;}
QLabel[class="sectionTitle"],QLabel[class="heroTitle"] {font-size:16px;font-weight:650;}
QFrame[class="panel"],QFrame[class="metricAccent"],QFrame[class="hero"] {background:${surface};border:1px solid ${border};border-radius:6px;}
QFrame[class="panel"] QLabel {border:0;}
QPushButton,QToolButton {background:${raised};color:${text};border:1px solid ${border};border-radius:6px;padding:6px 12px;min-height:22px;font-weight:500;}
QPushButton:hover,QToolButton:hover {background:${accentSoft};border-color:${accent};}
QPushButton:focus,QToolButton:focus {border:2px solid ${accent};}
QPushButton:pressed,QToolButton:pressed {background:${selection};}
QPushButton:disabled,QToolButton:disabled {color:${muted};background:${surface};border-color:${border};}
QPushButton[class="primary"] {background:${accent};color:${barText};border-color:${accent};font-weight:650;}
QPushButton[class="primary"]:hover {border-color:${text};}
QPushButton[class="nav"] {background:transparent;color:${muted};border:1px solid transparent;border-radius:6px;text-align:left;padding:6px 10px;min-height:24px;}
QPushButton[class="nav"]:hover {background:${raised};color:${text};}
QPushButton[class="nav"]:checked {background:${accentSoft};border:1px solid ${border};color:${text};font-weight:650;}
QComboBox,QSpinBox,QLineEdit,QTextEdit {background:${editor};color:${text};border:1px solid ${border};border-radius:6px;min-height:24px;padding:5px 9px;selection-background-color:${selection};}
QComboBox {padding-right:24px;}
QComboBox:focus,QSpinBox:focus,QLineEdit:focus,QTextEdit:focus {border-color:${accent};}
QComboBox QAbstractItemView {background:${surface};color:${text};selection-background-color:${selection};padding:4px;}
QTableWidget,QTreeWidget,QTreeView {background:${editor};alternate-background-color:${surface};color:${text};border:0;gridline-color:${grid};selection-background-color:${selection};selection-color:${text};}
QHeaderView::section {background:${raised};color:${muted};border:0;border-bottom:1px solid ${border};padding:9px 10px;font-weight:600;}
QTreeWidget::item,QTreeView::item {min-height:26px;padding:3px 6px;}
QTreeWidget::item:hover,QTreeView::item:hover {background:${raised};}
QTreeWidget::item:selected,QTreeView::item:selected {background:${selection};color:${text};}
QListWidget {background:${editor};color:${text};border:1px solid ${border};border-radius:6px;outline:0;padding:5px;}
QListWidget::item {padding:7px 10px;border-bottom:1px solid ${grid};}
QListWidget::item:selected {background:${selection};color:${text};}
QPlainTextEdit#codeEditor,QPlainTextEdit#ideConsole {background:${editor};color:${text};border:0;selection-background-color:${selection};padding:10px;font-family:"${mono}";font-size:14px;}
QPlainTextEdit#codeEditor QWidget {font-family:"${mono}";}
QScrollBar:vertical {background:transparent;width:10px;border:0;}
QScrollBar:horizontal {background:transparent;height:10px;border:0;}
QScrollBar::handle:vertical,QScrollBar::handle:horizontal {background:${border};border-radius:6px;min-height:28px;min-width:28px;}
QScrollBar::handle:hover {background:${muted};}
QScrollBar::add-line,QScrollBar::sub-line {width:0;height:0;}
QSplitter::handle {background:transparent;}
QSplitter::handle:hover {background:${accentSoft};}
QSlider::groove:horizontal {height:4px;background:${border};border-radius:6px;}
QSlider::sub-page:horizontal {background:${accent};border-radius:6px;}
QSlider::handle:horizontal {background:${accent};width:14px;margin:-5px 0;border-radius:6px;}
QProgressBar {background:${raised};border:0;border-radius:6px;color:${text};}
QProgressBar::chunk {background:${accent};border-radius:6px;}
QStatusBar {background:${sidebar};color:${muted};border-top:1px solid ${border};font-size:12px;}
QMenuBar {background:${sidebar};color:${text};padding:3px;}
QMenuBar::item:selected,QMenu::item:selected {background:${accentSoft};}
QMenu {background:${surface};color:${text};border:1px solid ${border};border-radius:6px;padding:6px;}
QMenu::item {padding:8px 32px 8px 14px;}
QToolTip {background:${surface};color:${text};border:1px solid ${border};padding:8px;}
QLineEdit#commandSearch {background:${editor};font-size:16px;padding:7px 10px;min-height:30px;}
QListWidget#commandResults {background:transparent;border:0;padding:4px;}
QListWidget#commandResults::item {padding:7px 10px;}
QListWidget#commandResults::item:selected {background:${selection};color:${text};}
QTabBar::tab {background:transparent;color:${muted};padding:6px 12px;border-bottom:2px solid transparent;min-height:24px;}
QTabBar::tab:selected {color:${text};border-bottom:2px solid ${accent};font-weight:650;}
QTabBar::tab:hover {background:${accentSoft};}
QTabWidget::pane {border:0;background:transparent;}
QToolButton#toggleOutput {padding:2px 9px;min-height:18px;font-size:12px;}
QDockWidget {color:${text};font-weight:600;}
QDockWidget::title {background:${sidebar};padding:6px 10px;border-bottom:1px solid ${border};text-align:left;}
QGroupBox {border:1px solid ${border};border-radius:6px;margin-top:12px;padding:14px 12px 10px 12px;color:${text};font-weight:600;}
QGroupBox::title {subcontrol-origin:margin;left:14px;padding:0 5px;}
QWidget#sidebar QToolButton,QWidget#ideHeader QToolButton,QWidget#editorBreadcrumb QToolButton {background:transparent;border:1px solid transparent;padding:3px;min-height:16px;min-width:16px;}
QWidget#sidebar QToolButton:hover,QWidget#ideHeader QToolButton:hover {background:${raised};border-color:${border};}
QWidget#sidebar QToolButton:checked {background:${selection};border-color:${border};}
)QSS");

    const QRegularExpression pixels("(-?[0-9]+)px");
    QString scaled;
    int from = 0;
    auto matches = pixels.globalMatch(sheet);
    while (matches.hasNext()) {
        const auto match = matches.next();
        scaled += sheet.mid(from, match.capturedStart() - from);
        scaled += QString::number(qRound(match.captured(1).toInt() * uiScale)) + "px";
        from = match.capturedEnd();
    }
    sheet = scaled + sheet.mid(from);
    sheet.replace("${sans}", QFontDatabase::systemFont(QFontDatabase::GeneralFont).family());
    sheet.replace("${mono}", QSettings()
                                 .value("editor/fontFamily",
                                        QFontDatabase::systemFont(QFontDatabase::FixedFont).family())
                                 .toString());
    const Palette& p = palette();
    const std::pair<const char*, const char*> vars[] = {{"window", p.window},
                                                        {"sidebar", p.sidebar},
                                                        {"surface", p.surface},
                                                        {"raised", p.raised},
                                                        {"border", p.border},
                                                        {"text", p.text},
                                                        {"muted", p.muted},
                                                        {"accent", p.accent},
                                                        {"accentSoft", p.accentSoft},
                                                        {"editor", p.editor},
                                                        {"selection", p.selection},
                                                        {"good", p.good},
                                                        {"warning", p.warning},
                                                        {"error", p.error},
                                                        {"grid", p.grid},
                                                        {"currentLine", p.currentLine},
                                                        {"barText", p.barText}};
    for (const auto& [name, value] : vars)
        sheet.replace("${" + QString::fromLatin1(name) + "}", QString::fromLatin1(value));
    return sheet;
}
}

double ThemeManager::scale() noexcept {
    return uiScale;
}

void ThemeManager::setScale(double value) {
    uiScale = std::clamp(value, 0.85, 2.0);
    apply(selected);
}

bool ThemeManager::isLight() noexcept {
    return selected == Theme::Light;
}

QFont ThemeManager::font(int pixels, QFont::Weight weight) {
    QFont result = qApp ? qApp->font() : QFont();
    result.setPixelSize(qRound(pixels * uiScale));
    result.setWeight(weight);
    return result;
}

Theme ThemeManager::current() noexcept {
    return selected;
}

QColor ThemeManager::color(Role role) {
    const Palette& p = palette();
    switch (role) {
        case Role::Window:
            return QColor(p.window);
        case Role::Sidebar:
            return QColor(p.sidebar);
        case Role::Surface:
            return QColor(p.surface);
        case Role::Raised:
            return QColor(p.raised);
        case Role::Border:
            return QColor(p.border);
        case Role::Text:
            return QColor(p.text);
        case Role::Muted:
            return QColor(p.muted);
        case Role::Accent:
            return QColor(p.accent);
        case Role::AccentSoft:
            return QColor(p.accentSoft);
        case Role::Editor:
            return QColor(p.editor);
        case Role::Selection:
            return QColor(p.selection);
        case Role::Good:
            return QColor(p.good);
        case Role::Warning:
            return QColor(p.warning);
        case Role::Error:
            return QColor(p.error);
        case Role::Grid:
            return QColor(p.grid);
        case Role::CurrentLine:
            return QColor(p.currentLine);
        case Role::BarText:
            return QColor(p.barText);
    }
    return QColor(p.text);
}

void ThemeManager::apply(Theme theme) {
    selected = theme;
    if (!qApp)
        return;
    qApp->setStyle(QStyleFactory::create("Fusion"));
    QPalette pal;
    pal.setColor(QPalette::Window, color(Role::Window));
    pal.setColor(QPalette::WindowText, color(Role::Text));
    pal.setColor(QPalette::Base, color(Role::Editor));
    pal.setColor(QPalette::AlternateBase, color(Role::Surface));
    pal.setColor(QPalette::Text, color(Role::Text));
    pal.setColor(QPalette::Button, color(Role::Raised));
    pal.setColor(QPalette::ButtonText, color(Role::Text));
    pal.setColor(QPalette::Highlight, color(Role::Selection));
    pal.setColor(QPalette::HighlightedText, color(Role::Text));
    pal.setColor(QPalette::ToolTipBase, color(Role::Raised));
    pal.setColor(QPalette::ToolTipText, color(Role::Text));
    pal.setColor(QPalette::PlaceholderText, color(Role::Muted));
    qApp->setPalette(pal);
    qApp->setStyleSheet(stylesheet());
}

QString ThemeManager::key(Theme theme) {
    return theme == Theme::Light ? "light" : theme == Theme::HighContrast ? "high-contrast" : "darcula";
}

Theme ThemeManager::fromKey(const QString& key) {
    return key == "light" ? Theme::Light : key == "high-contrast" ? Theme::HighContrast : Theme::Darcula;
}

QString ThemeManager::displayName(Theme theme) {
    return theme == Theme::Light ? "Light" : theme == Theme::HighContrast ? "High Contrast" : "Darcula";
}
}
