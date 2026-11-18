#include "UiLanguage.hpp"
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QRegularExpression>
#include <QResource>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <algorithm>
#include <functional>

void initializeVeyraResources() {
    Q_INIT_RESOURCE(branding);
}

namespace {
unsigned localeGeneration = 1;
QString selected = "en";
QHash<QString, QString> forward, reverse, englishMemo, russianMemo;

struct Pattern {
    QRegularExpression expression;
    QString replacement;
};

QList<Pattern> englishPatterns, russianPatterns;
QList<QPair<QString, QString>> englishFragments, russianFragments;
const QRegularExpression cyrillic("[А-Яа-яЁё]"), placeholder("%[1-9]");

void addPattern(const QString& from, const QString& to, QList<Pattern>& list) {
    if (!from.contains(QRegularExpression("%[1-9]")))
        return;
    QString expression = "^";
    int begin = 0;
    auto matches = QRegularExpression("%[1-9]").globalMatch(from);
    QStringList ids;
    while (matches.hasNext()) {
        const auto match = matches.next();
        expression += QRegularExpression::escape(from.mid(begin, match.capturedStart() - begin)) + "(.*?)";
        ids << match.captured();
        begin = match.capturedEnd();
    }
    expression += QRegularExpression::escape(from.mid(begin)) + "$";
    QString replacement = to;
    for (int i = 0; i < ids.size(); ++i)
        replacement.replace(ids[i], QString("\\%1").arg(i + 1));
    list.append(
        {QRegularExpression(expression, QRegularExpression::DotMatchesEverythingOption), replacement});
}

QString convert(const QString& value, bool english) {
    if (value.isEmpty())
        return value;
    const auto& map = english ? forward : reverse;
    const auto found = map.constFind(value);
    if (found != map.cend())
        return *found;
    auto& memo = english ? englishMemo : russianMemo;
    const auto cached = memo.constFind(value);
    if (cached != memo.cend())
        return *cached;
    const auto remember = [&](const QString& result) {
        if (value.size() <= 8192) {
            if (memo.size() >= 4096)
                memo.clear();
            memo.insert(value, result);
        }
        return result;
    };
    const auto& patterns = english ? englishPatterns : russianPatterns;
    for (const auto& pattern : patterns) {
        const auto match = pattern.expression.match(value);
        if (match.hasMatch()) {
            auto output = value;
            output.replace(pattern.expression, pattern.replacement);
            return remember(output);
        }
    }
    if (english && value.contains(cyrillic) && value.size() < 16384) {
        auto output = value;
        for (const auto& entry : englishFragments)
            output.replace(entry.first, entry.second);
        return remember(output);
    }
    if (!english && value.size() < 16384) {
        auto output = value;
        for (const auto& entry : russianFragments)
            output.replace(entry.first, entry.second);
        return remember(output);
    }
    return remember(value);
}

void localize(QObject* object, const char* field, const QString& current,
              const std::function<void(const QString&)>& setter) {
    const auto sourceKey = QByteArray("localeSource_") + field, shownKey = QByteArray("localeShown_") + field,
               generationKey = QByteArray("localeVersion_") + field;
    const auto shown = object->property(shownKey);
    if (shown.isValid() && shown.toString() == current &&
        object->property(generationKey).toUInt() == localeGeneration)
        return;
    QString source = object->property(sourceKey).toString();
    if (!shown.isValid() || shown.toString() != current) {
        source = convert(current, false);
        object->setProperty(sourceKey, source);
    }
    const auto value = convert(source, selected == "en");
    object->setProperty(shownKey, value);
    object->setProperty(generationKey, localeGeneration);
    if (value != current)
        setter(value);
}
}

namespace UiLanguage {
void initialize() {
    static bool initialized = false;
    if (initialized)
        return;
    initialized = true;
    initializeVeyraResources();
    QFile file(":/i18n/en.json");
    if (file.open(QIODevice::ReadOnly)) {
        const auto object = QJsonDocument::fromJson(file.readAll()).object();
        for (auto it = object.begin(); it != object.end(); ++it) {
            const auto value = it.value().toString();
            forward.insert(it.key(), value);
            reverse.insert(value, it.key());
            addPattern(it.key(), value, englishPatterns);
            addPattern(value, it.key(), russianPatterns);
            if (it.key().size() >= 18 && !it.key().contains(placeholder))
                englishFragments.append({it.key(), value});
            if (value.size() >= 12 && !value.contains(placeholder))
                russianFragments.append({value, it.key()});
        }
        std::sort(englishFragments.begin(), englishFragments.end(),
                  [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    }
    std::sort(russianFragments.begin(), russianFragments.end(),
              [](const auto& left, const auto& right) { return left.first.size() > right.first.size(); });
    selected = QSettings().value("ui/language", "en").toString() == "en" ? "en" : "ru";
    if (qobject_cast<QApplication*>(QCoreApplication::instance())) {
        auto* timer = new QTimer(qApp);
        timer->setInterval(500);
        QObject::connect(timer, &QTimer::timeout, qApp, [] { apply(); });
        timer->start();
    }
}

QString nativeName(const QString& code) {
    initialize();
    return code == "ru" ? reverse.value("Russian", "Russian") : QString("English");
}

QString code() {
    initialize();
    return selected;
}

QString text(const QString& source) {
    initialize();
    return convert(source, selected == "en");
}

void setLanguage(const QString& value) {
    initialize();
    const auto requested = value == "en" ? QString("en") : QString("ru");
    if (selected != requested)
        ++localeGeneration;
    selected = requested;
    QSettings().setValue("ui/language", selected);
    apply();
}

void apply(QWidget* root) {
    if (!qobject_cast<QApplication*>(QCoreApplication::instance()))
        return;
    QList<QPointer<QWidget>> widgets;
    for (auto* widget : QApplication::allWidgets())
        if (!root || root == widget || root->isAncestorOf(widget))
            widgets << widget;
    for (const auto& guarded : widgets) {
        auto* widget = guarded.data();
        if (!widget)
            continue;
        const auto name = widget->objectName();
        if (name == "projectName" || name == "sourceBreadcrumbPath" || name == "settingsInterfaceLanguage")
            continue;
        localize(widget, "tooltip", widget->toolTip(),
                 [widget](const QString& value) { widget->setToolTip(value); });
        localize(widget, "accessibleName", widget->accessibleName(),
                 [widget](const QString& value) { widget->setAccessibleName(value); });
        localize(widget, "accessibleDescription", widget->accessibleDescription(),
                 [widget](const QString& value) { widget->setAccessibleDescription(value); });
        if (auto* label = qobject_cast<QLabel*>(widget))
            localize(label, "text", label->text(), [label](const QString& value) { label->setText(value); });
        if (auto* button = qobject_cast<QAbstractButton*>(widget))
            localize(button, "text", button->text(),
                     [button](const QString& value) { button->setText(value); });
        if (auto* line = qobject_cast<QLineEdit*>(widget))
            localize(line, "placeholder", line->placeholderText(),
                     [line](const QString& value) { line->setPlaceholderText(value); });
        if (auto* edit = qobject_cast<QPlainTextEdit*>(widget))
            localize(edit, "placeholder", edit->placeholderText(),
                     [edit](const QString& value) { edit->setPlaceholderText(value); });
        if (auto* status = qobject_cast<QStatusBar*>(widget)) {
            const auto value = text(status->currentMessage());
            if (value != status->currentMessage())
                status->showMessage(value, 5000);
        }
        if (auto* dock = qobject_cast<QDockWidget*>(widget))
            localize(dock, "title", dock->windowTitle(),
                     [dock](const QString& value) { dock->setWindowTitle(value); });
        if (widget->isWindow())
            localize(widget, "window", widget->windowTitle(),
                     [widget](const QString& value) { widget->setWindowTitle(value); });
        if (auto* combo = qobject_cast<QComboBox*>(widget); combo && !combo->isEditable())
            for (int i = 0; i < combo->count(); ++i) {
                const auto value = text(combo->itemText(i));
                if (value != combo->itemText(i))
                    combo->setItemText(i, value);
            }
        if (auto* tabs = qobject_cast<QTabWidget*>(widget))
            for (int i = 0; i < tabs->count(); ++i) {
                const auto value = text(tabs->tabText(i));
                if (value != tabs->tabText(i))
                    tabs->setTabText(i, value);
            }
        if (auto* table = qobject_cast<QTableWidget*>(widget))
            for (int i = 0; i < table->columnCount(); ++i)
                if (auto* item = table->horizontalHeaderItem(i)) {
                    const auto value = text(item->text());
                    if (value != item->text())
                        item->setText(value);
                }
        if (auto* list = qobject_cast<QListWidget*>(widget);
            list && (name == "settingsSections" || name == "codeFindings" || name == "ideProblems"))
            for (int i = 0; i < list->count(); ++i) {
                auto* item = list->item(i);
                const auto value = text(item->text());
                if (value != item->text())
                    item->setText(value);
            }
        if (widget->property("localeGeneration").toUInt() != localeGeneration) {
            widget->setProperty("localeGeneration", localeGeneration);
            widget->update();
        }
    }
    for (auto* window : QApplication::topLevelWidgets())
        for (auto* action : window->findChildren<QAction*>()) {
            localize(action, "text", action->text(),
                     [action](const QString& value) { action->setText(value); });
            localize(action, "tooltip", action->toolTip(),
                     [action](const QString& value) { action->setToolTip(value); });
        }
}
}
