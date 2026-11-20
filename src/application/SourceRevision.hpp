#pragma once
#include <QTextDocument>
#include <QVariant>

inline int sourceRevision(QTextDocument* document) {
    if (!document->property("sourceRevision").isValid()) {
        document->setProperty("sourceRevision", 0);
        QObject::connect(
            document, &QTextDocument::contentsChange, document, [document](int, int removed, int added) {
                if (removed || added)
                    document->setProperty("sourceRevision", document->property("sourceRevision").toInt() + 1);
            });
    }
    return document->property("sourceRevision").toInt();
}
