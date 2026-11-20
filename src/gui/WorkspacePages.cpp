#include "../application/BackgroundJob.hpp"
#include "../application/DocumentController.hpp"
#include "CMakeProjectDialog.hpp"
#include "GlassSurfaces.hpp"
#include "MainWindow.hpp"
#include "MarkdownPreview.hpp"
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
#include <QScrollBar>
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
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>

using namespace tsui::widgets;

QWidget* MainWindow::buildCodePage() {
    auto* page = new QWidget;
    page->setObjectName("codePage");
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    documentTabs_ = new DocumentTabBar;
    documentTabs_->setObjectName("documentTabs");
    documentTabs_->setDocumentMode(true);
    documentTabs_->setTabsClosable(true);
    documentTabs_->setMovable(true);
    documentTabs_->setExpanding(false);
    documentTabs_->setUsesScrollButtons(true);
    documentTabs_->setElideMode(Qt::ElideMiddle);
    layout->addWidget(documentTabs_);
    auto* breadcrumb = new QWidget;
    breadcrumb->setObjectName("editorBreadcrumb");
    auto* pathRow = new QHBoxLayout(breadcrumb);
    pathRow->setContentsMargins(14, 4, 12, 4);
    pathRow->setSpacing(8);
    sourcePathLabel_ = label("untitled.cpp", "caption");
    sourcePathLabel_->setObjectName("sourceBreadcrumbPath");
    sourcePathLabel_->setWordWrap(false);
    sourcePathLabel_->setMinimumWidth(0);
    sourcePathLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    sourcePathLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pathRow->addWidget(sourcePathLabel_, 1);
    markdownMode_ = new QComboBox;
    markdownMode_->setObjectName("markdownMode");
    markdownMode_->addItems(
        {UiLanguage::tr("Source"), UiLanguage::tr("Editor + preview"), UiLanguage::tr("Preview")});
    markdownMode_->setToolTip(UiLanguage::tr("Markdown mode"));
    pathRow->addWidget(markdownMode_);
    markdownMode_->hide();
    connect(markdownMode_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { updateMarkdown(); });
    auto* locate = new QToolButton;
    locate->setFixedSize(24, 24);
    locate->setIconSize(QSize(16, 16));
    locate->setIcon(tsui::icon("folder"));
    locate->setToolTip(UiLanguage::tr("Reveal current file in project"));
    locate->setAccessibleName(UiLanguage::tr("Reveal current file in project"));
    pathRow->addWidget(locate);
    connect(locate, &QToolButton::clicked, this, [this] {
        projectDock_->show();
        revealActiveFile();
    });
    layout->addWidget(breadcrumb);
    codeSearchBar_ = new QWidget;
    codeSearchBar_->setObjectName("sourceSearchBar");
    auto* searchRow = new QHBoxLayout(codeSearchBar_);
    searchRow->setContentsMargins(12, 5, 12, 5);
    codeSearch_ = new QLineEdit;
    codeSearch_->setObjectName("sourceSearch");
    codeSearch_->setPlaceholderText(UiLanguage::tr("Find in file · Ctrl+F"));
    searchRow->addWidget(codeSearch_, 1);
    auto* find = new QToolButton;
    find->setText(UiLanguage::tr("Next"));
    searchRow->addWidget(find);
    auto* closeSearch = new QToolButton;
    closeSearch->setText("×");
    closeSearch->setToolTip(UiLanguage::tr("Close search · Escape"));
    searchRow->addWidget(closeSearch);
    layout->addWidget(codeSearchBar_);
    codeSearchBar_->hide();
    editor_ = new CodeEditor;
    editor_->setMinimumSize(180, 180);
    documentSplit_ = new QSplitter(Qt::Horizontal);
    documentSplit_->setObjectName("documentSplit");
    documentSplit_->addWidget(editor_);
    markdownPreview_ = new MarkdownPreview;
    documentSplit_->addWidget(markdownPreview_);
    markdownPreview_->hide();
    documentSplit_->setSizes({600, 600});
    layout->addWidget(documentSplit_, 1);
    emptyEditor_ = new QWidget;
    emptyEditor_->setObjectName("emptyEditor");
    auto* welcome = new QVBoxLayout(emptyEditor_);
    welcome->setContentsMargins(60, 50, 60, 50);
    welcome->addStretch();
    auto* title = label(UiLanguage::tr("Your next idea starts here"), "welcomeTitle");
    title->setAlignment(Qt::AlignCenter);
    welcome->addWidget(title);
    auto* description = label(UiLanguage::tr("Open a file, create a project, or connect an SDK to begin."),
                              "welcomeDescription");
    description->setAlignment(Qt::AlignCenter);
    welcome->addWidget(description);
    auto* actions = new QHBoxLayout;
    actions->addStretch();
    auto* create = new QPushButton(UiLanguage::tr("New project"));
    create->setObjectName("welcomeNewProject");
    create->setProperty("class", "primary");
    auto* open = new QPushButton(UiLanguage::tr("Open project"));
    open->setObjectName("welcomeOpenProject");
    auto* sdk = new QPushButton(UiLanguage::tr("Connect SDK"));
    connect(create, &QPushButton::clicked, this, &MainWindow::createProject);
    connect(open, &QPushButton::clicked, this, &MainWindow::openProjectFolder);
    connect(sdk, &QPushButton::clicked, this, &MainWindow::showSdkDiscovery);
    actions->addWidget(create);
    actions->addWidget(open);
    actions->addWidget(sdk);
    actions->addStretch();
    welcome->addSpacing(24);
    welcome->addLayout(actions);
    auto* keys = label(UiLanguage::tr("Ctrl+O  Open files     ·     Ctrl+K  Find an action"), "caption");
    keys->setAlignment(Qt::AlignCenter);
    welcome->addSpacing(20);
    welcome->addWidget(keys);
    welcome->addStretch();
    layout->addWidget(emptyEditor_, 1);
    emptyEditor_->hide();

    threadHint_ = new QLabel;
    threadHint_->setObjectName("inlineThreadHint");
    threadHint_->setWordWrap(true);
    threadHint_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    threadHint_->setContentsMargins(14, 8, 14, 8);
    threadHint_->setMaximumHeight(120);
    threadHint_->hide();
    layout->addWidget(threadHint_);
    markdownTimer_ = new QTimer(this);
    markdownTimer_->setSingleShot(true);
    markdownTimer_->setInterval(180);
    connect(markdownTimer_, &QTimer::timeout, this, [this] { updateMarkdown(); });
    connect(editor_, &CodeEditor::sourceTextChanged, markdownTimer_, qOverload<>(&QTimer::start));
    connect(editor_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (!markdownPreview_->isVisible() || !QSettings().value("markdown/syncScroll", true).toBool())
            return;
        const auto maximum = editor_->verticalScrollBar()->maximum();
        markdownPreview_->verticalScrollBar()->setValue(
            maximum ? value * markdownPreview_->verticalScrollBar()->maximum() / maximum : 0);
    });
    connect(markdownPreview_, &QTextBrowser::anchorClicked, this, [this](const QUrl& link) {
        if (link.toString().startsWith('#')) {
            markdownPreview_->scrollToAnchor(link.fragment());
            return;
        }
        const auto resolved = markdownPreview_->document()->baseUrl().resolved(link);
        if (!resolved.isLocalFile())
            return;
        const auto path = QFileInfo(resolved.toLocalFile()).canonicalFilePath(),
                   root = QDir(workspaceRoot_).canonicalPath();
        if (!root.isEmpty() && path.startsWith(root + '/') && QFileInfo(path).isFile())
            loadCodeFile(path);
    });
    connect(editor_, &CodeEditor::openFileRequested, this, [this](const QString& path) {
        if (QFileInfo(path).isDir()) {
            workspaceRoot_ = QDir(path).canonicalPath();
            QSettings().setValue("workspace/root", workspaceRoot_);
            updateProjectTree();
        } else
            loadCodeFile(path);
    });
    connect(find, &QToolButton::clicked, this, &MainWindow::findNext);
    connect(codeSearch_, &QLineEdit::returnPressed, this, &MainWindow::findNext);
    const auto hideSearch = [this] {
        codeSearchBar_->hide();
        editor_->setFocus();
    };
    connect(closeSearch, &QToolButton::clicked, this, hideSearch);
    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), codeSearchBar_);
    escape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, this, hideSearch);
    activeLanguage_ = label("C++", "caption");
    activeLanguage_->setWordWrap(false);
    editorPosition_ = label("1:1 · UTF-8 · LF", "caption");
    editorPosition_->setWordWrap(false);
    statusBar()->addPermanentWidget(activeLanguage_);
    statusBar()->addPermanentWidget(editorPosition_);
    inspectionDock_ = new QDockWidget(UiLanguage::tr("Thread hints"), this);
    inspectionDock_->setObjectName("codeInspectionDock");
    inspectionDock_->setAllowedAreas(Qt::RightDockWidgetArea | Qt::LeftDockWidgetArea);
    auto* analysis = new QWidget;
    analysis->setObjectName("sourceInspection");
    auto* hintsLayout = new QVBoxLayout(analysis);
    hintsLayout->setContentsMargins(12, 10, 12, 10);
    hintsLayout->setSpacing(10);
    codeSummary_ = label(UiLanguage::tr("Local C/C++ concurrency hints"), "description");
    codeSummary_->setWordWrap(true);
    hintsLayout->addWidget(codeSummary_);
    findings_ = new QListWidget;
    findings_->setObjectName("codeFindings");
    findings_->setWordWrap(true);
    findings_->setTextElideMode(Qt::ElideNone);
    findings_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    findings_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    hintsLayout->addWidget(findings_, 1);
    auto* refresh = new QPushButton(UiLanguage::tr("Run inspection again"));
    hintsLayout->addWidget(refresh);
    connect(refresh, &QPushButton::clicked, this, &MainWindow::inspectSource);
    inspectionDock_->setWidget(analysis);
    addDockWidget(Qt::RightDockWidgetArea, inspectionDock_);
    resizeDocks({inspectionDock_}, {340}, Qt::Horizontal);
    inspectionDock_->hide();
    connect(findings_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        editor_->jumpToLine(item->data(Qt::UserRole).toInt());
        editor_->setFocus();
    });
    connect(editor_, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        updateThreadHint();
        const auto cursor = editor_->textCursor();
        editorPosition_->setText(QString("%1:%2  ·  UTF-8  ·  LF")
                                     .arg(cursor.blockNumber() + 1)
                                     .arg(cursor.positionInBlock() + 1));
    });
    return page;
}
