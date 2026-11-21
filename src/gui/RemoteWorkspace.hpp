#pragma once
#include "LspClient.hpp"
#include "RemoteSession.hpp"
#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QStringDecoder>

class CodeEditor;
class QTreeWidget;
class QTreeWidgetItem;
class QTabWidget;
class QLineEdit;
class QPlainTextEdit;
class QLabel;
class QPushButton;
class QCloseEvent;

class RemoteWorkspace final : public QMainWindow {
    struct Document {
        QPointer<CodeEditor> editor;
        QString fingerprint;
        int uploadingRevision = -1;
        bool loading = false;
        QString uploadedText;
        bool crlf = false, utf8Bom = false;
    };

    RemoteSession *files_, *commands_;
    LspClient* lsp_;
    QHash<QString, Document> documents_;
    QHash<QString, QString> languageCommands_, sdkRoots_;
    QTreeWidget* tree_;
    QTabWidget* tabs_;
    QLineEdit *serverCommand_, *sdkRoot_, *command_, *stdin_;
    QPlainTextEdit* output_;
    QLabel* status_;
    QPushButton *connect_, *save_, *discover_, *languageConnect_, *run_, *stop_;
    QStringList saveQueue_;
    bool connected_ = false, closeAfterSave_ = false;
    QString currentLanguage_;
    QString comparisonPath_;
    QPointer<CodeEditor> completionEditor_;
    int completionRevision_ = -1, completionPosition_ = -1;
    QStringDecoder outputDecoder_{QStringDecoder::Utf8};
    std::function<void(const QString&, const QByteArray&, const QString&)> displayFile_;
    CodeEditor* currentEditor() const;
    QString currentPath() const;
    void configure();
    void trustHost(const RemoteConnection& connection);
    void populate(const QString& path, const QJsonArray& entries);
    void open(const QString& relative);
    void activate();
    void startLanguageServer();
    void discoverTools();
    void saveCurrent();
    void saveAll();
    void pumpSave();
    void updateTabs();
    void navigateDefinition();
    void formatCurrent();
    void appendOutput(const QByteArray& bytes);
    void reload();
    void compareCurrent();
    void showComparison(const QString& path, const QByteArray& contents, const QString& fingerprint);
    void newFile();
    void updateControls();
    bool modified() const;

  public:
    explicit RemoteWorkspace(QWidget* parent = nullptr);
    ~RemoteWorkspace() override;

  protected:
    void closeEvent(QCloseEvent* event) override;
};
