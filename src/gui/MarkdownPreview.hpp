#pragma once
#include <QDir>
#include <QFileInfo>
#include <QTextBrowser>

class MarkdownPreview final : public QTextBrowser {
    QString folder_;

  public:
    explicit MarkdownPreview(QWidget* parent = nullptr) : QTextBrowser(parent) {
        setObjectName("markdownPreview");
        setOpenLinks(false);
        setOpenExternalLinks(false);
    }

    void render(const QString& text, const QString& path) {
        folder_ = QFileInfo(path).absolutePath();
        document()->setBaseUrl(QUrl::fromLocalFile(folder_ + '/'));
        setMarkdown(text);
    }

  protected:
    QVariant loadResource(int type, const QUrl& name) override {
        const auto url = document()->baseUrl().resolved(name);
        if (!url.isLocalFile())
            return {};
        const auto path = QFileInfo(url.toLocalFile()).canonicalFilePath();
        const auto root = QDir(folder_).canonicalPath();
        if (path.isEmpty() || root.isEmpty() || !(path == root || path.startsWith(root + '/')))
            return {};
        if (QFileInfo(path).size() > 8 * 1024 * 1024)
            return {};
        return QTextBrowser::loadResource(type, url);
    }
};
