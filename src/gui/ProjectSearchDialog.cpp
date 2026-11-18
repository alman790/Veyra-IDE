#include "../application/ProjectSearch.hpp"
#include "MainWindow.hpp"
#include "UiLanguage.hpp"
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>

void MainWindow::findProjectFiles() {
    showProjectSearch(true);
}

void MainWindow::findProjectText() {
    showProjectSearch(false);
}

void MainWindow::showProjectSearch(bool filenames) {
    const QString root = workspaceRoot_.isEmpty() ? QFileInfo(sourcePath_).absolutePath() : workspaceRoot_;
    QDialog dialog(this);
    dialog.setWindowTitle(filenames ? UiLanguage::tr("Find project file")
                                    : UiLanguage::tr("Find text in project"));
    dialog.resize(800, 560);
    auto* layout = new QVBoxLayout(&dialog);
    auto* directory = new QLabel(UiLanguage::tr("Project: ") + root);
    directory->setWordWrap(true);
    layout->addWidget(directory);
    auto* query = new QLineEdit;
    query->setPlaceholderText(filenames ? UiLanguage::tr("Part of a filename")
                                        : UiLanguage::tr("Case-insensitive search text"));
    layout->addWidget(query);
    auto* row = new QHBoxLayout;
    auto* start = new QPushButton(UiLanguage::tr("Find"));
    auto* stop = new QPushButton(UiLanguage::tr("Stop"));
    stop->setEnabled(false);
    row->addWidget(start);
    row->addWidget(stop);
    row->addStretch();
    layout->addLayout(row);
    auto* status = new QLabel(
        UiLanguage::tr("Search scans UTF-8 text files. Build and dependency directories are excluded."));
    status->setWordWrap(true);
    layout->addWidget(status);
    auto* list = new QListWidget;
    list->setWordWrap(true);
    layout->addWidget(list, 1);
    BackgroundJob job(&dialog);
    connect(start, &QPushButton::clicked, &dialog, [&, root] {
        if (job.busy() || query->text().trimmed().isEmpty())
            return;
        const QString term = query->text();
        auto result = std::make_shared<ProjectSearchResult>();
        list->clear();
        start->setEnabled(false);
        stop->setEnabled(true);
        status->setText(UiLanguage::tr("Searching…"));
        job.start([=](const auto& cancelled,
                      const auto&) { *result = ProjectSearch::scan(root, term, filenames, cancelled); },
                  [&, result](const QString& error, bool cancelled) {
                      start->setEnabled(true);
                      stop->setEnabled(false);
                      const auto issue = error.isEmpty() ? result->error : error;
                      status->setText(
                          !issue.isEmpty()
                              ? issue
                              : QString(UiLanguage::tr("Found: %1 · files scanned: %2%3%4"))
                                    .arg(result->matches.size())
                                    .arg(result->files)
                                    .arg(cancelled ? UiLanguage::tr(" · cancelled") : "")
                                    .arg(result->truncated ? UiLanguage::tr(" · search limit reached") : ""));
                      for (const auto& match : result->matches) {
                          auto* item = new QListWidgetItem(QString("%1:%2:%3  %4")
                                                               .arg(QDir(root).relativeFilePath(match.path))
                                                               .arg(match.line)
                                                               .arg(match.column)
                                                               .arg(match.text),
                                                           list);
                          item->setData(Qt::UserRole, match.path);
                          item->setData(Qt::UserRole + 1, match.line);
                          item->setData(Qt::UserRole + 2, match.column);
                      }
                  });
    });
    connect(query, &QLineEdit::returnPressed, start, &QPushButton::click);
    connect(stop, &QPushButton::clicked, &job, &BackgroundJob::cancel);
    connect(&dialog, &QDialog::finished, &job, &BackgroundJob::cancel);
    connect(list, &QListWidget::itemActivated, &dialog, [&](QListWidgetItem* item) {
        if (loadCodeFile(item->data(Qt::UserRole).toString())) {
            editor_->jumpToLine(item->data(Qt::UserRole + 1).toInt());
            auto cursor = editor_->textCursor();
            cursor.movePosition(QTextCursor::StartOfBlock);
            cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor,
                                item->data(Qt::UserRole + 2).toInt() - 1);
            editor_->setTextCursor(cursor);
            dialog.accept();
        }
    });
    if (filenames && projectQuery_ && !projectQuery_->text().isEmpty()) {
        query->setText(projectQuery_->text());
        QTimer::singleShot(0, start, &QPushButton::click);
    }
    query->setFocus();
    dialog.exec();
}
