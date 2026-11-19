#pragma once

#include "CodeEditor.hpp"
#include "CodeRunner.hpp"
#include "LspClient.hpp"
#include "ThemeManager.hpp"
#include "ts/Analyzer.hpp"
#include <QMainWindow>
#include <QPointer>
#include <QTemporaryDir>

class QStackedWidget;
class QComboBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTimer;
class QLineEdit;
class QCloseEvent;
class QTabBar;
class QDockWidget;
class QTreeView;
class QFileSystemModel;
class ProjectFileFilter;
class QPlainTextEdit;
class QAction;
class DocumentController;
class BackgroundJob;
class SourceInspector;
class MarkdownPreview;
class GitPanel;
class DebuggerPanel;
class QSplitter;
class AgentPanel;

class MainWindow final : public QMainWindow {
    struct NavigationLocation {
        QPointer<QTextDocument> document;
        QTextCursor cursor;
        QString path;
        int position, anchor, vertical, horizontal;
    };

    QList<NavigationLocation> navigationBack_, navigationForward_;
    bool restoringNavigation_ = false;
    NavigationLocation currentLocation() const;
    void rememberNavigation();
    void navigateHistory(bool forward);
    QWidget* emptyEditor_ = nullptr;
    QTextDocument* emptyDocument_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QComboBox* scaleSelector_ = nullptr;
    int uiPercent_ = 100;
    QComboBox* themeSelector_ = nullptr;
    QComboBox* runContext_ = nullptr;
    QAction* glassToggle_ = nullptr;
    QDockWidget* projectDock_ = nullptr;
    QDockWidget* outputDock_ = nullptr;
    QTreeView* projectTree_ = nullptr;
    QLineEdit* projectQuery_ = nullptr;
    QFileSystemModel* projectFiles_ = nullptr;
    ProjectFileFilter* projectFilter_ = nullptr;
    QLabel* projectName_ = nullptr;
    QLabel* activeView_ = nullptr;
    QWidget* codeSearchBar_ = nullptr;
    QSplitter* documentSplit_ = nullptr;
    MarkdownPreview* markdownPreview_ = nullptr;
    QComboBox* markdownMode_ = nullptr;
    QTimer* markdownTimer_ = nullptr;
    GitPanel* gitPanel_ = nullptr;
    DebuggerPanel* debugger_ = nullptr;
    QDockWidget* debugDock_ = nullptr;
    QDockWidget* gitDock_ = nullptr;
    QDockWidget* inspectionDock_ = nullptr;
    QDockWidget* agentDock_ = nullptr;
    AgentPanel* agentPanel_ = nullptr;
    QListWidget* workbenchProblems_ = nullptr;
    QPlainTextEdit* runConsole_ = nullptr;
    QTimer* codeAuditTimer_ = nullptr;
    SourceInspector* sourceInspector_ = nullptr;
    ts::CodeReport currentCodeReport_;
    CodeEditor* editor_ = nullptr;
    DocumentController* documents_ = nullptr;
    QTemporaryDir scratchDocuments_;
    QString lspSourcePath_;
    QTabBar* documentTabs_ = nullptr;
    QTimer* sessionSaveTimer_ = nullptr;
    QString sessionPath_;
    bool switchingDocument_ = false;
    BackgroundJob* localModelJob_ = nullptr;
    QTimer* neuralReviewTimer_ = nullptr;
    QTimer* predictiveTimer_ = nullptr;
    unsigned long long localGeneration_ = 0;
    BackgroundJob* analysisJob_ = nullptr;
    bool analysisPending_ = false;
    bool closing_ = false;
    CodeRunner* codeRunner_ = nullptr;
    LspClient* lsp_ = nullptr;
    QTimer* lspChangeTimer_ = nullptr;
    QLineEdit* runInput_ = nullptr;
    QLabel* activeLanguage_ = nullptr;
    QPushButton* sourceBuild_ = nullptr;
    QPushButton* sourceRun_ = nullptr;
    QPushButton* sourceStop_ = nullptr;
    bool trustedToRun_ = false;
    QString pendingBuildLine_;

    struct SourceIssue {
        int line = 0;
        QString message;
        QString file;
    };

    ts::Array<SourceIssue> compilerIssues_;
    ts::Array<SourceIssue> lspIssues_;
    QListWidget* findings_ = nullptr;
    QLabel* codeSummary_ = nullptr;
    QLabel* threadHint_ = nullptr;
    QLabel* editorPosition_ = nullptr;
    QLabel* sourcePathLabel_ = nullptr;
    QLineEdit* codeSearch_ = nullptr;
    QString sourcePath_;
    QString workspaceRoot_;
    QString sourceDisplayName_ = "untitled.cpp";

  public:
    explicit MainWindow(QWidget* parent = nullptr);

  protected:
    void closeEvent(QCloseEvent*) override;

  private:
    QWidget* buildCodePage();
    QWidget* buildSidebar();
    void goToPage(int page);
    void buildNativeMenus();
    QWidget* buildIdeHeader();
    void setupToolWindows();
    void updateProjectTree();
    void applyTheme(tsui::Theme theme);
    void showCommandPalette();
    void inspectSource();
    void revealActiveFile();
    void setGlassEnabled(bool enabled);
    void applyUiScale(int percent);
    void resetWorkspace();
    void appendLog(const QString& text);
    bool confirmDiscardCode();
    void updateWindowTitle();
    void analyzeCode();
    void startDeepInspection();
    void applyCodeAnalysis(const ts::CodeReport& report);
    void updateThreadHint();
    void openCode();
    bool loadCodeFile(const QString& path);
    void openProjectFolder();
    void createProject();
    void showEmptyEditor();
    void newCode();
    void createProjectEntry(bool directory, const QString& base = QString());
    void installDocumentWorkspace();
    void storeEditorState();
    void updateDocumentTabs();
    void closeDocumentTab(int index);
    void saveAllDocuments();
    void saveCodeAs();
    void configureProjectFiles();
    void findProjectFiles();
    void findProjectText();
    void showProjectSearch(bool filenames);
    void buildSource(bool execute);
    void buildActive(bool execute);
    void buildProject(bool execute);
    void stopSource();
    void configureLanguageServer();
    void syncLanguage(bool restartLsp);
    void consumeBuildOutput(const QString& chunk);
    void updateExternalProblems();
    void jumpToProblemItem(QListWidgetItem* item);
    bool writeCode();
    void saveCode();
    void findNext();
    void replaceSourceText();
    void goToSourceLine();
    void configureEditorFont();
    void showSettings();
    void showSdkDiscovery();
    void setupLocalAssistant();
    void setupProjectAgent();
    void applyAgentProposal();
    void requestPredictiveCompletion();
    void reviewThreadWithModel(bool interactive = false);
    void navigateSymbol(bool references);
    void renameSymbol();
    void formatDocument();
    void showSymbolInfo();
    void refreshSemanticTokens();
    void startDebugging();
    QJsonObject breakpoints() const;
    bool applyLanguageEdits(const QJsonObject& edit);
    bool prepareLanguageRequest();
    void applyPreferences();
    void requestCodeCompletion(int attempt = 0);
    void updateMarkdown(bool changedDocument = false);
    void configureToolchain();
};
