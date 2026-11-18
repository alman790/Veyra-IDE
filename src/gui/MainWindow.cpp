#include "MainWindow.hpp"
#include "../application/BackgroundJob.hpp"
#include "../application/DocumentController.hpp"
#include "CMakeProjectDialog.hpp"
#include "DebuggerPanel.hpp"
#include "GitPanel.hpp"
#include "GlassSurfaces.hpp"
#include "Keymap.hpp"
#include "SourceInspector.hpp"
#include "UiIcons.hpp"
#include "UiLanguage.hpp"
#include "UiLayout.hpp"
#include "WorkspaceControls.hpp"
#include "WorkspaceWidgets.hpp"
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringConverter>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextCursor>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>

using namespace tsui::widgets;

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    UiLanguage::initialize();
    setWindowIcon(QIcon(":/branding/veyra.png"));
    tsui::GlassStyle::setEnabled(QSettings().value("appearance/glass", true).toBool());
    setWindowTitle("Veyra IDE");
    analysisJob_ = new BackgroundJob(this);
    resize(1440, 960);
    setMinimumSize(800, 600);
    auto* root = new tsui::GlassBackdrop;
    root->setObjectName("root");
    setCentralWidget(root);
    auto* rootLayout = new QVBoxLayout(root);
    rootLayout->setSpacing(0);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    auto* header = new QToolBar(this);
    header->setObjectName("workspaceToolbar");
    header->setMovable(false);
    header->setFloatable(false);
    header->addWidget(buildIdeHeader());
    addToolBar(Qt::TopToolBarArea, header);
    auto* rail = new QToolBar(this);
    rail->setObjectName("workspaceRail");
    rail->setMovable(false);
    rail->setFloatable(false);
    rail->addWidget(buildSidebar());
    addToolBar(Qt::LeftToolBarArea, rail);
    auto* workspace = new QWidget;
    workspace->setObjectName("workbench");
    auto* workLayout = new QHBoxLayout(workspace);
    workLayout->setSpacing(0);
    workLayout->setContentsMargins(0, 0, 0, 0);

    auto* editorArea = new QWidget;
    auto* editorLayout = new QVBoxLayout(editorArea);
    editorLayout->setSpacing(0);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    pages_ = new QStackedWidget;
    pages_->setObjectName("workspacePages");
    editorLayout->addWidget(pages_, 1);
    workLayout->addWidget(editorArea, 1);
    rootLayout->addWidget(workspace, 1);
    pages_->addWidget(buildCodePage());

    codeRunner_ = new CodeRunner(this);
    lsp_ = new LspClient(this);
    sourceInspector_ = new SourceInspector(this);
    setupLocalAssistant();
    lspChangeTimer_ = new QTimer(this);
    lspChangeTimer_->setSingleShot(true);
    lspChangeTimer_->setInterval(420);
    connect(lspChangeTimer_, &QTimer::timeout, this, [this]() {
        if (lsp_)
            lsp_->changeText(editor_->toPlainText());
    });
    editor_->setCompletionRequest([this] { requestCodeCompletion(); });
    workspaceRoot_ = QSettings().value("workspace/root").toString();
    if (!workspaceRoot_.isEmpty() && !QDir(workspaceRoot_).exists())
        workspaceRoot_.clear();
    if (workspaceRoot_.isEmpty() && QFileInfo::exists(QDir::current().filePath("CMakeLists.txt")))
        workspaceRoot_ = QDir::currentPath();
    if (!workspaceRoot_.isEmpty()) {
        workspaceRoot_ = QDir(workspaceRoot_).canonicalPath();
        QSettings().setValue("workspace/root", workspaceRoot_);
    }
    setupToolWindows();
    buildNativeMenus();
    const QSettings startupSettings;
    const bool migrated = startupSettings.value("appearance/layoutVersion", 0).toInt() >= 3;
    applyTheme(migrated ? tsui::ThemeManager::fromKey(
                              startupSettings.value("appearance/theme", "darcula").toString())
                        : tsui::Theme::Darcula);
    QSettings().setValue("appearance/layoutVersion", 3);
    if (migrated && startupSettings.contains("window/ideDocks"))
        restoreState(startupSettings.value("window/ideDocks").toByteArray());
    statusBar()->showMessage(UiLanguage::tr("Ready"));
    codeAuditTimer_ = new QTimer(this);
    codeAuditTimer_->setSingleShot(true);
    codeAuditTimer_->setInterval(250);
    connect(codeAuditTimer_, &QTimer::timeout, this, &MainWindow::analyzeCode);
    connect(editor_, &QPlainTextEdit::textChanged, this, [this]() {
        editor_->setDiagnostics({});
        if (sourceInspector_)
            sourceInspector_->cancel();
        if (!switchingDocument_)
            trustedToRun_ = false;
        if (lsp_ && !lspSourcePath_.isEmpty() && lsp_->ready())
            lspChangeTimer_->start();
        editor_->setProperty("analysisCurrent", false);
        updateThreadHint();
        if (QSettings().value("threads/live", true).toBool())
            codeAuditTimer_->start();
    });
    connect(workspaceShortcut("build", this), &QShortcut::activated, this, [this]() { buildActive(false); });
    connect(workspaceShortcut("run", this), &QShortcut::activated, this, [this]() { buildActive(true); });
    connect(new QShortcut(QKeySequence("Ctrl+Alt+B"), this), &QShortcut::activated, this,
            [this]() { buildProject(false); });
    connect(new QShortcut(QKeySequence("Ctrl+Alt+F5"), this), &QShortcut::activated, this,
            [this]() { buildProject(true); });
    connect(workspaceShortcut("stop", this), &QShortcut::activated, this, &MainWindow::stopSource);
    connect(new QShortcut(QKeySequence("Ctrl+Shift+Return"), this), &QShortcut::activated, this,
            &MainWindow::analyzeCode);
    connect(new QShortcut(QKeySequence::Open, this), &QShortcut::activated, this, &MainWindow::openCode);
    connect(new QShortcut(QKeySequence::Save, this), &QShortcut::activated, this, &MainWindow::saveCode);
    connect(new QShortcut(QKeySequence::SaveAs, this), &QShortcut::activated, this, &MainWindow::saveCodeAs);
    connect(new QShortcut(QKeySequence("Ctrl+Shift+O"), this), &QShortcut::activated, this,
            &MainWindow::openProjectFolder);
    connect(new QShortcut(QKeySequence("Ctrl+Alt+N"), this), &QShortcut::activated, this,
            &MainWindow::createProject);
    connect(workspaceShortcut("find", this), &QShortcut::activated, this, [this]() {
        goToPage(2);
        if (codeSearch_) {
            codeSearchBar_->show();
            codeSearch_->setFocus();
            codeSearch_->selectAll();
        }
    });
    const QSettings settings;
    if (settings.contains("window/geometry"))
        restoreGeometry(settings.value("window/geometry").toByteArray());
    gitDock_ = new QDockWidget("Git", this);
    gitDock_->setObjectName("gitDock");
    gitPanel_ = new GitPanel(gitDock_, [this](const QString& file, int line) {
        if (loadCodeFile(file))
            editor_->jumpToLine(line);
    });
    gitDock_->setWidget(gitPanel_);
    addDockWidget(Qt::RightDockWidgetArea, gitDock_);
    gitDock_->hide();
    gitPanel_->setAvailabilityCallback([this](bool available) {
        if (auto* button = findChild<QToolButton*>("railGit"))
            button->setVisible(available && QSettings().value("git/enabled", true).toBool());
        if (!available)
            gitDock_->hide();
    });
    gitPanel_->setProject(workspaceRoot_);
    connect(editor_, &QPlainTextEdit::cursorPositionChanged, this,
            [this] { gitPanel_->setFile(sourcePath_, editor_->textCursor().blockNumber() + 1); });
    installDocumentWorkspace();
    applyPreferences();
    analyzeCode();
    goToPage(2);
    applyUiScale(settings.value("appearance/uiScale", 100).toInt());
    if (!migrated || !settings.contains("window/ideDocks"))
        QTimer::singleShot(0, this, [this] { resizeDocks({projectDock_}, {280}, Qt::Horizontal); });
    appendLog(UiLanguage::tr("Editor ready."));
}

void MainWindow::buildNativeMenus() {
    auto* file = menuBar()->addMenu(UiLanguage::tr("&File"));
    file->addAction(UiLanguage::tr("New project…"), this, &MainWindow::createProject);
    file->addAction(UiLanguage::tr("Open project folder…"), this, &MainWindow::openProjectFolder);
    file->addAction(UiLanguage::tr("New file…"), this, &MainWindow::newCode);
    file->addAction(UiLanguage::tr("Open files…"), this, &MainWindow::openCode);
    file->addSeparator();
    file->addAction(UiLanguage::tr("Save"), this, &MainWindow::saveCode);
    file->addAction(UiLanguage::tr("Save as…"), this, &MainWindow::saveCodeAs);
    file->addAction(UiLanguage::tr("Save all"), this, &MainWindow::saveAllDocuments);
    file->addSeparator();
    auto* settings = file->addAction(UiLanguage::tr("Settings…"), this, &MainWindow::showSettings);
    workspaceActionShortcut(settings, "settings");
    settings->setMenuRole(QAction::PreferencesRole);
    file->addAction(UiLanguage::tr("Exit"), this, &QWidget::close);
    auto* code = menuBar()->addMenu(UiLanguage::tr("&Code"));
    code->addAction(UiLanguage::tr("Find project file…"), this, &MainWindow::findProjectFiles);
    code->addAction(UiLanguage::tr("Find text in project…"), this, &MainWindow::findProjectText);
    workspaceActionShortcut(
        code->addAction(UiLanguage::tr("Find and replace…"), this, &MainWindow::replaceSourceText),
        "replace");
    workspaceActionShortcut(code->addAction(UiLanguage::tr("Go to line…"), this, &MainWindow::goToSourceLine),
                            "line");
    code->addAction(UiLanguage::tr("Editor font…"), this, &MainWindow::configureEditorFont);
    code->addAction(UiLanguage::tr("Thread hints"), this, &MainWindow::inspectSource);
    code->addAction(UiLanguage::tr("Discover installed SDKs…"), this, &MainWindow::showSdkDiscovery);
    code->addAction(UiLanguage::tr("Compiler / interpreter…"), this, &MainWindow::configureToolchain);
    code->addAction(UiLanguage::tr("Language server…"), this, &MainWindow::configureLanguageServer);
    code->addSeparator();
    auto* definition =
        code->addAction(UiLanguage::tr("Go to definition"), this, [this] { navigateSymbol(false); });
    definition->setShortcut(QKeySequence("F12"));
    auto* references = code->addAction(UiLanguage::tr("Find usages"), this, [this] { navigateSymbol(true); });
    references->setShortcut(QKeySequence("Shift+F12"));
    auto* rename = code->addAction(UiLanguage::tr("Rename symbol…"), this, &MainWindow::renameSymbol);
    rename->setShortcut(QKeySequence("Shift+F6"));
    auto* format = code->addAction(UiLanguage::tr("Reformat code"), this, &MainWindow::formatDocument);
    format->setShortcut(QKeySequence("Ctrl+Alt+L"));
    auto* info = code->addAction(UiLanguage::tr("Quick documentation"), this, &MainWindow::showSymbolInfo);
    info->setShortcut(QKeySequence("Ctrl+Q"));
    auto* run = menuBar()->addMenu(UiLanguage::tr("&Build"));
    run->addAction(UiLanguage::tr("Build"), this, [this] { buildActive(false); });
    run->addAction(UiLanguage::tr("Run"), this, [this] { buildActive(true); });
    auto* debug = run->addAction(UiLanguage::tr("Debug"), this, &MainWindow::startDebugging);
    debug->setShortcut(QKeySequence("F5"));
    run->addAction(UiLanguage::tr("Stop"), this, &MainWindow::stopSource);
    auto* view = menuBar()->addMenu(UiLanguage::tr("&View"));
    view->addAction(projectDock_->toggleViewAction());
    view->addAction(outputDock_->toggleViewAction());
    view->addAction(inspectionDock_->toggleViewAction());
    view->addAction(UiLanguage::tr("Find action…"), this, &MainWindow::showCommandPalette);
    view->addAction(UiLanguage::tr("Restore panel layout"), this, &MainWindow::resetWorkspace);
    view->addAction(UiLanguage::tr("Zoom in UI"), QKeySequence::ZoomIn, this,
                    [this] { applyUiScale(uiPercent_ + 10); });
    view->addAction(UiLanguage::tr("Zoom out UI"), QKeySequence::ZoomOut, this,
                    [this] { applyUiScale(uiPercent_ - 10); });
    glassToggle_ = view->addAction(UiLanguage::tr("Glass surfaces"));
    glassToggle_->setCheckable(true);
    glassToggle_->setChecked(tsui::GlassStyle::enabled());
    connect(glassToggle_, &QAction::toggled, this, &MainWindow::setGlassEnabled);
}

void MainWindow::goToPage(int) {
    editor_->setFocus();
    updateWindowTitle();
}

void MainWindow::updateWindowTitle() {
    const auto project = workspaceRoot_.isEmpty() ? QString("Veyra IDE")
                                                  : QFileInfo(workspaceRoot_).fileName() + " — Veyra IDE";
    if (sourceDisplayName_.isEmpty()) {
        setWindowTitle(project);
        return;
    }
    setWindowTitle(sourceDisplayName_ + (editor_ && editor_->document()->isModified() ? " *" : "") + " — " +
                   project);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (codeRunner_ && codeRunner_->busy()) {
        closing_ = true;
        codeRunner_->stop();
        QTimer::singleShot(50, this, [this] { close(); });
        event->ignore();
        return;
    }
    if (debugger_ && debugger_->busy()) {
        debugger_->stop();
        QTimer::singleShot(100, this, [this] { close(); });
        event->ignore();
        return;
    }
    if (localModelJob_ && localModelJob_->busy()) {
        closing_ = true;
        localModelJob_->cancel();
        QTimer::singleShot(50, this, [this] { close(); });
        event->ignore();
        return;
    }
    if (analysisJob_->busy()) {
        closing_ = true;
        analysisPending_ = false;
        analysisJob_->cancel();
        QTimer::singleShot(50, this, [this] { close(); });
        event->ignore();
        return;
    }
    if (!confirmDiscardCode()) {
        closing_ = false;
        event->ignore();
        return;
    }
    if (sourceInspector_)
        sourceInspector_->cancel();
    if (lsp_)
        lsp_->stop();
    if (documents_) {
        storeEditorState();
        QString issue;
        if (!documents_->saveSession(sessionPath_, issue))
            appendLog("Session: " + issue);
    }
    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/page", pages_->currentIndex());
    settings.setValue("window/ideDocks", saveState());
    QMainWindow::closeEvent(event);
}

QWidget* MainWindow::buildSidebar() {
    auto* side = new QWidget;
    side->setObjectName("sidebar");
    side->setFixedWidth(44);
    side->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto* layout = new QVBoxLayout(side);
    layout->setContentsMargins(4, 8, 4, 8);
    layout->setSpacing(8);
    auto* files = new WorkspaceToolButton;
    files->setObjectName("railProject");
    files->setIconSize(QSize(20, 20));
    files->setIcon(tsui::icon("folder"));
    files->setToolTip(UiLanguage::tr("Project files · Alt+1"));
    files->setAccessibleName(UiLanguage::tr("Project files"));
    files->setCheckable(true);
    files->setChecked(true);
    layout->addWidget(files);
    connect(files, &QToolButton::clicked, this, [this] {
        if (projectDock_)
            projectDock_->setVisible(!projectDock_->isVisible());
    });
    auto* code = new WorkspaceToolButton;
    code->setObjectName("railEditor");
    code->setIconSize(QSize(20, 20));
    code->setIcon(tsui::icon("code"));
    code->setToolTip(UiLanguage::tr("Editor · Ctrl+3"));
    code->setAccessibleName(UiLanguage::tr("Editor"));
    layout->addWidget(code);
    connect(code, &QToolButton::clicked, this, [this] { goToPage(2); });
    auto* git = new WorkspaceToolButton;
    git->setObjectName("railGit");
    git->setText("Git");
    git->setToolTip(UiLanguage::tr("Git: changes, history and line authorship"));
    git->setAccessibleName("Git");
    layout->addWidget(git);
    connect(git, &QToolButton::clicked, this, [this] {
        if (gitDock_) {
            gitDock_->setVisible(!gitDock_->isVisible());
            if (gitDock_->isVisible()) {
                gitPanel_->setProject(workspaceRoot_);
                gitPanel_->setFile(sourcePath_, editor_->textCursor().blockNumber() + 1);
                gitPanel_->refresh();
            }
        }
    });
    layout->addStretch();
    auto* output = new WorkspaceToolButton;
    output->setObjectName("railConsole");
    output->setIcon(tsui::icon("console"));
    output->setToolTip(UiLanguage::tr("Console and diagnostics"));
    output->setAccessibleName(UiLanguage::tr("Console"));
    layout->addWidget(output);
    connect(output, &QToolButton::clicked, this, [this] {
        if (outputDock_)
            outputDock_->setVisible(!outputDock_->isVisible());
    });
    return side;
}

void MainWindow::openProjectFolder() {
    const QString folder = QFileDialog::getExistingDirectory(
        this, "Open project folder", workspaceRoot_.isEmpty() ? QDir::homePath() : workspaceRoot_);
    if (folder.isEmpty())
        return;
    workspaceRoot_ = QDir(folder).canonicalPath();
    QSettings().setValue("workspace/root", workspaceRoot_);
    updateProjectTree();
    syncLanguage(true);
    goToPage(2);
    statusBar()->showMessage("Workspace: " + workspaceRoot_, 7000);
}
