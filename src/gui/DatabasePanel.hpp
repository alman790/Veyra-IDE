#pragma once
#include "DatabaseSession.hpp"
#include <QAbstractTableModel>
#include <QJsonArray>
#include <QMap>
#include <QWidget>

class QTableView;
class QListWidget;
class QLineEdit;
class QPlainTextEdit;
class QLabel;
class QPushButton;
class QSpinBox;
class QSortFilterProxyModel;

class DatabaseGridModel final : public QAbstractTableModel {
    QJsonArray columns_, rows_;
    QMap<QPair<int, int>, QJsonObject> changes_;
    QJsonObject cell(const QModelIndex& index) const;

  public:
    using QAbstractTableModel::QAbstractTableModel;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    void setNull(const QModelIndex& index);
    void load(const QJsonObject& page);
    void discard();
    QJsonArray changes() const;

    bool dirty() const {
        return !changes_.isEmpty();
    }
};

class DatabasePanel final : public QWidget {
    Q_OBJECT
    DatabaseSession* session_;
    DatabaseGridModel* model_;
    QSortFilterProxyModel* proxy_;
    QListWidget* tables_;
    QTableView* grid_;
    QPlainTextEdit* sql_;
    QLabel* status_;
    QPushButton *connect_, *disconnect_, *run_, *apply_, *discard_, *previous_, *next_, *stop_, *refresh_;
    QSpinBox* limit_;
    QJsonObject page_;
    QString table_, statement_;
    QString lastError_;
    bool sqlMode_ = false;
    bool allowDiscard();
    void configure();
    void load(int offset = 0);
    void updateControls();
    void exportCsv();

  public:
    explicit DatabasePanel(QWidget* parent = nullptr);

    bool hasPendingChanges() const {
        return model_->dirty();
    }

    bool requestClose();

    void shutdown() {
        session_->cancel();
    }

  signals:
    void titleChanged(const QString& title);
};
