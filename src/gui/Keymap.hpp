#pragma once
#include <QAction>
#include <QSettings>
#include <QShortcut>

struct WorkspaceShortcut {
    const char* id;
    const char* label;
    const char* key;
};

inline constexpr WorkspaceShortcut workspaceShortcuts[] = {
    {"build", "Build", "Ctrl+B"},       {"run", "Run code", "Ctrl+F5"},
    {"stop", "Stop", "Shift+F5"},       {"commands", "Find action", "Ctrl+K"},
    {"find", "Find in file", "Ctrl+F"}, {"replace", "Find and replace", "Ctrl+H"},
    {"line", "Go to line", "Ctrl+G"},   {"project", "Project tree", "Alt+1"},
    {"settings", "Settings", "Ctrl+,"}};

inline QString shortcutKey(const char* id) {
    for (const auto& spec : workspaceShortcuts)
        if (QString::fromLatin1(spec.id) == id)
            return QSettings().value(QString("keymap/") + id, spec.key).toString();
    return {};
}

inline QShortcut* workspaceShortcut(const char* id, QWidget* owner) {
    auto* shortcut = new QShortcut(QKeySequence(shortcutKey(id)), owner);
    shortcut->setProperty("keymapId", id);
    return shortcut;
}

inline void workspaceActionShortcut(QAction* action, const char* id) {
    action->setProperty("keymapId", id);
    action->setShortcut(QKeySequence(shortcutKey(id)));
}
