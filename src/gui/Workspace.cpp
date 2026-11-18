#include "DebuggerPanel.hpp"
#include "GitPanel.hpp"
#include "GlassSurfaces.hpp"
#include "Keymap.hpp"
#include "MainWindow.hpp"
#include "ProjectFileModel.hpp"
#include "TerminalWidget.hpp"
#include "ThemeManager.hpp"
#include "UiIcons.hpp"
#include "UiLanguage.hpp"
#include "UiLayout.hpp"
#include "WorkspaceControls.hpp"
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFont>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGraphicsDropShadowEffect>
#include <QHeaderView>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <QVariant>
#include <QWidgetAction>
#include <algorithm>
#include <utility>

void MainWindow::inspectSource() {
    goToPage(2);
    if (inspectionDock_)
        inspectionDock_->show();
    analyzeCode();
    reviewThreadWithModel(true);
}

QWidget* MainWindow::buildIdeHeader() {
    auto* bar = new tsui::FrostedHeader;
    bar->setObjectName("ideHeader");
    bar->setMinimumHeight(44);
    bar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(12, 4, 10, 4);
    row->setSpacing(9);
    auto* mark = new QLabel;
    mark->setObjectName("applicationMark");
    mark->setFixedSize(26, 26);
    mark->setAlignment(Qt::AlignCenter);
    mark->setText({});
    mark->setPixmap(QPixmap(":/branding/veyra.png"));
    mark->setScaledContents(true);
    row->addWidget(mark);
    auto* project = new HoverToolButton;
    project->setObjectName("projectMenu");
    project->setText(UiLanguage::tr("Project"));
    project->setToolTip(UiLanguage::tr("Open project or file"));
    project->setAccessibleName(UiLanguage::tr("Project"));
    auto* files = new QMenu(project);
    files->addAction(UiLanguage::tr("New project…"), this, &MainWindow::createProject);
    files->addAction(UiLanguage::tr("Open project folder…"), this, &MainWindow::openProjectFolder);
    files->addAction(UiLanguage::tr("Open files…"), this, &MainWindow::openCode);
    files->addAction(UiLanguage::tr("New file"), this, &MainWindow::newCode);
    files->addSeparator();
    files->addAction(UiLanguage::tr("Save all"), this, &MainWindow::saveAllDocuments);
    project->setMenu(files);
    row->addWidget(project);
    projectName_ = new QLabel("Veyra IDE");
    projectName_->setObjectName("projectName");
    projectName_->setMinimumWidth(1);
    projectName_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    projectName_->setMaximumWidth(240);
    row->addWidget(projectName_);
    activeView_ = new QLabel(UiLanguage::tr("Editor"));
    activeView_->setProperty("class", "caption");
    activeView_->setMinimumWidth(1);
    activeView_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    activeView_->setMaximumWidth(120);
    row->addWidget(activeView_);
    row->addStretch();
    auto* controls = new QWidget;
    controls->setMaximumWidth(460);
    auto* controlsRow = new tsui::FlowLayout(controls);
    controlsRow->setSpacing(5);
    row->addWidget(controls);
    runContext_ = new QComboBox;
    runContext_->setObjectName("runConfiguration");
    runContext_->addItem(UiLanguage::tr("File"), false);
    runContext_->addItem("CMake", true);
    runContext_->setFixedWidth(100);
    runContext_->setToolTip(UiLanguage::tr(
        "Build/run context. For CMake choose a target and configuration in the project dialog."));
    runContext_->setAccessibleName(UiLanguage::tr("Run configuration"));
    controlsRow->addWidget(runContext_);
    const auto button = [&](const QString& name, const QString& icon, const QString& tip) {
        auto* value = new QPushButton;
        value->setObjectName(name);
        value->setProperty("class", "toolbarIcon");
        value->setProperty("tsIcon", icon);
        value->setIcon(tsui::icon(icon));
        value->setIconSize(QSize(19, 19));
        value->setFixedSize(32, 32);
        value->setToolTip(tip);
        value->setAccessibleName(tip);
        controlsRow->addWidget(value);
        return value;
    };
    sourceBuild_ = button("buildCurrent", "build", UiLanguage::tr("Build selected configuration · Ctrl+B"));
    sourceRun_ = button("runCurrent", "play", UiLanguage::tr("Run selected configuration · Ctrl+F5"));
    sourceStop_ = button("stopCurrent", "stop", UiLanguage::tr("Stop program · Shift+F5"));
    sourceStop_->setEnabled(false);
    connect(sourceBuild_, &QPushButton::clicked, this, [this] { buildActive(false); });
    connect(sourceRun_, &QPushButton::clicked, this, [this] { buildActive(true); });
    connect(sourceStop_, &QPushButton::clicked, this, &MainWindow::stopSource);
    auto* inspect = button("inspectionTools", "inspect", UiLanguage::tr("Thread hints"));
    connect(inspect, &QPushButton::clicked, this, &MainWindow::inspectSource);
    auto* commands = button("ideCommand", "search", UiLanguage::tr("Find action · Ctrl+K / Ctrl+Shift+A"));
    connect(commands, &QPushButton::clicked, this, &MainWindow::showCommandPalette);
    auto* settings = button("appearanceTools", "settings", UiLanguage::tr("Settings · Ctrl+,"));
    connect(settings, &QPushButton::clicked, this, &MainWindow::showSettings);

    scaleSelector_ = new QComboBox(bar);
    scaleSelector_->setObjectName("uiScaleSelector");
    for (int percent : {85, 100, 110, 125, 150, 175, 200})
        scaleSelector_->addItem(QString::number(percent) + "%", percent);
    scaleSelector_->hide();
    themeSelector_ = new QComboBox(bar);
    themeSelector_->setObjectName("themeSelector");
    for (auto theme : {tsui::Theme::Darcula, tsui::Theme::Light, tsui::Theme::HighContrast})
        themeSelector_->addItem(tsui::ThemeManager::displayName(theme), tsui::ThemeManager::key(theme));
    themeSelector_->hide();
    connect(scaleSelector_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index >= 0)
            applyUiScale(scaleSelector_->itemData(index).toInt());
    });
    connect(themeSelector_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index >= 0)
            applyTheme(tsui::ThemeManager::fromKey(themeSelector_->itemData(index).toString()));
    });
    connect(workspaceShortcut("commands", this), &QShortcut::activated, this,
            &MainWindow::showCommandPalette);
    for (const auto* key : {"Ctrl+Shift+A", "Ctrl+Shift+P"})
        connect(new QShortcut(QKeySequence(key), this), &QShortcut::activated, this,
                &MainWindow::showCommandPalette);
    return bar;
}

void MainWindow::applyUiScale(int percent) {
    percent = std::clamp(percent, 85, 200);
    auto widgets = findChildren<QWidget*>();

    for (auto* widget : widgets) {
        if (widget->property("tsBaseMinimum").isValid())
            continue;
        widget->setProperty("tsBaseMinimum", widget->minimumSize());
        widget->setProperty("tsBaseMaximum", widget->maximumSize());
        if (auto* button = qobject_cast<QAbstractButton*>(widget))
            widget->setProperty("tsBaseIcon", button->iconSize());
    }
    for (auto* layout : findChildren<QLayout*>()) {
        if (layout->property("tsBaseMargins").isValid())
            continue;
        const auto m = layout->contentsMargins();
        layout->setProperty("tsBaseMargins", QVariantList{m.left(), m.top(), m.right(), m.bottom()});
        layout->setProperty("tsBaseSpacing", layout->spacing());
    }
    uiPercent_ = percent;
    const double factor = percent / 100.0;
    tsui::ThemeManager::setScale(factor);
    auto dimension = [factor](int value) {
        return value >= QWIDGETSIZE_MAX ? QWIDGETSIZE_MAX : qRound(value * factor);
    };
    for (auto* widget : widgets) {
        const auto minimum = widget->property("tsBaseMinimum").toSize();
        const auto maximum = widget->property("tsBaseMaximum").toSize();
        if (qobject_cast<QSplitter*>(widget)) {

            widget->setMinimumSize(0, 0);
        } else if (widget->layout() && !qobject_cast<QTabWidget*>(widget)) {
            const bool fixedWidth = minimum.width() == maximum.width();
            const bool fixedHeight = minimum.height() == maximum.height();
            const int contentWidth = widget->objectName().endsWith("Content") ? dimension(540) : 0;
            widget->setMinimumSize(fixedWidth ? dimension(minimum.width()) : contentWidth,
                                   fixedHeight ? dimension(minimum.height()) : 0);
            widget->setMaximumSize(dimension(maximum.width()), dimension(maximum.height()));
        } else {
            widget->setMinimumSize(dimension(minimum.width()), dimension(minimum.height()));
            widget->setMaximumSize(dimension(maximum.width()), dimension(maximum.height()));
        }
        if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
            const auto icon = widget->property("tsBaseIcon").toSize();
            button->setIconSize(QSize(dimension(icon.width()), dimension(icon.height())));
        }
    }
    for (auto* layout : findChildren<QLayout*>()) {
        const auto m = layout->property("tsBaseMargins").toList();
        if (m.size() == 4)
            layout->setContentsMargins(qRound(m[0].toInt() * factor), qRound(m[1].toInt() * factor),
                                       qRound(m[2].toInt() * factor), qRound(m[3].toInt() * factor));
        const int spacing = layout->property("tsBaseSpacing").toInt();
        if (spacing >= 0)
            layout->setSpacing(qRound(spacing * factor));
    }
    for (auto* table : findChildren<QTableWidget*>()) {
        table->verticalHeader()->setDefaultSectionSize(qRound(38 * factor));
        table->horizontalHeader()->setMinimumSectionSize(qRound(65 * factor));
    }
    if (editor_) {
        QSettings settings;
        QFont font(
            settings.value("editor/fontFamily", QFontDatabase::systemFont(QFontDatabase::FixedFont).family())
                .toString());
        font.setStyleHint(QFont::Monospace);
        font.setPixelSize(qRound(settings.value("editor/basePixelSize", 14).toInt() * factor));
        editor_->setFont(font);
        editor_->setTabStopDistance(QFontMetrics(font).horizontalAdvance(' ') *
                                    std::clamp(settings.value("editor/tabWidth", 4).toInt(), 2, 8));
        editor_->refreshTheme();
    }
    if (scaleSelector_) {
        QSignalBlocker block(scaleSelector_);
        int index = scaleSelector_->findData(percent);
        if (index < 0) {
            scaleSelector_->addItem(QString::number(percent) + "%", percent);
            index = scaleSelector_->count() - 1;
        }
        scaleSelector_->setCurrentIndex(index);
    }
    QSettings().setValue("appearance/uiScale", percent);
    for (auto* widget : widgets) {
        widget->updateGeometry();
        widget->update();
    }
}

void MainWindow::resetWorkspace() {
    addDockWidget(Qt::LeftDockWidgetArea, projectDock_);
    projectDock_->setFloating(false);
    addDockWidget(Qt::BottomDockWidgetArea, outputDock_);
    outputDock_->setFloating(false);
    projectDock_->show();
    outputDock_->hide();
    if (inspectionDock_)
        inspectionDock_->hide();
    resizeDocks({projectDock_}, {240}, Qt::Horizontal);
    resizeDocks({outputDock_}, {200}, Qt::Vertical);
}

void MainWindow::setupToolWindows() {
    setDockOptions(QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks |
                   QMainWindow::AnimatedDocks);
    projectDock_ = new QDockWidget(UiLanguage::tr("Project files"), this);
    projectDock_->setObjectName("ideProjectDock");
    projectDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    projectTree_ = new QTreeView;
    projectFiles_ = new QFileSystemModel(this);
    projectFiles_->setReadOnly(true);
    projectFiles_->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);
    projectFilter_ = new ProjectFileFilter(this);
    projectFilter_->setSourceModel(projectFiles_);
    projectTree_->setModel(projectFilter_);
    projectTree_->setUniformRowHeights(true);
    projectTree_->setSelectionBehavior(QAbstractItemView::SelectItems);
    projectTree_->setSelectionMode(QAbstractItemView::SingleSelection);
    projectTree_->setAnimated(false);
    projectTree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    for (int column = 1; column < 4; ++column)
        projectTree_->hideColumn(column);
    projectTree_->setHeaderHidden(true);
    projectTree_->setIndentation(16);
    projectTree_->setTextElideMode(Qt::ElideMiddle);
    connect(projectFiles_, &QFileSystemModel::directoryLoaded, this, [this](const QString& path) {
        if (!sourcePath_.isEmpty() && QFileInfo(sourcePath_).absolutePath() == path)
            revealActiveFile();
    });
    projectTree_->setObjectName("ideProjectTree");
    projectTree_->setHeaderHidden(true);
    auto* projectPanel = new QWidget;
    auto* projectLayout = new QVBoxLayout(projectPanel);
    projectLayout->setContentsMargins(0, 0, 0, 0);
    projectLayout->setSpacing(4);
    auto* actions = new QHBoxLayout;
    actions->setContentsMargins(6, 4, 6, 0);
    auto* file = new QPushButton(UiLanguage::tr("+ File"));
    file->setObjectName("projectNewFile");
    auto* folder = new QPushButton(UiLanguage::tr("+ Folder"));
    folder->setObjectName("projectNewFolder");
    auto* view = new QComboBox;
    view->setObjectName("projectFileView");
    view->addItems({UiLanguage::tr("Working files"), UiLanguage::tr("All files")});
    actions->addWidget(file);
    actions->addWidget(folder);
    actions->addWidget(view, 1);
    projectLayout->addLayout(actions);
    projectQuery_ = new QLineEdit;
    projectQuery_->setObjectName("projectFileFilter");
    projectQuery_->setPlaceholderText(UiLanguage::tr("Filter · Enter to search the entire project"));
    projectLayout->addWidget(projectQuery_);
    projectLayout->addWidget(projectTree_, 1);
    projectTree_->setSortingEnabled(true);
    projectTree_->sortByColumn(0, Qt::AscendingOrder);
    projectDock_->setWidget(projectPanel);
    connect(file, &QPushButton::clicked, this, [this] { createProjectEntry(false); });
    connect(folder, &QPushButton::clicked, this, [this] { createProjectEntry(true); });
    connect(view, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int index) { projectFilter_->setFocused(index == 0); });
    connect(projectQuery_, &QLineEdit::textChanged, projectFilter_, &ProjectFileFilter::setQuery);
    connect(projectQuery_, &QLineEdit::returnPressed, this, &MainWindow::findProjectFiles);
    addDockWidget(Qt::LeftDockWidgetArea, projectDock_);
    projectDock_->setMinimumWidth(150);
    connect(projectTree_, &QTreeView::clicked, this, [this](const QModelIndex& index) {
        const auto source = projectFilter_->mapToSource(index);
        if (!projectFiles_->isDir(source))
            loadCodeFile(projectFiles_->filePath(source));
    });
    connect(projectDock_, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (auto* button = findChild<QToolButton*>("railProject"))
            button->setChecked(visible);
    });
    connect(workspaceShortcut("project", this), &QShortcut::activated, this,
            [this] { projectDock_->setVisible(!projectDock_->isVisible()); });
    outputDock_ = new QDockWidget(UiLanguage::tr("Output and diagnostics"), this);
    outputDock_->setObjectName("ideOutputDock");
    outputDock_->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    auto* tabs = new QTabWidget;
    tabs->setObjectName("ideOutputTabs");
    runConsole_ = new QPlainTextEdit;
    runConsole_->setObjectName("ideConsole");
    runConsole_->setReadOnly(true);
    runConsole_->document()->setMaximumBlockCount(400);
    runConsole_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    workbenchProblems_ = new QListWidget;
    workbenchProblems_->setObjectName("ideProblems");
    auto* console = new QWidget;
    auto* consoleLayout = new QVBoxLayout(console);
    consoleLayout->setContentsMargins(0, 0, 0, 0);
    consoleLayout->addWidget(runConsole_, 1);
    runInput_ = new QLineEdit;
    runInput_->setObjectName("programInput");
    runInput_->setPlaceholderText(UiLanguage::tr("Running program stdin · Enter"));
    runInput_->setEnabled(false);
    consoleLayout->addWidget(runInput_);
    connect(runInput_, &QLineEdit::returnPressed, this, [this] {
        consumeBuildOutput(runInput_->text() + "\n");
        codeRunner_->sendInput(runInput_->text());
        runInput_->clear();
    });
    connect(codeRunner_, &CodeRunner::stateChanged, this,
            [this](bool active, bool program) { runInput_->setEnabled(active && program); });
    tabs->addTab(console, "Run / Build / Console");
    tabs->addTab(new TerminalWidget(
                     [this] {
                         return workspaceRoot_.isEmpty() ? QFileInfo(sourcePath_).absolutePath()
                                                         : workspaceRoot_;
                     },
                     tabs),
                 UiLanguage::tr("Terminal"));
    tabs->addTab(workbenchProblems_, UiLanguage::tr("Problems"));
    connect(workbenchProblems_, &QListWidget::itemActivated, this, &MainWindow::jumpToProblemItem);
    connect(workbenchProblems_, &QListWidget::itemClicked, this, &MainWindow::jumpToProblemItem);
    auto navigateProblem = [this](int step) {
        if (debugger_ && debugger_->busy()) {
            debugger_->send({{"command", step > 0 ? "next" : "stepOut"}});
            return;
        }
        if (pages_ && pages_->currentIndex() != 0)
            goToPage(2);
        if (!workbenchProblems_ || !workbenchProblems_->count())
            return;
        const int count = workbenchProblems_->count();
        int next = workbenchProblems_->currentRow();
        if (next < 0)
            next = step > 0 ? -1 : 0;
        next = (next + step + count) % count;
        workbenchProblems_->setCurrentRow(next);
        auto* item = workbenchProblems_->item(next);
        jumpToProblemItem(item);
    };
    connect(new QShortcut(QKeySequence("F8"), this), &QShortcut::activated, this,
            [navigateProblem]() { navigateProblem(1); });
    connect(new QShortcut(QKeySequence("Shift+F8"), this), &QShortcut::activated, this,
            [navigateProblem]() { navigateProblem(-1); });

    outputDock_->setWidget(tabs);
    addDockWidget(Qt::BottomDockWidgetArea, outputDock_);
    resizeDocks({projectDock_}, {260}, Qt::Horizontal);
    resizeDocks({outputDock_}, {156}, Qt::Vertical);
    projectDock_->show();
    outputDock_->hide();
    updateProjectTree();
}

void MainWindow::updateProjectTree() {
    if (!projectTree_ || !projectFiles_)
        return;
    QString folder = workspaceRoot_;
    if (folder.isEmpty() && !sourcePath_.isEmpty())
        folder = QFileInfo(sourcePath_).absolutePath();
    if (folder.isEmpty() || !QDir(folder).exists()) {
        projectTree_->setRootIndex({});
        projectTree_->hide();
        if (projectName_)
            projectName_->setText(UiLanguage::tr("Open project…"));
        return;
    }
    projectTree_->show();
    const auto absolute = QDir(folder).canonicalPath();
    if (runContext_ && runContext_->property("root").toString() != absolute) {
        runContext_->setProperty("root", absolute);
        runContext_->setCurrentIndex(QFileInfo::exists(QDir(absolute).filePath("CMakeLists.txt")) ? 1 : 0);
    }
    const auto source = projectFiles_->setRootPath(absolute);
    projectTree_->setRootIndex(projectFilter_->mapFromSource(source));
    if (projectName_) {
        projectName_->setText(QFileInfo(absolute).fileName());
        projectName_->setToolTip(absolute);
    }
    projectDock_->setWindowTitle(UiLanguage::tr("Project"));
    revealActiveFile();
    if (gitPanel_)
        gitPanel_->setProject(absolute);
}

void MainWindow::revealActiveFile() {
    if (!projectFiles_ || sourcePath_.isEmpty() ||
        (!workspaceRoot_.isEmpty() && QDir(workspaceRoot_).relativeFilePath(sourcePath_).startsWith("../")))
        return;
    projectFilter_->setActivePath(sourcePath_);
    const auto index = projectFilter_->mapFromSource(projectFiles_->index(sourcePath_));
    if (index.isValid()) {
        for (auto parent = index.parent(); parent.isValid() && parent != projectTree_->rootIndex();
             parent = parent.parent())
            projectTree_->expand(parent);
        projectTree_->setCurrentIndex(index);
        projectTree_->scrollTo(index);
    }
}

void MainWindow::jumpToProblemItem(QListWidgetItem* item) {
    if (!item || !editor_)
        return;
    const int line = item->data(Qt::UserRole).toInt();
    if (line < 1)
        return;
    const QString issueFile = item->data(Qt::UserRole + 1).toString();
    if (!issueFile.isEmpty()) {
        QFileInfo filename(issueFile);
        if (filename.isRelative()) {
            QString root = workspaceRoot_;
            if (root.isEmpty())
                root = QString::fromStdString(ts::findCMakeRoot(sourcePath_.toStdString()));
            if (root.isEmpty())
                root = QFileInfo(sourcePath_).absolutePath();
            filename = QFileInfo(QDir(root).filePath(issueFile));
        }
        if (filename.isFile() && filename.absoluteFilePath() != QFileInfo(sourcePath_).absoluteFilePath()) {
            if (!loadCodeFile(filename.absoluteFilePath()))
                return;
        }
    }
    goToPage(2);
    editor_->jumpToLine(line);
}

void MainWindow::appendLog(const QString& message) {
    if (runConsole_)
        runConsole_->appendPlainText(message);
}

void MainWindow::applyTheme(tsui::Theme theme) {
    tsui::ThemeManager::apply(theme);
    QSettings settings;
    settings.setValue("appearance/theme", tsui::ThemeManager::key(theme));
    if (themeSelector_) {
        QSignalBlocker guard(themeSelector_);
        const int index = themeSelector_->findData(tsui::ThemeManager::key(theme));
        if (index >= 0)
            themeSelector_->setCurrentIndex(index);
    }
    if (editor_)
        editor_->refreshTheme();
    const QColor iconColor = tsui::ThemeManager::color(tsui::Role::Muted);
    for (auto* control : findChildren<QAbstractButton*>()) {
        const QString key = control->property("tsIcon").toString();
        if (!key.isEmpty())
            control->setIcon(tsui::icon(key, key == "play"   ? tsui::ThemeManager::color(tsui::Role::Good)
                                             : key == "stop" ? tsui::ThemeManager::color(tsui::Role::Error)
                                                             : iconColor));
    }
    const auto widgets = findChildren<QWidget*>();
    for (QWidget* widget : widgets)
        widget->update();
    appendLog("Theme: " + tsui::ThemeManager::displayName(theme));
}

void MainWindow::setGlassEnabled(bool enabled) {
    tsui::GlassStyle::setEnabled(enabled);
    QSettings().setValue("appearance/glass", enabled);
    if (glassToggle_ && glassToggle_->isChecked() != enabled) {
        QSignalBlocker block(glassToggle_);
        glassToggle_->setChecked(enabled);
    }
    tsui::ThemeManager::apply(tsui::ThemeManager::current());
    update();
    for (auto* widget : findChildren<QWidget*>())
        widget->update();
    statusBar()->showMessage(
        enabled ? UiLanguage::tr("Glass surfaces enabled") : UiLanguage::tr("Opaque surfaces enabled"), 3000);
}

void MainWindow::showCommandPalette() {
    QDialog dialog(this);
    dialog.setWindowTitle("Veyra actions");
    dialog.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    dialog.setAttribute(Qt::WA_TranslucentBackground);
    dialog.resize(qRound(640 * tsui::ThemeManager::scale()), qRound(470 * tsui::ThemeManager::scale()));
    auto* frame = new tsui::GlassPopupSurface(&dialog);
    frame->setObjectName("commandSurface");
    frame->setStyleSheet("QFrame#commandSurface{background:transparent;border:0;}");
    auto* shadow = new QGraphicsDropShadowEffect(frame);
    shadow->setBlurRadius(28);
    shadow->setOffset(0, 9);
    shadow->setColor(QColor(0, 0, 0, 95));
    frame->setGraphicsEffect(shadow);
    auto* outer = new QVBoxLayout(&dialog);
    outer->setContentsMargins(16, 16, 16, 20);
    outer->addWidget(frame);
    auto* content = new QVBoxLayout(frame);
    content->setContentsMargins(13, 13, 13, 11);
    content->setSpacing(7);
    auto* input = new QLineEdit(frame);
    input->setObjectName("commandSearch");
    input->setPlaceholderText(UiLanguage::tr("Action or tool name…"));
    input->setAccessibleName("Search Veyra commands");
    content->addWidget(input);
    auto* list = new QListWidget(frame);
    list->setObjectName("commandResults");
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    content->addWidget(list, 1);
    const QStringList commands = {UiLanguage::tr("New project…"),
                                  UiLanguage::tr("Open project folder…"),
                                  UiLanguage::tr("New file…"),
                                  UiLanguage::tr("Build"),
                                  UiLanguage::tr("Run"),
                                  UiLanguage::tr("Stop"),
                                  UiLanguage::tr("Thread hints"),
                                  UiLanguage::tr("Settings…"),
                                  UiLanguage::tr("Find project file…"),
                                  UiLanguage::tr("Find text in project…")};
    for (int i = 0; i < commands.size(); ++i) {
        if (commands[i].isEmpty())
            continue;
        auto* item = new QListWidgetItem(commands[i], list);
        item->setData(Qt::UserRole, i);
    }
    list->setCurrentRow(0);
    auto selectFirstVisible = [list]() {
        for (int i = 0; i < list->count(); ++i) {
            if (!list->item(i)->isHidden()) {
                list->setCurrentRow(i);
                return;
            }
        }
        list->setCurrentRow(-1);
    };
    connect(input, &QLineEdit::textChanged, &dialog, [list, selectFirstVisible](const QString& query) {
        for (int i = 0; i < list->count(); ++i)
            list->item(i)->setHidden(!list->item(i)->text().contains(query, Qt::CaseInsensitive));
        selectFirstVisible();
    });
    auto execute = [this, &dialog](QListWidgetItem* item) {
        if (!item || item->isHidden())
            return;
        const int command = item->data(Qt::UserRole).toInt();
        dialog.accept();
        switch (command) {
            case 0:
                createProject();
                break;
            case 1:
                openProjectFolder();
                break;
            case 2:
                newCode();
                break;
            case 3:
                buildActive(false);
                break;
            case 4:
                buildActive(true);
                break;
            case 5:
                stopSource();
                break;
            case 6:
                inspectSource();
                break;
            case 7:
                showSettings();
                break;
            case 8:
                findProjectFiles();
                break;
            case 9:
                findProjectText();
                break;
        }
    };
    connect(list, &QListWidget::itemActivated, &dialog, execute);
    connect(list, &QListWidget::itemDoubleClicked, &dialog, execute);
    connect(input, &QLineEdit::returnPressed, &dialog, [list, execute]() { execute(list->currentItem()); });
    auto* down = new QShortcut(QKeySequence(Qt::Key_Down), &dialog);
    down->setContext(Qt::WidgetWithChildrenShortcut);
    connect(down, &QShortcut::activated, &dialog, [list]() {
        int row = list->currentRow();
        for (int i = row + 1; i < list->count(); ++i)
            if (!list->item(i)->isHidden()) {
                list->setCurrentRow(i);
                break;
            }
    });
    auto* up = new QShortcut(QKeySequence(Qt::Key_Up), &dialog);
    up->setContext(Qt::WidgetWithChildrenShortcut);
    connect(up, &QShortcut::activated, &dialog, [list]() {
        for (int i = list->currentRow() - 1; i >= 0; --i)
            if (!list->item(i)->isHidden()) {
                list->setCurrentRow(i);
                break;
            }
    });
    dialog.move(mapToGlobal(QPoint(qMax(0, (width() - dialog.width()) / 2), 50)));
    QTimer::singleShot(0, &dialog, [input]() { input->setFocus(); });
    dialog.exec();
}
