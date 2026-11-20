#include "DocumentController.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPlainTextDocumentLayout>
#include <QSaveFile>
#include <QStringDecoder>
#include <algorithm>

namespace {
QString normalized(const QString& path) {
    const QFileInfo info(path);
    return info.exists() ? info.canonicalFilePath() : info.absoluteFilePath();
}
}

std::unique_ptr<EditorDocument> DocumentController::makeDocument(const QString& path, const QString& text,
                                                                 const QString& name) {
    auto result = std::make_unique<EditorDocument>();
    result->text = std::make_unique<QTextDocument>();
    result->text->setDocumentLayout(new QPlainTextDocumentLayout(result->text.get()));
    result->path = path;
    result->name = name;
    result->text->setPlainText(text);
    result->text->setModified(false);
    return result;
}

int DocumentController::indexOf(const EditorDocument* document) const {
    for (int i = 0; i < count(); ++i)
        if (documents_[i].get() == document)
            return i;
    return -1;
}

void DocumentController::wire(EditorDocument* document) {
    connect(document->text.get(), &QTextDocument::modificationChanged, this,
            [this, document] { emit documentChanged(indexOf(document)); });
    connect(document->text.get(), &QTextDocument::contentsChange, this,
            [this, document](int, int removed, int added) {
                if (!removed && !added)
                    return;
                document->trusted = false;
                emit documentChanged(indexOf(document));
            });
}

int DocumentController::create(const QString& text, const QString& name) {
    if (count() >= 50)
        return -1;
    emit aboutToChangeActive();
    auto value =
        makeDocument({}, text, name.isEmpty() ? QString("untitled-%1.cpp").arg(nextUntitled_++) : name);
    wire(value.get());
    documents_.pushBack(std::move(value));
    active_ = count() - 1;
    emit documentsChanged();
    emit activeChanged(active_);
    return active_;
}

bool DocumentController::readUtf8(const QString& path, QString& text, QString& error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
        return false;
    }
    if (file.size() > 32 * 1024 * 1024) {
        error = "The editor is limited to 32 MiB per document.";
        return false;
    }
    const auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        error = file.errorString();
        return false;
    }
    QStringDecoder decoder(QStringDecoder::Utf8);
    text = decoder(bytes);
    if (decoder.hasError() || text.contains(QChar(0))) {
        error = "The file must contain valid UTF-8 text without null bytes.";
        return false;
    }
    error.clear();
    return true;
}

bool DocumentController::open(const QString& path, QString& error) {
    const auto canonical = normalized(path);
    for (int i = 0; i < count(); ++i)
        if (documents_[i]->path == canonical) {
            activate(i);
            return true;
        }
    if (count() >= 50) {
        error = "Maximum number of open documents reached: 50.";
        return false;
    }
    QString text;
    if (!readUtf8(canonical, text, error))
        return false;
    emit aboutToChangeActive();
    auto value = makeDocument(canonical, text, QFileInfo(canonical).fileName());
    wire(value.get());
    documents_.pushBack(std::move(value));
    active_ = count() - 1;
    emit documentsChanged();
    emit activeChanged(active_);
    return true;
}

void DocumentController::activate(int index) {
    if (index == active_ || !document(index))
        return;
    emit aboutToChangeActive();
    active_ = index;
    emit activeChanged(active_);
}

void DocumentController::moveDocument(int from, int to) {
    if (from == to || !document(from) || !document(to))
        return;
    emit aboutToChangeActive();
    auto* active = activeDocument();
    auto moved = std::move(documents_[from]);
    if (from < to)
        for (int i = from; i < to; ++i)
            documents_[i] = std::move(documents_[i + 1]);
    else
        for (int i = from; i > to; --i)
            documents_[i] = std::move(documents_[i - 1]);
    documents_[to] = std::move(moved);
    active_ = indexOf(active);
    emit documentsChanged();
    emit activeChanged(active_);
}

bool DocumentController::save(int index, const QString& requestedPath, QString& error) {
    auto* value = document(index);
    if (!value) {
        error = "Document not found.";
        return false;
    }
    const auto target = requestedPath.isEmpty() ? value->path : normalized(requestedPath);
    if (target.isEmpty()) {
        error = "No save path selected.";
        return false;
    }
    for (int i = 0; i < count(); ++i)
        if (i != index && documents_[i]->path == target) {
            error = "The file is already open in another tab.";
            return false;
        }
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly)) {
        error = file.errorString();
        return false;
    }
    const auto text = value->text->toPlainText().toUtf8();
    if (file.write(text) != text.size() || !file.commit()) {
        error = file.errorString();
        return false;
    }
    value->path = normalized(target);
    value->name = QFileInfo(value->path).fileName();
    value->text->setModified(false);
    error.clear();
    emit documentChanged(index);
    emit documentSaved(value->path);
    return true;
}

bool DocumentController::saveAll(const SavePath& choose, QString& error) {
    for (int i = 0; i < count(); ++i)
        if (documents_[i]->text->isModified() || documents_[i]->path.isEmpty()) {
            const auto target = documents_[i]->path.isEmpty() ? choose(*documents_[i]) : documents_[i]->path;
            if (target.isEmpty()) {
                error = "Save cancelled.";
                return false;
            }
            if (!save(i, target, error))
                return false;
        }
    return true;
}

bool DocumentController::close(int index, const Decide& decide, const SavePath& choose, QString& error) {
    auto* value = document(index);
    if (!value)
        return true;
    if (value->text->isModified()) {
        const auto decision = decide(*value);
        if (decision == CloseDecision::Cancel)
            return false;
        if (decision == CloseDecision::Save) {
            const auto path = value->path.isEmpty() ? choose(*value) : value->path;
            if (path.isEmpty() || !save(index, path, error))
                return false;
        }
    }
    emit aboutToChangeActive();
    const QString path = value->path;
    auto removed = std::move(documents_[index]);
    documents_.eraseAt(index);
    if (index < active_)
        --active_;
    else if (index == active_)
        active_ = std::min(index, count() - 1);
    emit documentClosed(path);
    if (documents_.empty()) {
        active_ = -1;
        emit documentsChanged();
        emit activeChanged(-1);
    } else {
        emit documentsChanged();
        emit activeChanged(active_);
    }
    return true;
}

bool DocumentController::prepareClose(const Decide& decide, const SavePath& choose, QString& error) {
    ts::Array<int> discarded;
    for (int i = 0; i < count(); ++i) {
        auto& value = *documents_[i];
        if (!value.text->isModified())
            continue;
        const auto decision = decide(value);
        if (decision == CloseDecision::Cancel)
            return false;
        if (decision == CloseDecision::Save) {
            const auto path = value.path.isEmpty() ? choose(value) : value.path;
            if (path.isEmpty() || !save(i, path, error))
                return false;
        } else
            discarded.pushBack(i);
    }
    for (int index : discarded) {
        auto& value = *documents_[index];
        QString restored;
        if (!value.path.isEmpty() && readUtf8(value.path, restored, error))
            value.text->setPlainText(restored);
        else
            value.text->setPlainText({});
        value.text->setModified(false);
    }
    error.clear();
    return true;
}

bool DocumentController::renamePath(const QString& from, const QString& to, QString& error) {
    const auto source = normalized(from), target = normalized(to);
    if (source == target)
        return true;
    if (QFileInfo::exists(target)) {
        error = "The path already exists.";
        return false;
    }
    if (!QDir().rename(source, target)) {
        error = "Could not rename the path.";
        return false;
    }
    for (int i = 0; i < count(); ++i) {
        auto& value = *documents_[i];
        if (value.path == source || value.path.startsWith(source + '/')) {
            value.path = target + value.path.mid(source.size());
            value.name = QFileInfo(value.path).fileName();
            emit documentChanged(i);
        }
    }
    error.clear();
    return true;
}

bool DocumentController::saveSession(const QString& path, QString& error) const {
    QJsonArray entries;
    for (const auto& pointer : documents_) {
        const auto& value = *pointer;
        QJsonObject entry{
            {"path", value.path},
            {"name", value.name},
            {"modified", value.text->isModified()},
            {"position", value.position},
            {"anchor", value.anchor},
            {"bookmarks", QJsonArray::fromVariantList(value.text->property("bookmarks").toList())},
            {"breakpoints", QJsonArray::fromVariantList(value.text->property("breakpoints").toList())},
            {"vertical", value.verticalScroll},
            {"horizontal", value.horizontalScroll}};
        if (value.path.isEmpty() || value.text->isModified())
            entry["text"] = value.text->toPlainText();
        entries.append(entry);
    }
    const auto bytes = QJsonDocument(QJsonObject{{"format", "veyra-session"},
                                                 {"version", 1},
                                                 {"active", active_},
                                                 {"documents", entries}})
                           .toJson(QJsonDocument::Compact);
    if (bytes.size() > 64 * 1024 * 1024) {
        error = "The session exceeds the 64 MiB limit.";
        return false;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        error = "Could not create the session directory.";
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error = file.errorString();
        return false;
    }
    error.clear();
    return true;
}

bool DocumentController::restoreSession(const QString& path, QString& error) {
    QFile file(path);
    if (!file.exists())
        return true;
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024) {
        error = "The session is unavailable or too large.";
        return false;
    }
    QJsonParseError parse;
    const auto json = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !json.isObject()) {
        error = "Invalid session: " + parse.errorString();
        return false;
    }
    const auto root = json.object();
    const auto list = root.value("documents").toArray();
    if (root.value("format") != "veyra-session" || root.value("version").toInt() != 1 || list.size() > 50) {
        error = "Unsupported session.";
        return false;
    }
    ts::Array<std::unique_ptr<EditorDocument>> restored;
    QStringList skipped;
    ts::Map<std::string, bool> seen;
    int restoredActive = 0, originalIndex = 0;
    const int wanted = root.value("active").toInt();
    for (const auto& entry : list) {
        const auto object = entry.toObject();
        const QString name = object.value("path").toString();
        QString text;
        if (!name.isEmpty()) {
            const auto canonical = normalized(name).toStdString();
            if (seen.contains(canonical)) {
                error = "The session contains a duplicate path: " + name;
                return false;
            }
            seen.insertOrAssign(canonical, true);
        }
        const int currentIndex = originalIndex++;
        if (object.contains("text"))
            text = object.value("text").toString();
        else if (!readUtf8(name, text, error)) {
            skipped << name + ": " + error;
            continue;
        }
        if (currentIndex == wanted)
            restoredActive = static_cast<int>(restored.size());
        if (text.toUtf8().size() > 32 * 1024 * 1024) {
            error = "A session document exceeds 32 MiB.";
            return false;
        }
        auto value = makeDocument(name, text, object.value("name").toString());
        value->text->setModified(object.value("modified").toBool());
        value->position = object.value("position").toInt();
        value->anchor = object.value("anchor").toInt();
        value->text->setProperty("bookmarks", object.value("bookmarks").toArray().toVariantList());
        value->text->setProperty("breakpoints", object.value("breakpoints").toArray().toVariantList());
        value->verticalScroll = object.value("vertical").toInt();
        value->horizontalScroll = object.value("horizontal").toInt();
        restored.pushBack(std::move(value));
    }
    if (restored.empty()) {
        error = skipped.join("\n");
        return true;
    }
    emit aboutToChangeActive();
    auto removed = std::move(documents_);
    documents_ = std::move(restored);
    for (auto& value : documents_)
        wire(value.get());
    active_ = std::clamp(restoredActive, 0, count() - 1);
    emit documentsChanged();
    emit activeChanged(active_);
    error = skipped.isEmpty() ? QString() : "Files not restored:\n" + skipped.join("\n");
    return true;
}
