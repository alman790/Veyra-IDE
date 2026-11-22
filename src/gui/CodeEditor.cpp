#include "CodeEditor.hpp"
#include "../application/SourceRevision.hpp"
#include "ThemeManager.hpp"
#include "UiLanguage.hpp"
#include "ts/SourceMask.hpp"
#include <QAbstractItemView>
#include <QCompleter>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QHelpEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QStringListModel>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextEdit>
#include <QToolTip>
#include <QUrl>

namespace {
class DocumentMarkers final : public QObject {
  public:
    QList<QTextCursor> bookmarks, breakpoints;

    explicit DocumentMarkers(QTextDocument* document) : QObject(document) {
        setObjectName("editorMarkers");
        for (const auto& value : document->property("breakpoints").toList()) {
            const auto block = document->findBlockByNumber(value.toInt() - 1);
            if (block.isValid())
                breakpoints.append(QTextCursor(block));
        }
        for (const auto& value : document->property("bookmarks").toList()) {
            const auto block = document->findBlockByNumber(value.toInt() - 1);
            if (block.isValid())
                bookmarks.append(QTextCursor(block));
        }
    }
};

DocumentMarkers* markers(QTextDocument* document) {
    for (auto* child : document->children())
        if (child->objectName() == "editorMarkers")
            return static_cast<DocumentMarkers*>(child);
    return new DocumentMarkers(document);
}

void toggleMarker(QList<QTextCursor>& list, const QTextBlock& block) {
    const auto end = std::remove_if(list.begin(), list.end(),
                                    [&block](const auto& cursor) { return cursor.block() == block; });
    const bool found = end != list.end();
    list.erase(end, list.end());
    if (!found)
        list.append(QTextCursor(block));
}

bool identifierPart(QChar value) {
    return value.isLetterOrNumber() || value == '_' || value.isMark();
}
}

LineNumberArea::LineNumberArea(CodeEditor* editor) : QWidget(editor), editor_(editor) {
    setObjectName("editorGutter");
    setMouseTracking(true);
    scrollTimer_.setInterval(30);
    connect(&scrollTimer_, &QTimer::timeout, this, [this] {
        auto* scroll = editor_->verticalScrollBar();
        const int distance = dragPosition_.y() < 0 ? dragPosition_.y() : dragPosition_.y() - height();
        scroll->setValue(scroll->value() +
                         (distance < 0 ? -1 : 1) * std::clamp(std::abs(distance) / 12, 1, 20));
        const int line = editor_->lineAtY(std::clamp(dragPosition_.y(), 0, height() - 1));
        editor_->selectLines(anchorLine_, line > 0 ? line : editor_->blockCount());
    });
}

QSize LineNumberArea::sizeHint() const {
    return QSize(editor_->lineNumberAreaWidth(), 0);
}

void LineNumberArea::paintEvent(QPaintEvent* e) {
    editor_->lineNumberAreaPaintEvent(e);
}

void LineNumberArea::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton)
        return;
    const int line = editor_->lineAtY(event->position().y());
    if (line < 1)
        return;
    const int x = event->position().x() - editor_->annotationWidth();
    if (x < 0) {
        const auto& lines = editor_->annotations_->lines();
        if (line <= lines.size()) {
            const auto hash = lines[line - 1].commit;
            if (!hash.isEmpty() && hash != QString(hash.size(), '0'))
                emit editor_->commitInspectionRequested(hash);
        }
        return;
    }
    if (x < 18)
        editor_->toggleBookmark(line);
    else if (x < 36)
        editor_->toggleBreakpointAt(event->position().y());
    else {
        selecting_ = true;
        anchorLine_ = event->modifiers().testFlag(Qt::ShiftModifier)
                          ? editor_->document()->findBlock(editor_->textCursor().anchor()).blockNumber() + 1
                          : line;
        editor_->setFocus();
        editor_->selectLines(anchorLine_, line);
    }
    event->accept();
}

void LineNumberArea::mouseMoveEvent(QMouseEvent* event) {
    if (!selecting_)
        return;
    dragPosition_ = event->position().toPoint();
    const bool outside = dragPosition_.y() < 0 || dragPosition_.y() >= height();
    if (outside)
        scrollTimer_.start();
    else {
        scrollTimer_.stop();
        const int line = editor_->lineAtY(dragPosition_.y());
        editor_->selectLines(anchorLine_, line > 0 ? line : editor_->blockCount());
    }
}

void LineNumberArea::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        selecting_ = false;
        scrollTimer_.stop();
    }
}

bool LineNumberArea::event(QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(event);
        const int line = editor_->lineAtY(help->pos().y());
        QString text;
        const int x = help->pos().x() - editor_->annotationWidth();
        if (x < 0) {
            const auto& lines = editor_->annotations_->lines();
            if (line > 0 && line <= lines.size()) {
                const auto& info = lines[line - 1];
                text = info.commit == QString(info.commit.size(), '0')
                           ? UiLanguage::tr("Uncommitted changes")
                           : info.author + "\n" +
                                 QDateTime::fromSecsSinceEpoch(info.timestamp).toString(Qt::ISODate) + "\n" +
                                 info.commit + "\n" + info.summary;
            } else
                text = editor_->annotations_->error().isEmpty() ? UiLanguage::tr("Loading Git annotations…")
                                                                : editor_->annotations_->error();
        } else if (x < 18)
            text = UiLanguage::tr("Toggle bookmark");
        else if (x < 36)
            text = UiLanguage::tr("Toggle breakpoint");
        else
            text = UiLanguage::tr("Drag to select lines");
        QToolTip::showText(help->globalPos(), text.toHtmlEscaped().replace('\n', "<br>"), this);
        return true;
    }
    return QWidget::event(event);
}

void LineNumberArea::contextMenuEvent(QContextMenuEvent* event) {
    const int line = editor_->lineAtY(event->pos().y());
    QMenu menu;
    auto* bookmark = menu.addAction(UiLanguage::tr("Toggle bookmark"));
    bookmark->setEnabled(line > 0);
    auto* breakpoint = menu.addAction(UiLanguage::tr("Toggle breakpoint"));
    breakpoint->setEnabled(line > 0);
    auto* authors = menu.addAction(UiLanguage::tr("Show Git annotations"));
    authors->setCheckable(true);
    authors->setEnabled(editor_->gitAnnotationsAvailable_);
    authors->setChecked(editor_->annotations_->enabled());
    const auto* chosen = menu.exec(event->globalPos());
    if (chosen == bookmark)
        editor_->toggleBookmark(line);
    else if (chosen == breakpoint)
        editor_->toggleBreakpointAt(event->pos().y());
    else if (chosen == authors)
        editor_->setAnnotationsVisible(authors->isChecked());
}

CodeHighlighter::CodeHighlighter(QTextDocument* doc, ts::LanguageId language)
    : QSyntaxHighlighter(doc), language_(language) {
    refreshTheme();
}

void CodeHighlighter::refreshTheme() {
    rules_.clear();
    const bool light = tsui::ThemeManager::isLight();
    auto add = [&](const QString& pattern, const QString& color, bool bold = false) {
        QTextCharFormat format;
        format.setFont(document()->defaultFont());
        format.setForeground(QColor(color));
        if (bold)
            format.setFontWeight(QFont::DemiBold);
        rules_.pushBack(Rule{QRegularExpression(pattern), format});
    };
    const auto profile = ts::profileFor(language_);
    blockComments_ = profile.blockComments;
    QStringList keywordsList;
    for (const auto& word : profile.keywords)
        keywordsList << QString::fromStdString(word);
    const auto keywords = keywordsList.join(' ');
    const QStringList words = keywords.split(' ', Qt::SkipEmptyParts);
    QStringList escaped;
    for (const auto& word : words)
        escaped << QRegularExpression::escape(word);
    add("\\b(?:" + escaped.join('|') + ")\\b", light ? "#6C4DA5" : "#C8A4ED", true);
    add(R"(\b\d+(?:\.\d+)?(?:[uUlLfF]+)?\b)", light ? "#A45A17" : "#E9B783");
    add(R"(\b[A-Za-z_]\w*(?=\s*\())", light ? "#2D66A2" : "#84B3EA");
    for (const auto& [pattern, role] : profile.highlightPatterns)
        add(QString::fromStdString(pattern),
            role == "keyword" ? (light ? "#9A6B20" : "#E6B76E") : (light ? "#2D66A2" : "#84B3EA"), true);
    add(R"("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')", light ? "#387B56" : "#93CDA0");
    if (!profile.lineComment.empty())
        add(QRegularExpression::escape(QString::fromStdString(profile.lineComment)) + "[^\\n]*",
            light ? "#79818C" : "#8996A9");
    multiline_.setFont(document()->defaultFont());
    multiline_.setForeground(QColor(light ? "#79818C" : "#8996A9"));
    rehighlight();
}

void CodeHighlighter::highlightBlock(const QString& text) {
    QTextCharFormat base;
    base.setFont(document()->defaultFont());
    setFormat(0, text.size(), base);
    const auto highlighted = text.left(32768);
    for (const auto& rule : rules_) {
        auto it = rule.pattern.globalMatch(highlighted);
        while (it.hasNext()) {
            auto match = it.next();
            setFormat(match.capturedStart(), match.capturedLength(), rule.format);
        }
    }
    setCurrentBlockState(0);
    if (blockComments_) {
        int from = previousBlockState() == 1 ? 0 : text.indexOf("/*");
        while (from >= 0) {
            int to = text.indexOf("*/", from);
            int length = to < 0 ? text.size() - from : to - from + 2;
            setFormat(from, length, multiline_);
            if (to < 0) {
                setCurrentBlockState(1);
                break;
            }
            from = text.indexOf("/*", from + length);
        }
    }
    if (semanticRevision_ == sourceRevision(document())) {
        for (const auto& token : semantic_.value(currentBlock().blockNumber())) {
            if (token.start < 0 || token.length < 0 || token.start + token.length > text.size())
                continue;
            const bool light = tsui::ThemeManager::isLight();
            QColor color;
            if (token.role == "type" || token.role == "class" || token.role == "struct" ||
                token.role == "interface" || token.role == "enum" || token.role == "typeParameter")
                color = QColor(light ? "#267A83" : "#8EC6C7");
            else if (token.role == "function" || token.role == "method")
                color = QColor(light ? "#2D66A2" : "#84B3EA");
            else if (token.role == "keyword" || token.role == "modifier")
                color = QColor(light ? "#6C4DA5" : "#C8A4ED");
            else if (token.role == "string")
                color = QColor(light ? "#387B56" : "#93CDA0");
            else if (token.role == "comment")
                color = QColor(light ? "#79818C" : "#8996A9");
            else if (token.role == "number" || token.role == "enumMember")
                color = QColor(light ? "#A45A17" : "#E9B783");
            else if (token.role == "macro")
                color = QColor(light ? "#9A6B20" : "#E6B76E");
            if (color.isValid()) {
                QTextCharFormat format;
                format.setForeground(color);
                setFormat(token.start, token.length, format);
            }
        }
    }
}

CodeEditor::CodeEditor(QWidget* parent) : QPlainTextEdit(parent) {
    sourceRevision(document());
    lineNumbers_ = new LineNumberArea(this);
    annotations_ = new GitAnnotations(this);
    annotations_->setEnabled(QSettings().value("editor/gitAnnotations", false).toBool());
    connect(annotations_, &GitAnnotations::updated, lineNumbers_, qOverload<>(&QWidget::update));
    highlighter_ = new CodeHighlighter(document());
    completer_ = new QCompleter(this);
    completer_->setCaseSensitivity(Qt::CaseInsensitive);
    completer_->setCompletionMode(QCompleter::PopupCompletion);
    completer_->setWidget(this);
    completer_->setMaxVisibleItems(8);
    completer_->popup()->setObjectName("codeCompletionPopup");
    completer_->popup()->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(completer_, QOverload<const QString&>::of(&QCompleter::activated), this,
            [this](const QString& word) { insertSuggestion(word); });
    completionTimer_.setSingleShot(true);
    connect(&completionTimer_, &QTimer::timeout, this, &CodeEditor::requestSuggestions);
    connect(this, &CodeEditor::sourceTextChanged, this, [this] {
        serverWords_.clear();
        serverEdits_.clear();
        completionAllowed_ = false;
        wordsRevision_ = -1;
        if (autoCompletion_ && editingKey_ && !isReadOnly())
            completionTimer_.start(completer_->popup()->isVisible() ? 0 : completionDelay_);
        else
            dismissCompletion();
    });
    applyPreferences();
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setStyleHint(QFont::Monospace);
    setFont(font);
    setTabStopDistance(QFontMetrics(font).horizontalAdvance(' ') * 4);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setCenterOnScroll(false);
    setMouseTracking(true);
    setObjectName("codeEditor");
    connect(this, &CodeEditor::blockCountChanged, this, &CodeEditor::updateLineNumberAreaWidth);
    connect(this, &CodeEditor::updateRequest, this, &CodeEditor::updateLineNumberArea);
    connect(this, &CodeEditor::cursorPositionChanged, this, &CodeEditor::highlightCurrentLine);
    connect(this, &CodeEditor::cursorPositionChanged, this, [this] {
        if (!editingKey_)
            dismissCompletion();
        setGhostText({});
    });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (!editingKey_)
            QTimer::singleShot(0, this, &CodeEditor::repositionCompletion);
    });
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (!editingKey_)
            QTimer::singleShot(0, this, &CodeEditor::repositionCompletion);
    });
    updateLineNumberAreaWidth(0);
    wireDocument();
    highlightCurrentLine();
    setLanguage(ts::LanguageId::Cpp);
}

int CodeEditor::lineNumberAreaWidth() const {
    int digits = 1;
    int max = std::max(1, blockCount());
    while (max >= 10) {
        max /= 10;
        ++digits;
    }
    return annotationWidth() + 54 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void CodeEditor::updateLineNumberAreaWidth(int) {
    setViewportMargins(lineNumberAreaWidth() + 10, 0, 0, 0);
    const auto rectangle = contentsRect();
    lineNumbers_->setGeometry(rectangle.left(), rectangle.top(), lineNumberAreaWidth(), rectangle.height());
}

void CodeEditor::updateLineNumberArea(const QRect& r, int dy) {
    if (dy)
        lineNumbers_->scroll(0, dy);
    else
        lineNumbers_->update(0, r.y(), lineNumbers_->width(), r.height());
    if (r.contains(viewport()->rect()))
        updateLineNumberAreaWidth(0);
}

void CodeEditor::resizeEvent(QResizeEvent* e) {
    QPlainTextEdit::resizeEvent(e);
    QRect cr = contentsRect();
    lineNumbers_->setGeometry(QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
    QTimer::singleShot(0, this, &CodeEditor::repositionCompletion);
}

void CodeEditor::lineNumberAreaPaintEvent(QPaintEvent* e) {
    QPainter painter(lineNumbers_);
    painter.fillRect(e->rect(), tsui::ThemeManager::color(tsui::Role::Editor));
    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    const int annotation = annotationWidth();
    QSet<int> breakpointLines;
    for (const auto& cursor : markers(document())->breakpoints)
        breakpointLines.insert(cursor.blockNumber() + 1);
    const auto bookmarks = bookmarkedLines();
    while (block.isValid()) {
        const auto geometry = blockBoundingGeometry(block).translated(contentOffset());
        const qreal top = geometry.top(), bottom = geometry.bottom();
        if (top > e->rect().bottom())
            break;
        if (block.isVisible() && bottom >= e->rect().top()) {
            painter.setPen(tsui::ThemeManager::color(tsui::Role::Muted));
            painter.drawText(annotation + 44, qRound(top), lineNumbers_->width() - annotation - 53,
                             fontMetrics().height(), Qt::AlignRight, QString::number(blockNumber + 1));
            const qreal middle = top + fontMetrics().height() / 2.0;
            if (bookmarks.contains(blockNumber + 1)) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(tsui::ThemeManager::color(tsui::Role::Accent));
                painter.drawRoundedRect(QRectF(annotation + 5, middle - 5, 8, 11), 2, 2);
            }
            if (breakpointLines.contains(blockNumber + 1)) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(tsui::ThemeManager::color(tsui::Role::Error));
                painter.drawEllipse(QPointF(annotation + 27, middle), 4.5, 4.5);
            }
            if (diagnosticLines_.contains(blockNumber + 1)) {
                const auto& d = diagnostics_[diagnosticLines_.value(blockNumber + 1)];
                painter.setBrush(d.severity == ts::RiskLevel::Critical
                                     ? tsui::ThemeManager::color(tsui::Role::Error)
                                     : (d.severity == ts::RiskLevel::Warning
                                            ? tsui::ThemeManager::color(tsui::Role::Warning)
                                            : tsui::ThemeManager::color(tsui::Role::Good)));
                painter.setPen(Qt::NoPen);
                painter.drawEllipse(QPointF(annotation + 39, middle), 2, 2);
            }
            if (annotation > 0) {
                const auto& lines = annotations_->lines();
                QString text;
                if (blockNumber < lines.size()) {
                    const auto& info = lines[blockNumber];
                    if (!info.commit.isEmpty())
                        text = info.commit == QString(info.commit.size(), '0')
                                   ? UiLanguage::tr("Uncommitted changes")
                                   : info.commit.left(8) + "  " + info.author;
                } else if (blockNumber == firstVisibleBlock().blockNumber())
                    text = annotations_->error().isEmpty()
                               ? UiLanguage::tr("Loading Git annotations…")
                               : UiLanguage::tr("Git annotations are unavailable.");
                painter.setPen(tsui::ThemeManager::color(tsui::Role::Muted));
                painter.drawText(6, qRound(top), annotation - 14, fontMetrics().height(), Qt::AlignLeft,
                                 fontMetrics().elidedText(text, Qt::ElideRight, annotation - 14));
            }
        }
        block = block.next();
        ++blockNumber;
    }
}

void CodeEditor::highlightCurrentLine() {
    QList<QTextEdit::ExtraSelection> selections;
    QTextEdit::ExtraSelection current;
    current.format.setBackground(tsui::ThemeManager::color(tsui::Role::CurrentLine));
    current.format.setProperty(QTextFormat::FullWidthSelection, true);
    current.cursor = textCursor();
    current.cursor.clearSelection();
    selections.append(current);
    selections.append(diagnosticSelections_);
    {
        const int caret = textCursor().position();
        if (bracketRevision_ != sourceRevision(document()) || caret < bracketOffset_ ||
            caret >= bracketOffset_ + static_cast<int>(bracketMask_.size())) {
            auto window = QTextCursor(document());
            const auto first = document()->findBlock(std::max(0, caret - 32768));
            bracketOffset_ = std::max(first.position(), caret - 32768);
            window.setPosition(bracketOffset_);
            window.setPosition(std::min(document()->characterCount() - 1, caret + 32768),
                               QTextCursor::KeepAnchor);
            auto source = window.selectedText().replace(QChar::ParagraphSeparator, '\n');
            const bool comment = bracketOffset_ == first.position() ? first.previous().userState() == 1
                                                                    : first.userState() == 1;
            if (bracketOffset_ != first.position()) {
                const auto newline = source.indexOf('\n');
                std::fill(source.begin(), source.begin() + (newline < 0 ? source.size() : newline),
                          QChar(' '));
            }
            std::string ascii = comment ? "/*" : "";
            ascii.reserve(source.size() + 2);
            for (const auto ch : source)
                ascii.push_back(ch.unicode() < 128 ? static_cast<char>(ch.unicode()) : 'x');
            bracketMask_ = ts::maskCommentsAndStrings(ascii);
            if (comment)
                bracketMask_.erase(0, 2);
            bracketRevision_ = sourceRevision(document());
        }
        int position = caret - bracketOffset_ - 1;
        const auto bracket = [](char value) {
            return std::string("()[]{}").find(value) != std::string::npos;
        };
        if (position < 0 || position >= static_cast<int>(bracketMask_.size()) ||
            !bracket(bracketMask_[position]))
            position = caret - bracketOffset_;
        if (position >= 0 && position < static_cast<int>(bracketMask_.size()) &&
            bracket(bracketMask_[position])) {
            const char value = bracketMask_[position];
            const auto at = std::string("()[]{}").find(value);
            const bool forward = at % 2 == 0;
            const char opposite = std::string("()[]{}")[forward ? at + 1 : at - 1];
            int depth = 1, match = -1;
            for (int i = position + (forward ? 1 : -1); i >= 0 && i < static_cast<int>(bracketMask_.size());
                 i += forward ? 1 : -1) {
                if (bracketMask_[i] == value)
                    ++depth;
                else if (bracketMask_[i] == opposite && --depth == 0) {
                    match = i;
                    break;
                }
            }
            for (int index : {position, match})
                if (index >= 0) {
                    QTextEdit::ExtraSelection pair;
                    pair.cursor = QTextCursor(document());
                    pair.cursor.setPosition(index + bracketOffset_);
                    pair.cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor);
                    pair.format.setBackground(
                        tsui::ThemeManager::color(match < 0 ? tsui::Role::Warning : tsui::Role::AccentSoft));
                    pair.format.setFontWeight(QFont::Bold);
                    selections.append(pair);
                }
        }
    }
    if (executionLine_ > 0) {
        const auto block = document()->findBlockByNumber(executionLine_ - 1);
        if (block.isValid()) {
            QTextEdit::ExtraSelection execution;
            execution.cursor = QTextCursor(block);
            execution.format.setProperty(QTextFormat::FullWidthSelection, true);
            execution.format.setBackground(QColor(64, 115, 77, 90));
            selections.append(execution);
        }
    }
    setExtraSelections(selections);
    lineNumbers_->update();
}

void CodeEditor::setDiagnostics(const ts::Array<ts::Diagnostic>& diagnostics) {
    diagnostics_ = diagnostics;
    diagnosticSelections_.clear();
    diagnosticLines_.clear();
    for (std::size_t index = 0; index < diagnostics_.size(); ++index) {
        const auto& d = diagnostics_[index];
        diagnosticLines_.insert(d.line, static_cast<int>(index));
        if (d.severity == ts::RiskLevel::Info)
            continue;
        QTextBlock block = document()->findBlockByNumber(d.line - 1);
        if (!block.isValid())
            continue;
        QTextEdit::ExtraSelection issue;
        issue.format.setBackground(d.severity == ts::RiskLevel::Critical ? QColor(149, 53, 73, 55)
                                                                         : QColor(149, 104, 40, 40));
        issue.format.setProperty(QTextFormat::FullWidthSelection, true);
        issue.cursor = QTextCursor(block);
        issue.cursor.clearSelection();
        diagnosticSelections_.append(issue);
    }
    highlightCurrentLine();
}

void CodeEditor::refreshTheme() {
    document()->setDefaultFont(font());
    viewport()->setFont(font());
    if (highlighter_)
        highlighter_->refreshTheme();
    highlightCurrentLine();
    if (lineNumbers_)
        lineNumbers_->update();
}

void CodeEditor::jumpToLine(int line) {
    if (line < 1)
        return;
    QTextBlock b = document()->findBlockByNumber(line - 1);
    if (!b.isValid())
        return;
    invalidateViewRestore();
    emit navigationRequested();
    QTextCursor c(b);
    setTextCursor(c);
    centerCursor();
    setFocus();
}

void CodeEditor::mouseMoveEvent(QMouseEvent* e) {
    QPlainTextEdit::mouseMoveEvent(e);
    int line = cursorForPosition(e->pos()).blockNumber() + 1;
    for (const auto& d : diagnostics_)
        if (d.line == line && d.severity != ts::RiskLevel::Info) {
            QToolTip::showText(e->globalPosition().toPoint(),
                               QString::fromStdString(d.title + "\n" + d.explanation), this);
            return;
        }
    QToolTip::hideText();
}

void CodeEditor::useDocument(QTextDocument* document) {
    if (document == this->document())
        return;
    completionTimer_.stop();
    completer_->popup()->hide();
    serverWords_.clear();
    serverEdits_.clear();
    completionAllowed_ = false;
    wordsRevision_ = -1;
    delete highlighter_;
    highlighter_ = nullptr;
    bracketRevision_ = -1;
    diagnosticSelections_.clear();
    diagnosticLines_.clear();
    setExtraSelections({});
    document->setDefaultFont(font());
    sourceRevision(document);
    QPlainTextEdit::setDocument(document);
    highlighter_ = new CodeHighlighter(document, language_);
    diagnostics_.clear();
    diagnosticSelections_.clear();
    diagnosticLines_.clear();
    wireDocument();
    annotations_->setDocument(document, {});
    setGhostText({});
    executionLine_ = -1;
    document->setProperty("analysisCurrent", false);
    document->setDefaultFont(font());
    updateLineNumberAreaWidth(0);
    highlightCurrentLine();
}

void CodeEditor::setLanguage(ts::LanguageId language) {
    if (language_ == language)
        return;
    dismissCompletion();
    language_ = language;
    highlighter_->setLanguage(language);
    documentWords_.clear();
    wordsRevision_ = -1;
    completionAllowed_ = false;
    serverWords_.clear();
    serverEdits_.clear();
    completer_->popup()->hide();
}

void CodeEditor::applyPreferences() {
    QSettings settings;
    autoCompletion_ = settings.value("editor/autoCompletion", true).toBool();
    autoPairs_ = settings.value("editor/autoPairs", true).toBool();
    indentGuides_ = settings.value("editor/indentGuides", true).toBool();
    tabWidth_ = std::clamp(settings.value("editor/tabWidth", 4).toInt(), 2, 8);
    completionDelay_ = std::clamp(settings.value("editor/completionDelay", 180).toInt(), 0, 1500);
    completionTimer_.setInterval(completionDelay_);
    setLineWrapMode(settings.value("editor/wordWrap", false).toBool() ? QPlainTextEdit::WidgetWidth
                                                                      : QPlainTextEdit::NoWrap);
    setTabStopDistance(QFontMetricsF(font()).horizontalAdvance(' ') * tabWidth_);
    if (!autoCompletion_)
        dismissCompletion();
    viewport()->update();
}

QString CodeEditor::completionPrefix() const {
    const auto cursor = textCursor();
    const auto text = cursor.block().text().left(cursor.positionInBlock());
    int from = text.size();
    while (from > 0 && identifierPart(text[from - 1]))
        --from;
    return text.mid(from);
}

void CodeEditor::setCompletionWords(const QStringList& suggestions,
                                    const QHash<QString, QJsonObject>& edits) {
    if (!completionAllowed_ || completionPosition_ != textCursor().position() ||
        completionRevision_ != sourceRevision(document()))
        return;
    serverWords_ = suggestions;
    serverEdits_ = edits;
    setProperty("semanticCandidates", suggestions);
    showCompletion();
}

void CodeEditor::requestSuggestions() {
    setProperty("completionState", QString("focus=%1 popup=%2 selection=%3")
                                       .arg(hasFocus())
                                       .arg(completer_->popup()->isVisible())
                                       .arg(textCursor().hasSelection()));
    if ((!hasFocus() && !completer_->popup()->isVisible()) || textCursor().hasSelection())
        return;
    if (!viewport()->rect().intersects(cursorRect())) {
        dismissCompletion();
        return;
    }
    const auto prefix = completionPrefix();
    const auto before = textCursor().block().text().left(textCursor().positionInBlock());
    const bool member = before.endsWith('.') || before.endsWith("->") || before.endsWith("::");
    emit predictiveCompletionRequested();
    if (prefix.size() < 2 && !member) {
        completer_->popup()->hide();
        return;
    }
    setProperty("completionState", "requested");
    completionAllowed_ = true;
    completionPosition_ = textCursor().position();
    completionRevision_ = sourceRevision(document());
    updateDocumentWords();
    if (completionRequest_)
        completionRequest_();
    showCompletion();
}

void CodeEditor::insertSuggestion(const QString& suggestion) {
    if (isReadOnly() || textCursor().hasSelection() || !completionAllowed_ ||
        completionPosition_ != textCursor().position() || completionRevision_ != sourceRevision(document())) {
        dismissCompletion();
        return;
    }
    QTextCursor cursor = textCursor();
    if (serverEdits_.contains(suggestion)) {
        const auto details = serverEdits_.value(suggestion);
        auto range = details.value("range").toObject();
        if (range.isEmpty())
            range = details.value("replace").toObject();

        struct Edit {
            int start;
            int end;
            QString text;
        };

        QVector<Edit> edits;
        const auto position = [this](const QJsonObject& point) {
            const auto block = document()->findBlockByNumber(point.value("line").toInt(-1));
            const int column = point.value("character").toInt(-1);
            return !block.isValid() || column < 0 || column > block.text().size() ? -1
                                                                                  : block.position() + column;
        };
        const int from = position(range.value("start").toObject()),
                  to = position(range.value("end").toObject());
        if (from < 0 || to < from || cursor.position() < from || cursor.position() > to)
            return;
        edits.append({from, to, suggestion});
        for (const auto& value : details.value("additionalTextEdits").toArray()) {
            const auto edit = value.toObject();
            const auto range = edit.value("range").toObject();
            const int start = position(range.value("start").toObject()),
                      end = position(range.value("end").toObject());
            if (start < 0 || end < start)
                return;
            edits.append({start, end, edit.value("newText").toString()});
        }
        std::sort(edits.begin(), edits.end(), [](const auto& a, const auto& b) {
            return a.start == b.start ? a.end > b.end : a.start > b.start;
        });
        int previous = document()->characterCount();
        for (const auto& edit : edits) {
            if (edit.end > previous)
                return;
            previous = edit.start;
        }
        QTextCursor anchor(document());
        anchor.setPosition(to);
        cursor.beginEditBlock();
        for (const auto& edit : edits) {
            cursor.setPosition(edit.start);
            cursor.setPosition(edit.end, QTextCursor::KeepAnchor);
            cursor.insertText(edit.text);
        }
        cursor.endEditBlock();
        setTextCursor(anchor);
        dismissCompletion();
        return;
    }
    const auto prefix = completionPrefix();
    const int wordStart = cursor.position() - prefix.size();
    int wordEnd = cursor.position();
    while (identifierPart(document()->characterAt(wordEnd)))
        ++wordEnd;
    cursor.beginEditBlock();
    cursor.setPosition(wordStart);
    cursor.setPosition(wordEnd, QTextCursor::KeepAnchor);
    QString insertion = suggestion;
    const auto before = cursor.block().text().left(cursor.selectionStart() - cursor.block().position());

    const auto scope = insertion.lastIndexOf("::");
    if (scope >= 0 && before.endsWith(insertion.left(scope + 2)))
        insertion = insertion.mid(scope + 2);
    cursor.insertText(insertion);
    cursor.endEditBlock();
    setTextCursor(cursor);
    completionTimer_.stop();
    completer_->popup()->hide();
}

void CodeEditor::showCompletion() {
    if (!completer_ || !completionAllowed_ || (!hasFocus() && !completer_->popup()->isVisible()))
        return;
    QStringList words = serverWords_;
    const auto prefix = completionPrefix();
    const auto before = textCursor().block().text().left(textCursor().positionInBlock() - prefix.size());
    const bool member = before.endsWith('.') || before.endsWith("->");
    if (!member && !before.endsWith("::")) {
        updateDocumentWords();
        for (const auto& word : documentWords_)
            if (word != prefix && word.startsWith(prefix, Qt::CaseInsensitive))
                words << word;
    }
    words.removeDuplicates();
    const auto selected = completer_->popup()->currentIndex().data().toString();
    auto* model = qobject_cast<QStringListModel*>(completer_->model());
    if (model) {
        if (model->stringList() != words)
            model->setStringList(words);
    } else
        completer_->setModel(new QStringListModel(words, completer_));
    completer_->setCompletionPrefix(prefix);
    if (completer_->completionCount() == 0) {
        completer_->popup()->hide();
        return;
    }
    QRect rect = cursorRect();
    rect.moveTopLeft(viewport()->mapTo(this, rect.topLeft()));
    rect.setWidth(std::clamp(completer_->popup()->sizeHintForColumn(0) + 32, 240, 640));
    completer_->complete(rect);
    int selectedRow = 0;
    for (int row = 0; row < completer_->completionCount(); ++row)
        if (completer_->completionModel()->index(row, 0).data().toString() == selected) {
            selectedRow = row;
            break;
        }
    completer_->popup()->setCurrentIndex(completer_->completionModel()->index(selectedRow, 0));
}

void CodeEditor::dismissCompletion() {
    completionTimer_.stop();
    completionAllowed_ = false;
    serverWords_.clear();
    serverEdits_.clear();
    if (completer_)
        completer_->popup()->hide();
}

void CodeEditor::repositionCompletion() {
    if (!completer_ || !completer_->popup()->isVisible())
        return;
    if (completionTimer_.isActive())
        return;
    if (!completionAllowed_ || completionPosition_ != textCursor().position() ||
        completionRevision_ != sourceRevision(document()) || !viewport()->rect().intersects(cursorRect())) {
        dismissCompletion();
        return;
    }
    auto rectangle = cursorRect();
    rectangle.moveTopLeft(viewport()->mapTo(this, rectangle.topLeft()));
    rectangle.setWidth(completer_->popup()->width());
    const auto selected = completer_->popup()->currentIndex();
    completer_->complete(rectangle);
    if (selected.isValid())
        completer_->popup()->setCurrentIndex(selected);
}

void CodeEditor::updateDocumentWords() {
    const int block = textCursor().blockNumber();
    if (wordsRevision_ == sourceRevision(document()) && wordsBlock_ == block)
        return;
    wordsRevision_ = sourceRevision(document());
    wordsBlock_ = block;
    documentWords_.clear();
    auto window = QTextCursor(document());
    window.setPosition(std::max(0, textCursor().position() - 32768));
    window.setPosition(std::min(document()->characterCount() - 1, textCursor().position() + 32768),
                       QTextCursor::KeepAnchor);
    const auto text = window.selectedText().replace(QChar::ParagraphSeparator, '\n');
    const auto masked = QString::fromStdString(ts::maskCommentsAndStrings(text.toStdString()));
    static const QRegularExpression identifiers(R"([\p{L}_][\p{L}\p{M}\p{N}_]*)");
    auto matches = identifiers.globalMatch(masked);
    QSet<QString> seen;
    while (matches.hasNext() && seen.size() < 20000) {
        const auto word = matches.next().captured();
        if (word.size() > 1 && word.size() <= 256)
            seen.insert(word);
    }
    documentWords_ = seen.values();
    documentWords_.sort(Qt::CaseInsensitive);
}

void CodeEditor::mousePressEvent(QMouseEvent* event) {
    invalidateViewRestore();
    dismissCompletion();
    QPlainTextEdit::mousePressEvent(event);
}

void CodeEditor::focusOutEvent(QFocusEvent* event) {
    if (event->reason() != Qt::PopupFocusReason) {
        dismissCompletion();
        emit assistantDismissed();
    }
    QPlainTextEdit::focusOutEvent(event);
}

void CodeEditor::indentSelection(bool outdent) {
    auto original = textCursor();
    QTextCursor work(document());
    work.beginEditBlock();
    const auto first = document()->findBlock(original.selectionStart());
    const auto last = document()->findBlock(
        original.hasSelection() ? std::max(original.selectionStart(), original.selectionEnd() - 1)
                                : original.position());
    auto block = first;
    while (block.isValid()) {
        const auto next = block.next();
        work.setPosition(block.position());
        if (outdent) {
            const auto text = block.text();
            int remove = 0;
            while (remove < tabWidth_ && remove < text.size() && text[remove] == ' ')
                ++remove;
            if (remove == 0 && text.startsWith('\t'))
                remove = 1;
            if (remove > 0) {
                work.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, remove);
                work.removeSelectedText();
            }
        } else
            work.insertText(QString(tabWidth_, ' '));
        if (block == last)
            break;
        block = next;
    }
    work.endEditBlock();
    setTextCursor(original);
}

void CodeEditor::keyPressEvent(QKeyEvent* event) {
    invalidateViewRestore();
    if (isReadOnly()) {
        QPlainTextEdit::keyPressEvent(event);
        return;
    }
    QScopedValueRollback<bool> editing(editingKey_, true);
    if (!ghostText_.isEmpty() && event->key() == Qt::Key_Tab && event->modifiers() == Qt::NoModifier &&
        !textCursor().hasSelection() && !completer_->popup()->isVisible()) {
        auto cursor = textCursor();
        auto suggestion = ghostText_;
        const auto next = document()->characterAt(cursor.position());
        if (!suggestion.isEmpty() && QString(")] }\"'").contains(next) && suggestion.endsWith(next))
            suggestion.chop(1);
        setGhostText({});
        cursor.beginEditBlock();
        cursor.insertText(suggestion);
        cursor.endEditBlock();
        setTextCursor(cursor);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        setGhostText({});
        emit assistantDismissed();
    }
    if (completer_ && completer_->popup()->isVisible()) {
        if (event->key() == Qt::Key_Escape) {
            dismissCompletion();
            setFocus(Qt::OtherFocusReason);
            event->accept();
            return;
        }
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
             event->key() == Qt::Key_Tab) &&
            (event->modifiers() == Qt::NoModifier || event->modifiers() == Qt::KeypadModifier)) {
            const auto selected = completer_->popup()->currentIndex().data().toString();
            if (!selected.isEmpty())
                insertSuggestion(selected);
            dismissCompletion();
            event->accept();
            return;
        }
        if ((event->key() == Qt::Key_Up || event->key() == Qt::Key_Down || event->key() == Qt::Key_PageUp ||
             event->key() == Qt::Key_PageDown) &&
            event->modifiers() == Qt::NoModifier) {
            const int count = completer_->completionCount();
            const int row = completer_->popup()->currentIndex().row();
            const bool down = event->key() == Qt::Key_Down || event->key() == Qt::Key_PageDown;
            const bool page = event->key() == Qt::Key_PageUp || event->key() == Qt::Key_PageDown;
            const int next = page ? std::clamp(row + (down ? 1 : -1) * completer_->maxVisibleItems(), 0,
                                               std::max(0, count - 1))
                                  : (row + (down ? 1 : -1) + count) % std::max(1, count);
            const auto index = completer_->completionModel()->index(next, 0);
            completer_->popup()->setCurrentIndex(index);
            completer_->popup()->scrollTo(index);
            event->accept();
            return;
        }
    }
    if (event->key() == Qt::Key_Space && event->modifiers() == Qt::ControlModifier) {
        completionTimer_.stop();
        completionAllowed_ = true;
        completionPosition_ = textCursor().position();
        completionRevision_ = sourceRevision(document());
        updateDocumentWords();
        if (completionRequest_)
            completionRequest_();
        showCompletion();
        event->accept();
        return;
    }
    switch (event->key()) {
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Home:
        case Qt::Key_End:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
        case Qt::Key_Escape:
        case Qt::Key_Backtab:
            dismissCompletion();
            break;
    }
    if (event->key() == Qt::Key_Backtab) {
        indentSelection(true);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Tab && event->modifiers() == Qt::NoModifier) {
        if (textCursor().hasSelection())
            indentSelection(false);
        else {
            auto cursor = textCursor();
            cursor.insertText(QString(tabWidth_ - cursor.positionInBlock() % tabWidth_, ' '));
            setTextCursor(cursor);
        }
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
        event->modifiers() == Qt::NoModifier) {
        auto cursor = textCursor();
        const auto prefix = cursor.block().text().left(cursor.positionInBlock());
        int leading = 0;
        while (leading < prefix.size() && (prefix[leading] == ' ' || prefix[leading] == '\t'))
            ++leading;
        const auto indent = prefix.left(leading);
        const bool block = prefix.trimmed().endsWith('{');
        const bool closing = block && document()->characterAt(cursor.position()) == '}';
        cursor.beginEditBlock();
        cursor.insertText("\n" + indent + (block ? QString(tabWidth_, ' ') : QString{}));
        const int position = cursor.position();
        if (closing)
            cursor.insertText("\n" + indent);
        cursor.setPosition(position);
        cursor.endEditBlock();
        setTextCursor(cursor);
        event->accept();
        return;
    }
    if (event->modifiers() == Qt::NoModifier || event->modifiers() == Qt::ShiftModifier) {
        auto cursor = textCursor();
        const auto text = event->text();
        const QString opens = "([{\"\'";
        const QString closing = ")]}\"\'";
        const bool quote = text == "\"" || text == "'";
        bool escaped = false;
        for (int index = cursor.position() - 1; index >= 0 && document()->characterAt(index) == '\\'; --index)
            escaped = !escaped;
        if (autoPairs_ && quote && !escaped && !cursor.hasSelection() &&
            document()->characterAt(cursor.position()) == text.front()) {
            cursor.movePosition(QTextCursor::Right);
            setTextCursor(cursor);
            event->accept();
            return;
        }
        const auto next = document()->characterAt(cursor.position());
        const auto previous = document()->characterAt(cursor.position() - 1);
        const auto beforeQuote = cursor.block().text().left(cursor.positionInBlock());
        const auto prefixMatch = QRegularExpression("[A-Za-z_][A-Za-z_0-9]*$").match(beforeQuote);
        bool prefixedQuote = false;
        if (prefixMatch.hasMatch())
            for (const auto& prefix : ts::profileFor(language_).quotePrefixes)
                prefixedQuote = prefixedQuote || prefixMatch.captured().toStdString() == prefix;
        const bool quoteContext =
            !quote || (!escaped && (!previous.isLetterOrNumber() || cursor.hasSelection() || prefixedQuote) &&
                       !next.isLetterOrNumber());
        if (autoPairs_ && quoteContext && text.size() == 1 && opens.contains(text)) {
            const bool hadSelection = cursor.hasSelection();
            const bool backwards = cursor.position() < cursor.anchor();
            const int start = cursor.selectionStart();
            const auto selected = cursor.selectedText().replace(QChar::ParagraphSeparator, '\n');
            cursor.beginEditBlock();
            const int index = opens.indexOf(text);
            cursor.insertText(text + selected + closing[index]);
            cursor.movePosition(QTextCursor::Left);
            if (hadSelection) {
                cursor.setPosition(backwards ? start + 1 + selected.size() : start + 1);
                cursor.setPosition(backwards ? start + 1 : start + 1 + selected.size(),
                                   QTextCursor::KeepAnchor);
            }
            cursor.endEditBlock();
            setTextCursor(cursor);
            event->accept();
            return;
        }
        if (autoPairs_ && text.size() == 1 && closing.contains(text) && !cursor.hasSelection() &&
            document()->characterAt(cursor.position()) == text.front()) {
            cursor.movePosition(QTextCursor::Right);
            setTextCursor(cursor);
            event->accept();
            return;
        }
        if (autoPairs_ && event->key() == Qt::Key_Backspace && !cursor.hasSelection() &&
            cursor.position() > 0) {
            const auto left = document()->characterAt(cursor.position() - 1),
                       right = document()->characterAt(cursor.position());
            const int index = opens.indexOf(left);
            if (index >= 0 && right == closing[index]) {
                cursor.beginEditBlock();
                cursor.movePosition(QTextCursor::Left);
                cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 2);
                cursor.removeSelectedText();
                cursor.endEditBlock();
                setTextCursor(cursor);
                event->accept();
                return;
            }
        }
    }
    QPlainTextEdit::keyPressEvent(event);
}

void CodeEditor::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        return;
    }
    QPlainTextEdit::dragEnterEvent(event);
}

void CodeEditor::dropEvent(QDropEvent* event) {
    if (event->mimeData()->hasUrls()) {
        for (const auto& url : event->mimeData()->urls())
            if (url.isLocalFile())
                emit openFileRequested(url.toLocalFile());
        event->acceptProposedAction();
        return;
    }
    QPlainTextEdit::dropEvent(event);
}

void CodeEditor::paintEvent(QPaintEvent* event) {
    QPlainTextEdit::paintEvent(event);
    if (!ghostText_.isEmpty() && hasFocus() && !completer_->popup()->isVisible()) {
        QPainter ghost(viewport());
        ghost.setFont(font());
        ghost.setPen(tsui::ThemeManager::color(tsui::Role::Muted));
        const auto rectangle = cursorRect();
        const auto lines = ghostText_.split('\n');
        for (int index = 0; index < std::min(4, int(lines.size())); ++index)
            ghost.drawText(rectangle.left() + 1,
                           rectangle.top() + fontMetrics().ascent() + index * fontMetrics().height(),
                           lines[index]);
    }
    if (!indentGuides_)
        return;
    QPainter painter(viewport());
    painter.setPen(QPen(tsui::ThemeManager::color(tsui::Role::Grid), 1, Qt::DotLine));
    const auto offset = contentOffset();
    const auto advance = QFontMetricsF(document()->defaultFont()).horizontalAdvance(' ');
    auto block = firstVisibleBlock();
    while (block.isValid()) {
        const auto geometry = blockBoundingGeometry(block).translated(offset);
        if (geometry.top() > viewport()->height())
            break;
        if (block.isVisible() && geometry.bottom() >= 0) {
            const auto text = block.text();
            int indent = 0;
            for (const auto ch : text) {
                if (ch == ' ')
                    ++indent;
                else if (ch == '\t')
                    indent += tabWidth_ - indent % tabWidth_;
                else
                    break;
                if (indent >= 256)
                    break;
            }
            for (int column = 0; column < indent; column += tabWidth_) {
                const qreal x = offset.x() + document()->documentMargin() + column * advance;
                if (x >= 0)
                    painter.drawLine(QPointF(x, geometry.top()), QPointF(x, geometry.bottom()));
            }
        }
        block = block.next();
    }
}

void CodeEditor::setGhostText(const QString& text) {
    if (ghostText_ == text)
        return;
    ghostText_ = text;
    setProperty("ghostText", text);
    viewport()->update();
}

void CodeEditor::toggleBreakpointAt(int y) {
    const auto block = document()->findBlockByNumber(lineAtY(y) - 1);
    if (block.isValid()) {
        toggleMarker(markers(document())->breakpoints, block);
        syncMarkers();
    }
}

int CodeEditor::annotationWidth() const {
    return annotations_->enabled() ? std::clamp(fontMetrics().horizontalAdvance('M') * 27, 180, 340) : 0;
}

int CodeEditor::lineAtY(int y) const {
    auto block = firstVisibleBlock();
    while (block.isValid()) {
        const auto rectangle = blockBoundingGeometry(block).translated(contentOffset());
        if (rectangle.top() > y)
            break;
        if (block.isVisible() && rectangle.bottom() > y)
            return block.blockNumber() + 1;
        block = block.next();
    }
    return -1;
}

void CodeEditor::selectLines(int anchorLine, int line) {
    if (line < 1 || anchorLine < 1)
        return;
    dismissCompletion();
    const auto first = document()->findBlockByNumber(std::min(anchorLine, line) - 1);
    const auto last = document()->findBlockByNumber(std::max(anchorLine, line) - 1);
    if (!first.isValid() || !last.isValid())
        return;
    const int start = first.position();
    const int end = last.next().isValid() ? last.next().position() : document()->characterCount() - 1;
    QTextCursor cursor(document());
    cursor.setPosition(line < anchorLine ? end : start);
    cursor.setPosition(line < anchorLine ? start : end, QTextCursor::KeepAnchor);
    setTextCursor(cursor);
}

void CodeEditor::toggleBookmark(int line) {
    const auto block = document()->findBlockByNumber(line - 1);
    if (!block.isValid())
        return;
    toggleMarker(markers(document())->bookmarks, block);
    syncMarkers();
}

QList<int> CodeEditor::bookmarkedLines() const {
    QSet<int> lines;
    for (const auto& cursor : markers(document())->bookmarks)
        lines.insert(cursor.blockNumber() + 1);
    auto result = lines.values();
    std::sort(result.begin(), result.end());
    return result;
}

void CodeEditor::navigateBookmark(bool forward) {
    const auto lines = bookmarkedLines();
    if (lines.isEmpty())
        return;
    const int current = textCursor().blockNumber() + 1;
    if (forward) {
        const auto next = std::upper_bound(lines.begin(), lines.end(), current);
        jumpToLine(next == lines.end() ? lines.first() : *next);
    } else {
        const auto next = std::lower_bound(lines.begin(), lines.end(), current);
        jumpToLine(next == lines.begin() ? lines.last() : *std::prev(next));
    }
}

void CodeEditor::syncMarkers() {
    QVariantList bookmarks;
    for (int line : bookmarkedLines())
        bookmarks.append(line);
    document()->setProperty("bookmarks", bookmarks);
    QVariantList points;
    QSet<int> unique;
    for (const auto& cursor : markers(document())->breakpoints)
        unique.insert(cursor.blockNumber() + 1);
    auto lines = unique.values();
    std::sort(lines.begin(), lines.end());
    for (int line : lines)
        points.append(line);
    if (document()->property("breakpoints").toList() != points) {
        document()->setProperty("breakpoints", points);
        emit breakpointChanged();
    }
    lineNumbers_->update();
}

void CodeEditor::wireDocument() {
    disconnect(markerConnection_);
    markers(document());
    markerConnection_ =
        connect(document(), &QTextDocument::contentsChange, this, [this](int, int removed, int added) {
            if (removed || added) {
                syncMarkers();
                emit sourceTextChanged();
            }
        });
}

void CodeEditor::setFilePath(const QString& path) {
    annotations_->setDocument(document(), path);
}

void CodeEditor::setAnnotationsVisible(bool visible) {
    if (!gitAnnotationsAvailable_)
        return;
    annotations_->setEnabled(visible);
    QSettings().setValue("editor/gitAnnotations", visible);
    updateLineNumberAreaWidth(0);
    lineNumbers_->update();
    emit annotationsVisibilityChanged(visible);
}

void CodeEditor::setGitAnnotationsAvailable(bool available) {
    gitAnnotationsAvailable_ = available;
    annotations_->setEnabled(available && QSettings().value("editor/gitAnnotations", false).toBool());
    updateLineNumberAreaWidth(0);
    lineNumbers_->update();
}

void CodeEditor::changeEvent(QEvent* event) {
    QPlainTextEdit::changeEvent(event);
    if (event->type() == QEvent::FontChange && lineNumbers_) {
        document()->setDefaultFont(font());
        if (highlighter_)
            highlighter_->refreshTheme();
        setTabStopDistance(QFontMetricsF(font()).horizontalAdvance(' ') * tabWidth_);
        updateLineNumberAreaWidth(0);
    }
}

void CodeEditor::inputMethodEvent(QInputMethodEvent* event) {
    dismissCompletion();
    const QScopedValueRollback<bool> editing(editingKey_, true);
    QPlainTextEdit::inputMethodEvent(event);
}

void CodeEditor::insertFromMimeData(const QMimeData* source) {
    dismissCompletion();
    const QScopedValueRollback<bool> editing(editingKey_, true);
    QPlainTextEdit::insertFromMimeData(source);
}

void CodeEditor::setExecutionLine(int line) {
    executionLine_ = line;
    highlightCurrentLine();
}

void CodeHighlighter::setSemanticTokens(const QJsonArray& data, const QStringList& legend) {
    semantic_.clear();
    int line = 0, column = 0;
    for (int index = 0; index + 4 < data.size(); index += 5) {
        const int delta = data[index].toInt();
        line += delta;
        column = delta == 0 ? column + data[index + 1].toInt() : data[index + 1].toInt();
        const int type = data[index + 3].toInt();
        if (line < 0 || type < 0 || type >= legend.size())
            continue;
        semantic_[line].append({column, data[index + 2].toInt(), legend[type]});
    }
    semanticRevision_ = sourceRevision(document());
    rehighlight();
}

void CodeEditor::setSemanticTokens(const QJsonArray& data, const QStringList& legend) {
    if (highlighter_)
        highlighter_->setSemanticTokens(data, legend);
}
