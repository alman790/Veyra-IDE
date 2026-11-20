#pragma once
#include "GitAnnotations.hpp"
#include "ts/Analyzer.hpp"
#include "ts/LanguageTools.hpp"
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QSyntaxHighlighter>
#include <QTimer>
#include <functional>

class CodeEditor;
class QCompleter;
class QKeyEvent;

class LineNumberArea final : public QWidget {
    CodeEditor* editor_;
    bool selecting_ = false;
    int anchorLine_ = -1;
    QPoint dragPosition_;
    QTimer scrollTimer_;

  public:
    explicit LineNumberArea(CodeEditor* editor);
    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    bool event(QEvent* event) override;
};

class CodeHighlighter final : public QSyntaxHighlighter {
    struct Rule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };

    ts::Array<Rule> rules_;
    QTextCharFormat multiline_;

    struct SemanticSpan {
        int start;
        int length;
        QString role;
    };

    QHash<int, QList<SemanticSpan>> semantic_;
    int semanticRevision_ = -1;
    ts::LanguageId language_ = ts::LanguageId::Cpp;
    bool blockComments_ = true;

  public:
    explicit CodeHighlighter(QTextDocument* document, ts::LanguageId language = ts::LanguageId::Cpp);
    void refreshTheme();
    void setSemanticTokens(const QJsonArray& data, const QStringList& legend);

    void setLanguage(ts::LanguageId lang) {
        language_ = lang;
        semantic_.clear();
        refreshTheme();
    }

  protected:
    void highlightBlock(const QString& text) override;
};

class CodeEditor final : public QPlainTextEdit {
    Q_OBJECT

    LineNumberArea* lineNumbers_;
    CodeHighlighter* highlighter_ = nullptr;
    ts::Array<ts::Diagnostic> diagnostics_;
    QList<QTextEdit::ExtraSelection> diagnosticSelections_;
    QHash<int, int> diagnosticLines_;
    ts::LanguageId language_ = ts::LanguageId::Cpp;
    QCompleter* completer_ = nullptr;
    GitAnnotations* annotations_;
    int executionLine_ = -1;
    QString ghostText_;
    QStringList documentWords_;
    int wordsRevision_ = -1;
    int wordsBlock_ = -1;
    int completionPosition_ = -1;
    int completionRevision_ = -1;
    bool completionAllowed_ = false;
    bool editingKey_ = false;
    void dismissCompletion();
    void updateDocumentWords();
    QHash<QString, QJsonObject> serverEdits_;
    QStringList serverWords_;
    QTimer completionTimer_;
    bool autoCompletion_ = true, autoPairs_ = true, indentGuides_ = true;
    int tabWidth_ = 4;
    quint64 viewGeneration_ = 0;
    QString completionPrefix() const;
    void requestSuggestions();
    std::string bracketMask_;
    int bracketRevision_ = -1;
    int bracketOffset_ = 0;
    void indentSelection(bool outdent);
    std::function<void()> completionRequest_;
    void insertSuggestion(const QString& completion);
    int annotationWidth() const;
    void syncMarkers();
    void wireDocument();
    QMetaObject::Connection markerConnection_;

    friend class LineNumberArea;

  public:
    explicit CodeEditor(QWidget* parent = nullptr);
    int lineNumberAreaWidth() const;
    void lineNumberAreaPaintEvent(QPaintEvent* event);
    void setDiagnostics(const ts::Array<ts::Diagnostic>& diagnostics);
    void jumpToLine(int line);

    quint64 viewGeneration() const {
        return viewGeneration_;
    }

    void invalidateViewRestore() {
        ++viewGeneration_;
    }

    void refreshTheme();
    void showCompletion();
    void setGhostText(const QString& text);
    void setSemanticTokens(const QJsonArray& data, const QStringList& legend);
    void toggleBreakpointAt(int y);
    void toggleBookmark(int line);
    void navigateBookmark(bool forward);
    QList<int> bookmarkedLines() const;
    int lineAtY(int y) const;
    void selectLines(int anchorLine, int line);
    void setFilePath(const QString& path);
    void setAnnotationsVisible(bool visible);

    GitAnnotations* annotations() const {
        return annotations_;
    }

    void setExecutionLine(int line);
    void applyPreferences();
    void setLanguage(ts::LanguageId language);
    void useDocument(QTextDocument* document);
    void setCompletionWords(const QStringList& suggestions, const QHash<QString, QJsonObject>& edits = {});

    void setCompletionRequest(std::function<void()> callback) {
        completionRequest_ = std::move(callback);
    }

  signals:
    void sourceTextChanged();
    void breakpointChanged();
    void assistantDismissed();
    void predictiveCompletionRequested();
    void openFileRequested(const QString& path);
    void navigationRequested();
    void annotationsVisibilityChanged(bool visible);
    void commitInspectionRequested(const QString& commit);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void changeEvent(QEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    void insertFromMimeData(const QMimeData* source) override;

  private:
    void updateLineNumberAreaWidth(int);
    void updateLineNumberArea(const QRect&, int);
    void highlightCurrentLine();
};
