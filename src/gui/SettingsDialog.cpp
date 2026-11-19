#include "SettingsDialog.hpp"
#include "Keymap.hpp"
#include "SourceTemplates.hpp"
#include "ThemeManager.hpp"
#include "ToolDiscovery.hpp"
#include "UiLanguage.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTableWidget>
#include <QVBoxLayout>

SettingsDialog::SettingsDialog(QWidget* parent, std::function<void()> applied)
    : QDialog(parent), applied_(std::move(applied)) {
    setObjectName("settingsDialog");
    setWindowTitle(UiLanguage::tr("Settings"));
    resize(880, 620);
    setMinimumSize(650, 460);
    auto* outer = new QVBoxLayout(this);
    search_ = new QLineEdit;
    search_->setObjectName("settingsSearch");
    search_->setPlaceholderText(UiLanguage::tr("Search settings…"));
    outer->addWidget(search_);
    auto* body = new QHBoxLayout;
    outer->addLayout(body, 1);
    sections_ = new QListWidget;
    sections_->setObjectName("settingsSections");
    sections_->setFixedWidth(210);
    body->addWidget(sections_);
    pages_ = new QStackedWidget;
    body->addWidget(pages_, 1);
    QSettings prefs;
    const auto page = [&](const QString& title) {
        sections_->addItem(title);
        auto* widget = new QWidget;
        auto* form = new QFormLayout(widget);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        pages_->addWidget(widget);
        return form;
    };
    const auto check = [&](QFormLayout* form, const QString& title, const char* key, bool fallback) {
        auto* value = new QCheckBox(title);
        value->setObjectName(key);
        value->setProperty("settingsKey", key);
        value->setChecked(prefs.value(key, fallback).toBool());
        form->addRow(value);
        return value;
    };
    const auto spin = [&](QFormLayout* form, const QString& title, const char* key, int fallback, int low,
                          int high) {
        auto* value = new QSpinBox;
        value->setObjectName(key);
        value->setProperty("settingsKey", key);
        value->setRange(low, high);
        value->setValue(prefs.value(key, fallback).toInt());
        form->addRow(title, value);
        return value;
    };
    auto* appearance = page(UiLanguage::tr("Appearance"));
    auto* uiLanguage = new QComboBox;
    uiLanguage->setObjectName("settingsInterfaceLanguage");
    uiLanguage->addItem(UiLanguage::nativeName("ru"), "ru");
    uiLanguage->addItem(UiLanguage::nativeName("en"), "en");
    uiLanguage->setCurrentIndex(prefs.value("ui/language", "en").toString() == "en" ? 1 : 0);
    appearance->addRow(UiLanguage::tr("Interface language"), uiLanguage);
    auto* theme = new QComboBox;
    theme->setObjectName("settingsTheme");
    for (auto id : {"darcula", "light", "high-contrast"})
        theme->addItem(tsui::ThemeManager::displayName(tsui::ThemeManager::fromKey(id)), id);
    theme->setCurrentIndex(std::max(0, theme->findData(prefs.value("appearance/theme", "darcula"))));
    appearance->addRow(UiLanguage::tr("Theme"), theme);
    spin(appearance, UiLanguage::tr("UI scale, %"), "appearance/uiScale", 100, 85, 200);
    check(appearance, UiLanguage::tr("Glass surfaces"), "appearance/glass", true);
    auto* editor = page(UiLanguage::tr("Editor"));
    auto* font = new QFontComboBox;
    font->setObjectName("settingsFont");
    font->setEditable(false);
    font->setFontFilters(QFontComboBox::MonospacedFonts);
    font->setCurrentFont(
        QFont(prefs.value("editor/fontFamily", QFontDatabase::systemFont(QFontDatabase::FixedFont).family())
                  .toString()));
    editor->addRow(UiLanguage::tr("Monospaced font"), font);
    spin(editor, UiLanguage::tr("Font size, px"), "editor/basePixelSize", 14, 10, 32);
    spin(editor, UiLanguage::tr("Indent width"), "editor/tabWidth", 4, 2, 8);
    check(editor, UiLanguage::tr("Automatically close brackets"), "editor/autoPairs", true);
    check(editor, UiLanguage::tr("Indent guides"), "editor/indentGuides", true);
    check(editor, UiLanguage::tr("Wrap long lines"), "editor/wordWrap", false);
    auto* completion = page(UiLanguage::tr("Completion"));
    check(completion, UiLanguage::tr("Suggest completions while typing"), "editor/autoCompletion", true);
    spin(completion, UiLanguage::tr("Completion delay, ms"), "editor/completionDelay", 180, 50, 1500);
    check(completion, UiLanguage::tr("Show thread hints while typing"), "threads/live", true);
    spin(completion, UiLanguage::tr("Thread hint delay, ms"), "threads/delay", 250, 100, 2000);
    check(completion, UiLanguage::tr("Enable language server"), "lsp/enabled", true);
    auto* server = new QLineEdit(prefs.value("lsp/cpp/command", "clangd").toString());
    server->setObjectName("settingsClangd");
    completion->addRow(UiLanguage::tr("clangd command"), server);
    auto* database = new QLineEdit(prefs.value("lsp/cpp/compileCommands").toString());
    database->setObjectName("lsp/cpp/compileCommands");
    database->setProperty("settingsKey", "lsp/cpp/compileCommands");
    database->setPlaceholderText(UiLanguage::tr("Automatic from the configured CMake project"));
    completion->addRow(UiLanguage::tr("compile_commands.json directory"), database);
    auto* info = new QLabel(
        UiLanguage::tr("Library functions, types and class members come from clangd. Without a server, names "
                       "from the open document are available. Thread hints run locally."));
    info->setWordWrap(true);
    completion->addRow(info);
    auto* assistant = page(UiLanguage::tr("Local assistant"));
    check(assistant, UiLanguage::tr("Predict code locally while typing"), "assistant/completion", true);
    check(assistant, UiLanguage::tr("Use the local model for thread reviews"), "assistant/review", true);
    auto* localInfo = new QLabel(
        UiLanguage::tr("The bundled code model runs on this computer. Code stays local. Press Tab to accept "
                       "a gray continuation, Escape to dismiss, or Alt+\\ to request one."));
    localInfo->setWordWrap(true);
    assistant->addRow(localInfo);
    auto* tools = page(UiLanguage::tr("Build and tools"));
    for (const auto& field : {qMakePair(QString("CMake"), QString("toolchains/cmake")),
                              qMakePair(QString("C++ compiler"), QString("toolchains/cpp/build"))}) {
        auto* line = new QLineEdit(
            prefs
                .value(field.second,
                       field.first == "CMake"
                           ? QString("cmake")
                           : QString::fromStdString(ts::buildPlanFor("main.cpp", "output").build.program))
                .toString());
        line->setObjectName(field.second);
        line->setProperty("settingsKey", field.second);
        tools->addRow(field.first, line);
    }
    auto* debugger = new QLineEdit(prefs.value("debugger/adapter").toString());
    debugger->setPlaceholderText("lldb-dap");
    debugger->setProperty("settingsKey", "debugger/adapter");
    tools->addRow(UiLanguage::tr("LLDB DAP executable"), debugger);
    auto* toolInfo = new QLabel(UiLanguage::tr(
        "Choose the CMake target, generator and Debug/Release in the project Build/Run dialog. Enter a "
        "compiler executable name from PATH or an absolute path, without arguments."));
    toolInfo->setWordWrap(true);
    tools->addRow(toolInfo);
    auto* adapters = page(UiLanguage::tr("Language adapters"));
    auto* adapterInfo = new QLabel(UiLanguage::tr(
        "Language support is described by adapter manifests. SDKs may include veyra-languages.json; "
        "connecting that SDK imports its definitions. You can also import a manifest explicitly. An adapter "
        "describes file types and local commands; types and APIs come from its language server."));
    adapterInfo->setWordWrap(true);
    adapters->addRow(adapterInfo);
    auto* importAdapter = new QPushButton(UiLanguage::tr("Import language adapters…"));
    adapters->addRow(importAdapter);
    connect(importAdapter, &QPushButton::clicked, this, [this] {
        const auto file = QFileDialog::getOpenFileName(this, UiLanguage::tr("Import language adapters"), {},
                                                       "JSON (*.json)");
        if (file.isEmpty())
            return;
        std::string error;
        if (!ts::importLanguageProfiles(file.toStdString(), error))
            QMessageBox::warning(this, UiLanguage::tr("Language adapters"),
                                 UiLanguage::text(QString::fromStdString(error)));
    });
    auto* languages = page(UiLanguage::tr("Languages and SDKs"));
    auto* languageInfo = new QLabel(UiLanguage::tr(
        "Compiler and runtime fields accept executable paths without arguments. LSP commands accept "
        "arguments. Empty fields use the default tool. Checks cannot guarantee the absence of all errors."));
    languageInfo->setWordWrap(true);
    languages->addRow(languageInfo);
    auto* matrix = new QTableWidget(0, 5);
    matrix->setObjectName("settingsLanguageTools");
    matrix->setHorizontalHeaderLabels({"Language", "Build / check", "Run", "LSP", "SDK status"});
    matrix->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    matrix->verticalHeader()->hide();
    matrix->setMinimumHeight(340);
    matrix->setColumnWidth(0, 90);
    for (int column = 1; column < 4; ++column)
        matrix->setColumnWidth(column, 180);
    matrix->setColumnWidth(4, 220);
    for (const auto& profile : ts::availableLanguages()) {
        const auto row = matrix->rowCount();
        matrix->insertRow(row);
        auto* name = new QTableWidgetItem(QString::fromStdString(profile.name));
        name->setFlags(name->flags() & ~Qt::ItemIsEditable);
        matrix->setItem(row, 0, name);
        const auto plan = ts::buildPlanFor("Example." + sourceExtension(profile.id).toStdString(), "output");
        QStringList status;
        for (int column = 1; column < 4; ++column) {
            const bool lsp = column == 3;
            const auto spec = column == 1 ? plan.build : plan.run;
            if (column == 2 && plan.compiled && plan.run.program == plan.outputPath) {
                auto* item = new QTableWidgetItem("Built executable");
                item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                matrix->setItem(row, column, item);
                continue;
            }
            const auto fallback = lsp          ? QString::fromStdString(profile.suggestedServer)
                                  : spec.valid ? QString::fromStdString(spec.program)
                                               : QString();
            const auto key = (lsp ? "lsp/" : "toolchains/") + QString::fromStdString(profile.lspId) +
                             (lsp           ? "/command"
                              : column == 1 ? "/build"
                                            : "/run");
            if (fallback.isEmpty()) {
                auto* item = new QTableWidgetItem("—");
                item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                matrix->setItem(row, column, item);
                continue;
            }
            if (key == "lsp/cpp/command" || key == "toolchains/cpp/build") {
                auto* item = new QTableWidgetItem(UiLanguage::tr("See main settings"));
                item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                matrix->setItem(row, column, item);
                continue;
            }
            auto* line = new QLineEdit(prefs.value(key).toString());
            line->setPlaceholderText(fallback);
            line->setObjectName(key);
            line->setProperty("settingsKey", key);
            matrix->setCellWidget(row, column, line);
            const auto effective = line->text().isEmpty() ? fallback : line->text();
            const auto executable = lsp ? QProcess::splitCommand(effective).value(0) : effective;
            status << (ToolDiscovery::find(executable).isEmpty() ? "Missing: " : "Found: ") + executable;
        }
        auto* available = new QTableWidgetItem(status.join("\n"));
        available->setFlags(available->flags() & ~Qt::ItemIsEditable);
        matrix->setItem(row, 4, available);
        matrix->setRowHeight(row, 64);
    }
    languages->addRow(matrix);
    auto* sdkPage = page(UiLanguage::tr("Custom SDKs"));
    auto* sdkInfo = new QLabel(UiLanguage::tr(
        "Add an SDK directory: its bin directory and the directory itself are added to PATH for builds, LSP "
        "and the terminal. An optional variable, such as JAVA_HOME or GOROOT, is set to the SDK path. The "
        "first SDK takes priority. Restart the terminal after applying changes."));
    sdkInfo->setWordWrap(true);
    sdkPage->addRow(sdkInfo);
    const auto projectRoot = prefs.value("workspace/root").toString();
    if (!projectRoot.isEmpty()) {
        auto* projectSdk = new QLineEdit(ToolDiscovery::projectSdk(projectRoot).root);
        projectSdk->setObjectName("projectSdkRoot");
        projectSdk->setProperty("projectRoot", projectRoot);
        projectSdk->setPlaceholderText(UiLanguage::tr("Empty: system tools"));
        sdkPage->addRow(UiLanguage::tr("Current project SDK"), projectSdk);
    }
    auto* sdkTable = new QTableWidget(0, 3);
    sdkTable->setObjectName("settingsSdks");
    sdkTable->setHorizontalHeaderLabels(
        {UiLanguage::tr("Title"), UiLanguage::tr("SDK directory"), UiLanguage::tr("Environment variable")});
    sdkTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    sdkTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    sdkTable->setMinimumHeight(260);
    const auto addSdk = [sdkTable](const QString& name, const QString& root, const QString& variable) {
        const int row = sdkTable->rowCount();
        sdkTable->insertRow(row);
        sdkTable->setItem(row, 0, new QTableWidgetItem(name));
        sdkTable->setItem(row, 1, new QTableWidgetItem(root));
        sdkTable->setItem(row, 2, new QTableWidgetItem(variable));
        sdkTable->setCurrentCell(row, 0);
    };
    for (const auto& sdk : ToolDiscovery::sdks())
        addSdk(sdk.name, sdk.root, sdk.variable);
    sdkPage->addRow(sdkTable);
    auto* sdkButtons = new QHBoxLayout;
    auto* sdkAdd = new QPushButton(UiLanguage::tr("Add SDK…"));
    sdkAdd->setObjectName("sdkAdd");
    auto* sdkRemove = new QPushButton(UiLanguage::tr("Remove SDK"));
    sdkRemove->setObjectName("sdkRemove");
    sdkButtons->addWidget(sdkAdd);
    sdkButtons->addWidget(sdkRemove);
    sdkPage->addRow(sdkButtons);
    connect(sdkAdd, &QPushButton::clicked, this, [this, addSdk] {
        const auto root = QFileDialog::getExistingDirectory(this, UiLanguage::tr("SDK directory"));
        if (!root.isEmpty())
            addSdk(QFileInfo(root).fileName(), root, {});
    });
    connect(sdkRemove, &QPushButton::clicked, this, [sdkTable] {
        if (sdkTable->currentRow() >= 0)
            sdkTable->removeRow(sdkTable->currentRow());
    });
    auto* markdown = page("Markdown");
    auto* mode = new QComboBox;
    mode->setObjectName("settingsMarkdownMode");
    mode->addItems({UiLanguage::tr("Source only"), UiLanguage::tr("Source and preview"),
                    UiLanguage::tr("Preview only")});
    mode->setCurrentIndex(prefs.value("markdown/mode", 1).toInt());
    markdown->addRow(UiLanguage::tr("Default mode"), mode);
    check(markdown, UiLanguage::tr("Synchronize preview scroll position"), "markdown/syncScroll", true);
    auto* git = page("Git");
    check(git, UiLanguage::tr("Show Git panel for repositories"), "git/enabled", true);
    auto* gitInfo = new QLabel(UiLanguage::tr("Local read-only view: status, diff, history and blame. No "
                                              "commit, checkout, fetch or push. Git must be installed."));
    gitInfo->setWordWrap(true);
    git->addRow(gitInfo);
    auto* keys = page(UiLanguage::tr("Keymap"));
    for (const auto& spec : workspaceShortcuts) {
        auto* edit = new QKeySequenceEdit(QKeySequence(shortcutKey(spec.id)));
        edit->setObjectName(QString("keymap/") + spec.id);
        edit->setProperty("settingsKey", edit->objectName());
        keys->addRow(QString::fromUtf8(spec.label), edit);
    }
    if (prefs.contains("settings/geometry"))
        restoreGeometry(prefs.value("settings/geometry").toByteArray());
    connect(this, &QDialog::finished, this,
            [this] { QSettings().setValue("settings/geometry", saveGeometry()); });
    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply);
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (apply())
            accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { apply(); });
    connect(sections_, &QListWidget::currentRowChanged, pages_, &QStackedWidget::setCurrentIndex);
    sections_->setCurrentRow(0);
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& query) {
        int first = -1;
        for (int i = 0; i < sections_->count(); ++i) {
            QString text = sections_->item(i)->text();
            for (auto* label : pages_->widget(i)->findChildren<QLabel*>())
                text += ' ' + label->text();
            for (auto* box : pages_->widget(i)->findChildren<QCheckBox*>())
                text += ' ' + box->text();
            for (auto* table : pages_->widget(i)->findChildren<QTableWidget*>())
                for (int row = 0; row < table->rowCount(); ++row)
                    for (int column = 0; column < table->columnCount(); ++column)
                        if (auto* item = table->item(row, column))
                            text += ' ' + item->text();
            const bool match = text.contains(query, Qt::CaseInsensitive);
            sections_->item(i)->setHidden(!match);
            if (match && first < 0)
                first = i;
        }
        if (first >= 0)
            sections_->setCurrentRow(first);
        pages_->setVisible(first >= 0);
    });
}

bool SettingsDialog::apply() {
    QSet<QString> assigned;
    for (auto* edit : findChildren<QKeySequenceEdit*>()) {
        const auto key = edit->keySequence().toString(QKeySequence::PortableText);
        if (key.isEmpty())
            continue;
        if (assigned.contains(key)) {
            QMessageBox::warning(this, UiLanguage::tr("Keymap"),
                                 UiLanguage::tr("This shortcut is assigned to several actions: ") + key);
            return false;
        }
        const QStringList reserved = {"Ctrl+S",
                                      "Ctrl+O",
                                      "Ctrl+Shift+S",
                                      "Ctrl+Shift+O",
                                      "Ctrl+Shift+A",
                                      "Ctrl+Shift+P",
                                      "Ctrl+Space",
                                      "Ctrl+Alt+B",
                                      "Ctrl+Alt+F5",
                                      "Ctrl+Return",
                                      "Ctrl+Shift+Return",
                                      "Ctrl+1",
                                      "Ctrl+2",
                                      "Ctrl+3",
                                      "Ctrl+4",
                                      "Ctrl+5",
                                      "Ctrl+6",
                                      "F8",
                                      "Shift+F8"};
        if (reserved.contains(key)) {
            QMessageBox::warning(this, UiLanguage::tr("Keymap"),
                                 UiLanguage::tr("This shortcut is already used by another command: ") + key);
            return false;
        }
        assigned.insert(key);
    }
    auto* sdkTable = findChild<QTableWidget*>("settingsSdks");
    QList<ToolDiscovery::Sdk> sdks;
    QSet<QString> variables;
    for (int row = 0; row < sdkTable->rowCount(); ++row) {
        const auto cell = [sdkTable, row](int column) {
            const auto* item = sdkTable->item(row, column);
            return item ? item->text().trimmed() : QString();
        };
        ToolDiscovery::Sdk sdk{cell(0), QDir::cleanPath(QDir::fromNativeSeparators(cell(1))), cell(2)};
        const auto variableKey = sdk.variable.toUpper();
        if (sdk.name.isEmpty() || cell(1).isEmpty() || !QDir::isAbsolutePath(sdk.root) ||
            !QDir(sdk.root).exists() ||
            (!sdk.variable.isEmpty() &&
             (!QRegularExpression("^[A-Za-z_][A-Za-z0-9_]*$").match(sdk.variable).hasMatch() ||
              variableKey == "PATH" || variables.contains(variableKey)))) {
            QMessageBox::warning(this, UiLanguage::tr("SDK"),
                                 UiLanguage::tr("Enter a name, an existing absolute directory and a unique "
                                                "environment variable (other than PATH)."));
            return false;
        }
        if (!sdk.variable.isEmpty())
            variables.insert(variableKey);
        sdks.push_back(sdk);
    }
    for (const auto& sdk : sdks) {
        const auto manifest = QDir(sdk.root).filePath("veyra-languages.json");
        if (QFileInfo::exists(manifest)) {
            std::string error;
            if (!ts::importLanguageProfiles(manifest.toStdString(), error)) {
                QMessageBox::warning(this, UiLanguage::tr("SDK"),
                                     UiLanguage::text(QString::fromStdString(error)));
                return false;
            }
        }
    }
    const auto server = findChild<QLineEdit*>("settingsClangd")->text().trimmed();

    if (auto* projectSdk = findChild<QLineEdit*>("projectSdkRoot")) {
        const auto root = projectSdk->text().trimmed();
        if (!root.isEmpty() && !QDir(root).exists()) {
            QMessageBox::warning(this, "SDK", UiLanguage::tr("SDK directory is unavailable."));
            return false;
        }
        const auto projectRoot = projectSdk->property("projectRoot").toString();
        auto metadata = ToolDiscovery::project(projectRoot);
        auto sdk = metadata.value("sdk").toObject();
        sdk["root"] = root;
        metadata["sdk"] = sdk;
        QSaveFile file(QDir(projectRoot).filePath(".veyra/project.json"));
        QDir().mkpath(QDir(projectRoot).filePath(".veyra"));
        const auto content = QJsonDocument(metadata).toJson();
        if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.commit()) {
            QMessageBox::warning(this, "SDK", file.errorString());
            return false;
        }
    }
    QSettings prefs;
    for (auto* box : findChildren<QCheckBox*>())
        if (box->property("settingsKey").isValid())
            prefs.setValue(box->property("settingsKey").toString(), box->isChecked());
    for (auto* value : findChildren<QSpinBox*>())
        if (value->property("settingsKey").isValid())
            prefs.setValue(value->property("settingsKey").toString(), value->value());
    for (auto* line : findChildren<QLineEdit*>())
        if (line->property("settingsKey").isValid())
            prefs.setValue(line->property("settingsKey").toString(), line->text().trimmed());
    for (auto* edit : findChildren<QKeySequenceEdit*>())
        prefs.setValue(edit->property("settingsKey").toString(),
                       edit->keySequence().toString(QKeySequence::PortableText));
    prefs.remove("sdk/entries");
    prefs.beginWriteArray("sdk/entries", sdks.size());
    for (int i = 0; i < sdks.size(); ++i) {
        prefs.setArrayIndex(i);
        prefs.setValue("name", sdks[i].name);
        prefs.setValue("root", sdks[i].root);
        prefs.setValue("variable", sdks[i].variable);
    }
    prefs.endArray();
    prefs.setValue("ui/language", findChild<QComboBox*>("settingsInterfaceLanguage")->currentData());
    prefs.setValue("appearance/theme", findChild<QComboBox*>("settingsTheme")->currentData());
    prefs.setValue("editor/fontFamily", findChild<QFontComboBox*>("settingsFont")->currentFont().family());
    prefs.setValue("lsp/cpp/command", server);
    prefs.setValue("markdown/mode", findChild<QComboBox*>("settingsMarkdownMode")->currentIndex());
    prefs.sync();
    if (applied_)
        applied_();
    return true;
}
