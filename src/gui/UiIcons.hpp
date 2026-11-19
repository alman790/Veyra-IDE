#pragma once
#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QString>

namespace tsui {
inline QIcon icon(const QString& name, const QColor& ink = QColor("#A6A9B2")) {
    QPixmap pixmap(40, 40);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(2, 2);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(ink, 1.55, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (name == "close") {
        p.drawLine(QPointF(5, 5), QPointF(15, 15));
        p.drawLine(QPointF(15, 5), QPointF(5, 15));
    } else if (name == "folder") {
        QPainterPath path;
        path.moveTo(2, 5);
        path.lineTo(8, 5);
        path.lineTo(10, 7);
        path.lineTo(18, 7);
        path.lineTo(18, 16);
        path.lineTo(2, 16);
        path.closeSubpath();
        p.drawPath(path);
    } else if (name == "assistant") {
        p.drawRoundedRect(QRectF(2, 3, 16, 12), 3, 3);
        p.drawLine(QPointF(5, 15), QPointF(5, 18));
        p.drawLine(QPointF(5, 18), QPointF(9, 15));
        p.drawEllipse(QPointF(6, 9), 0.6, 0.6);
        p.drawEllipse(QPointF(10, 9), 0.6, 0.6);
        p.drawEllipse(QPointF(14, 9), 0.6, 0.6);
    } else if (name == "console") {
        p.drawRoundedRect(QRectF(2, 3, 16, 14), 1, 1);
        p.drawLine(QPointF(5, 7), QPointF(8, 10));
        p.drawLine(QPointF(8, 10), QPointF(5, 13));
        p.drawLine(QPointF(10, 13), QPointF(15, 13));
    } else if (name == "settings") {
        p.drawEllipse(QRectF(4, 4, 12, 12));
        p.drawEllipse(QRectF(8, 8, 4, 4));
        for (int i = 0; i < 8; ++i) {
            p.save();
            p.translate(10, 10);
            p.rotate(i * 45);
            p.drawLine(QPointF(0, -6), QPointF(0, -9));
            p.restore();
        }
    } else if (name == "inspect") {
        p.drawEllipse(QRectF(3, 3, 11, 11));
        p.drawLine(QPointF(13, 13), QPointF(18, 18));
        p.drawLine(QPointF(6, 8), QPointF(8, 10));
        p.drawLine(QPointF(8, 10), QPointF(12, 6));
    } else if (name == "code") {
        QPainterPath a;
        a.moveTo(7.8, 5);
        a.lineTo(3.3, 10);
        a.lineTo(7.8, 15);
        QPainterPath b;
        b.moveTo(12.2, 5);
        b.lineTo(16.7, 10);
        b.lineTo(12.2, 15);
        p.drawPath(a);
        p.drawPath(b);
    } else if (name == "play") {
        p.setBrush(ink);
        QPainterPath a;
        a.moveTo(6, 3.5);
        a.lineTo(16, 10);
        a.lineTo(6, 16.5);
        a.closeSubpath();
        p.drawPath(a);
    } else if (name == "stop") {
        p.setBrush(ink);
        p.drawRoundedRect(QRectF(4.5, 4.5, 11, 11), 1.5, 1.5);
    } else if (name == "build") {
        p.drawLine(QPointF(4, 16), QPointF(15, 5));
        p.drawLine(QPointF(12, 3), QPointF(17, 8));
        p.drawLine(QPointF(4, 16), QPointF(3, 17));
    } else if (name == "search") {
        p.drawEllipse(QRectF(3, 3, 10.5, 10.5));
        p.drawLine(QPointF(12.2, 12.2), QPointF(17.2, 17.2));
    } else if (name == "plugins") {
        p.drawRoundedRect(QRectF(4, 4, 12, 12), 2, 2);
        p.drawLine(QPointF(10, 1.5), QPointF(10, 4));
        p.drawLine(QPointF(10, 16), QPointF(10, 18.5));
        p.drawLine(QPointF(1.5, 10), QPointF(4, 10));
        p.drawLine(QPointF(16, 10), QPointF(18.5, 10));
    } else if (name == "file") {
        QPainterPath a;
        a.moveTo(4.5, 2.5);
        a.lineTo(12, 2.5);
        a.lineTo(16, 6.5);
        a.lineTo(16, 17.5);
        a.lineTo(4.5, 17.5);
        a.closeSubpath();
        p.drawPath(a);
        p.drawLine(QPointF(11.7, 2.8), QPointF(11.7, 7));
        p.drawLine(QPointF(11.7, 7), QPointF(15.6, 7));
    } else {
        p.drawEllipse(QRectF(5, 5, 10, 10));
    }
    p.end();
    pixmap.setDevicePixelRatio(2);
    return QIcon(pixmap);
}
}
