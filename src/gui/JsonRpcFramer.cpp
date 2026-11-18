#include "JsonRpcFramer.hpp"
#include <QJsonDocument>

QByteArray JsonRpcFramer::frame(const QJsonObject& message) {
    const auto json = QJsonDocument(message).toJson(QJsonDocument::Compact);
    return "Content-Length: " + QByteArray::number(json.size()) + "\r\n\r\n" + json;
}

bool JsonRpcFramer::feed(const QByteArray& bytes, const std::function<void(const QJsonObject&)>& receive,
                         QString& error) {
    error.clear();
    buffer_ += bytes;
    auto fail = [&](const QString& reason) {
        error = reason;
        clear();
        return false;
    };
    if (buffer_.size() > 16 * 1024 * 1024)
        return fail("JSON-RPC buffer exceeds 16 MiB");
    while (true) {
        if (expected_ < 0) {
            const auto end = buffer_.indexOf("\r\n\r\n");
            if (end < 0) {
                if (buffer_.size() > 65536)
                    return fail("JSON-RPC header exceeds 64 KiB");
                return true;
            }
            int length = -1;
            for (auto line : buffer_.left(end).split('\n')) {
                line = line.trimmed();
                const auto colon = line.indexOf(':');
                if (colon < 0)
                    return fail("Malformed JSON-RPC header");
                if (line.left(colon).toLower() == "content-length") {
                    if (length >= 0)
                        return fail("Duplicate Content-Length");
                    bool ok = false;
                    const auto parsed = line.mid(colon + 1).trimmed().toLongLong(&ok);
                    if (!ok || parsed < 1 || parsed > 8 * 1024 * 1024)
                        return fail("Invalid Content-Length");
                    length = static_cast<int>(parsed);
                }
            }
            if (length < 0)
                return fail("Missing Content-Length");
            expected_ = length;
            buffer_.remove(0, end + 4);
        }
        if (buffer_.size() < expected_)
            return true;
        const auto payload = buffer_.left(expected_);
        buffer_.remove(0, expected_);
        expected_ = -1;
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(payload, &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject())
            return fail("Invalid JSON-RPC object");
        receive(document.object());
    }
}
