#include "DatabasePanel.hpp"
#include "CodeEditor.hpp"
#include "UiLanguage.hpp"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QSplitter>
#include <QSqlDatabase>
#include <QTableView>
#include <QTextStream>
#include <QVBoxLayout>

int DatabaseGridModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : rows_.size();
}

int DatabaseGridModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : columns_.size();
}

QJsonObject DatabaseGridModel::cell(const QModelIndex& index) const {
    if (!index.isValid() || index.row() >= rows_.size() || index.column() >= columns_.size())
        return {};
    auto result = rows_[index.row()].toArray()[index.column()].toObject();
    const auto changed = changes_.constFind({index.row(), index.column()});
    if (changed != changes_.cend()) {
        result["text"] = changed->value("text");
        result["null"] = changed->value("null");
    }
    return result;
}

QVariant DatabaseGridModel::data(const QModelIndex& index, int role) const {
    const auto value = cell(index);
    if (value.isEmpty())
        return {};
    if (role == Qt::DisplayRole) {
        if (value.value("null").toBool())
            return QStringLiteral("NULL");
        if (value.value("binary").toBool())
            return UiLanguage::tr("Binary: %1 bytes").arg(value.value("text").toString());
        return value.value("text").toString() +
               (value.value("truncated").toBool() ? QStringLiteral("…") : QString());
    }
    if (role == Qt::EditRole)
        return value.value("null").toBool() ? QString() : value.value("text").toString();
    if (role == Qt::UserRole + 1) {
        if (value.value("null").toBool())
            return {};
        QVariant result(value.value("text").toString());
        const auto type =
            QMetaType::fromName(columns_[index.column()].toObject().value("type").toString().toLatin1());
        if (type.isValid())
            result.convert(type);
        return result;
    }
    if (role == Qt::ToolTipRole)
        return columns_[index.column()].toObject().value("type").toString() +
               (value.value("truncated").toBool() ? " · " + UiLanguage::tr("Display truncated") : QString());
    if (role == Qt::BackgroundRole && changes_.contains({index.row(), index.column()}))
        return QColor(80, 130, 200, 65);
    if (role == Qt::ForegroundRole && value.value("null").toBool())
        return QColor(140, 150, 165);
    return {};
}

QVariant DatabaseGridModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole)
        return {};
    if (orientation == Qt::Vertical)
        return section + 1;
    return section >= 0 && section < columns_.size() ? columns_[section].toObject().value("name").toString()
                                                     : QVariant();
}

Qt::ItemFlags DatabaseGridModel::flags(const QModelIndex& index) const {
    if (!index.isValid())
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable |
           (cell(index).value("editable").toBool() ? Qt::ItemIsEditable : Qt::NoItemFlags);
}

bool DatabaseGridModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (role != Qt::EditRole || !(flags(index) & Qt::ItemIsEditable))
        return false;
    const auto original = rows_[index.row()].toArray()[index.column()].toObject();
    const auto key = qMakePair(index.row(), index.column());
    if (!original.value("null").toBool() && original.value("text").toString() == value.toString())
        changes_.remove(key);
    else
        changes_[key] = {
            {"row", index.row()}, {"column", index.column()}, {"text", value.toString()}, {"null", false}};
    emit dataChanged(index, index);
    return true;
}

void DatabaseGridModel::setNull(const QModelIndex& index) {
    if (!(flags(index) & Qt::ItemIsEditable))
        return;
    const auto key = qMakePair(index.row(), index.column());
    if (rows_[index.row()].toArray()[index.column()].toObject().value("null").toBool())
        changes_.remove(key);
    else
        changes_[key] = {{"row", index.row()}, {"column", index.column()}, {"null", true}};
    emit dataChanged(index, index);
}

void DatabaseGridModel::load(const QJsonObject& page) {
    beginResetModel();
    columns_ = page.value("columns").toArray();
    rows_ = page.value("rows").toArray();
    changes_.clear();
    endResetModel();
}

void DatabaseGridModel::discard() {
    beginResetModel();
    changes_.clear();
    endResetModel();
}

QJsonArray DatabaseGridModel::changes() const {
    QJsonArray result;
    for (const auto& change : changes_)
        result.append(change);
    return result;
}

DatabasePanel::DatabasePanel(QWidget* parent) : QWidget(parent) {
    setObjectName("databasePanel");
    session_ = new DatabaseSession(this);
    auto* layout = new QVBoxLayout(this);
    auto* bar = new QHBoxLayout;
    connect_ = new QPushButton(UiLanguage::tr("Connect…"));
    connect_->setObjectName("databaseConnect");
    disconnect_ = new QPushButton(UiLanguage::tr("Disconnect"));
    refresh_ = new QPushButton(UiLanguage::tr("Refresh page"));
    auto* exportButton = new QPushButton(UiLanguage::tr("Export displayed page…"));
    exportButton->setToolTip(
        UiLanguage::tr("Exports displayed values, including truncation and binary markers."));
    bar->addWidget(connect_);
    bar->addWidget(disconnect_);
    bar->addWidget(refresh_);
    bar->addStretch();
    bar->addWidget(exportButton);
    layout->addLayout(bar);
    auto* split = new QSplitter;
    tables_ = new QListWidget;
    tables_->setObjectName("databaseTables");
    split->addWidget(tables_);
    auto* right = new QWidget;
    auto* content = new QVBoxLayout(right);
    auto* filter = new QLineEdit;
    filter->setPlaceholderText(UiLanguage::tr("Filter loaded rows…"));
    content->addWidget(filter);
    model_ = new DatabaseGridModel(this);
    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setFilterKeyColumn(-1);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setSortRole(Qt::UserRole + 1);
    grid_ = new QTableView;
    grid_->setObjectName("databaseGrid");
    grid_->setModel(proxy_);
    grid_->setSortingEnabled(true);
    grid_->setAlternatingRowColors(true);
    grid_->setMinimumHeight(120);
    grid_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    grid_->horizontalHeader()->setDefaultSectionSize(160);
    grid_->setContextMenuPolicy(Qt::CustomContextMenu);
    content->addWidget(grid_, 1);
    auto* paging = new QHBoxLayout;
    previous_ = new QPushButton(UiLanguage::tr("Previous page"));
    next_ = new QPushButton(UiLanguage::tr("Next page"));
    limit_ = new QSpinBox;
    limit_->setRange(25, 2000);
    limit_->setValue(200);
    apply_ = new QPushButton(UiLanguage::tr("Apply changes"));
    apply_->setObjectName("databaseApply");
    discard_ = new QPushButton(UiLanguage::tr("Discard changes"));
    paging->addWidget(previous_);
    paging->addWidget(next_);
    paging->addWidget(limit_);
    paging->addStretch();
    paging->addWidget(discard_);
    paging->addWidget(apply_);
    content->addLayout(paging);
    auto* editor = new CodeEditor;
    editor->setGitAnnotationsAvailable(false);
    editor->setLanguage(ts::LanguageId::Sql);
    editor->setMinimumHeight(100);
    editor->setPlaceholderText(UiLanguage::tr("Enter a SQL statement, or select a table on the left."));
    editor->setObjectName("databaseSql");
    sql_ = editor;
    editor->setCompletionRequest([this, editor] {
        QStringList names;
        for (int i = 0; i < tables_->count(); ++i)
            names << tables_->item(i)->text();
        for (int i = 0; i < model_->columnCount(); ++i)
            names << model_->headerData(i, Qt::Horizontal, Qt::DisplayRole).toString();
        editor->setCompletionWords(names);
    });
    content->addWidget(sql_);
    auto* queryBar = new QHBoxLayout;
    run_ = new QPushButton(UiLanguage::tr("Run statement"));
    run_->setObjectName("databaseRun");
    stop_ = new QPushButton(UiLanguage::tr("Stop request"));
    status_ = new QLabel(UiLanguage::tr("Choose a database driver and connection."));
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    queryBar->addWidget(run_);
    queryBar->addWidget(stop_);
    queryBar->addWidget(status_, 1);
    content->addLayout(queryBar);
    split->addWidget(right);
    split->setSizes({180, 800});
    layout->addWidget(split, 1);
    connect(connect_, &QPushButton::clicked, this, &DatabasePanel::configure);
    connect(disconnect_, &QPushButton::clicked, this, [this] {
        if (allowDiscard()) {
            session_->cancel();
            model_->load({});
            tables_->clear();
        }
    });
    connect(refresh_, &QPushButton::clicked, this, [this] {
        if (allowDiscard())
            load(page_.value("offset").toInt());
    });
    connect(exportButton, &QPushButton::clicked, this, &DatabasePanel::exportCsv);
    connect(filter, &QLineEdit::textChanged, proxy_, &QSortFilterProxyModel::setFilterFixedString);
    connect(tables_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        if (!allowDiscard())
            return;
        table_ = item->text();
        sqlMode_ = false;
        load();
    });
    connect(run_, &QPushButton::clicked, this, [this] {
        if (!allowDiscard())
            return;
        auto selected = sql_->textCursor().selectedText().replace(QChar::ParagraphSeparator, '\n');
        statement_ = selected.isEmpty() ? sql_->toPlainText() : selected;
        sqlMode_ = true;
        session_->query(statement_, 0, limit_->value());
    });
    auto* runShortcut = new QShortcut(QKeySequence("Ctrl+Return"), sql_);
    runShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(runShortcut, &QShortcut::activated, run_, &QPushButton::click);
    connect(stop_, &QPushButton::clicked, session_, &DatabaseSession::cancel);
    connect(apply_, &QPushButton::clicked, this,
            [this] { session_->apply(page_.value("page").toInt(), model_->changes()); });
    connect(discard_, &QPushButton::clicked, this, [this] {
        model_->discard();
        updateControls();
    });
    connect(previous_, &QPushButton::clicked, this, [this] {
        if (allowDiscard())
            load(qMax(0, page_.value("offset").toInt() - limit_->value()));
    });
    connect(next_, &QPushButton::clicked, this, [this] {
        if (allowDiscard())
            load(page_.value("offset").toInt() + model_->rowCount());
    });
    connect(model_, &QAbstractItemModel::dataChanged, this, [this] {
        updateControls();
        status_->setText(UiLanguage::tr("Staged cell changes: %1").arg(model_->changes().size()));
    });
    connect(session_, &DatabaseSession::busyChanged, this, [this](bool busy) {
        grid_->setEnabled(!busy);
        tables_->setEnabled(!busy);
        updateControls();
        if (busy) {
            lastError_.clear();
            status_->setText(UiLanguage::tr("Running database request…"));
        }
    });
    connect(session_, &DatabaseSession::connectedChanged, this, [this](bool connected) {
        if (!connected && lastError_.isEmpty())
            status_->setText(UiLanguage::tr("Disconnected. Reconnect to continue."));
        updateControls();
    });
    connect(session_, &DatabaseSession::failed, this, [this](const QString& message) {
        if (session_->operation() != "apply") {
            model_->load({});
            page_ = {};
        }
        lastError_ = UiLanguage::text(message);
        status_->setText(lastError_);
        updateControls();
    });
    connect(session_, &DatabaseSession::resultReady, this, [this](const QJsonObject& result) {
        const auto kind = result.value("kind").toString();
        if (kind == "connected") {
            tables_->clear();
            model_->load({});
            page_ = {};
            for (const auto& table : result.value("tables").toArray())
                tables_->addItem(table.toString());
            status_->setText(UiLanguage::tr("Connected. Open a table or run a SQL statement."));
        } else if (kind == "page") {
            page_ = result;
            model_->load(result);
            status_->setText(
                UiLanguage::tr("Rows %1–%2 · %3 pending changes")
                    .arg(model_->rowCount() ? result.value("offset").toInt() + 1 : 0)
                    .arg(model_->rowCount() ? result.value("offset").toInt() + model_->rowCount() : 0)
                    .arg(0));
            if (!result.value("editable").toBool())
                status_->setText(
                    status_->text() + " · " +
                    UiLanguage::tr("Read-only grid: editing requires a table key and transactions."));
            if (result.value("resultLimited").toBool())
                status_->setText(
                    status_->text() + " · " +
                    UiLanguage::tr("Result capped at 5000 rows or 8 MiB. Narrow the SQL query for more."));
        } else {
            model_->load({});
            page_ = {};
            status_->setText(UiLanguage::tr("Affected rows: %1").arg(result.value("affected").toDouble()));
        }
        updateControls();
    });
    connect(grid_, &QTableView::customContextMenuRequested, this, [this](const QPoint& point) {
        const auto index = grid_->indexAt(point);
        QMenu menu;
        auto* null = menu.addAction(UiLanguage::tr("Set NULL"));
        null->setEnabled(proxy_->flags(index) & Qt::ItemIsEditable);
        connect(null, &QAction::triggered, this,
                [this, index] { model_->setNull(proxy_->mapToSource(index)); });
        auto* copy = menu.addAction(UiLanguage::tr("Copy cell"));
        copy->setEnabled(index.isValid());
        connect(copy, &QAction::triggered, this,
                [index] { QApplication::clipboard()->setText(index.data().toString()); });
        menu.exec(grid_->viewport()->mapToGlobal(point));
    });
    updateControls();
}

bool DatabasePanel::allowDiscard() {
    if (!model_->dirty())
        return true;
    if (QMessageBox::question(this, UiLanguage::tr("Pending database changes"),
                              UiLanguage::tr("Discard the staged cell changes?"),
                              QMessageBox::Discard | QMessageBox::Cancel,
                              QMessageBox::Cancel) != QMessageBox::Discard)
        return false;
    model_->discard();
    return true;
}

bool DatabasePanel::requestClose() {
    if (session_->busy()) {
        QMessageBox::information(this, UiLanguage::tr("Database"),
                                 UiLanguage::tr("Stop the active database request before closing."));
        return false;
    }
    return allowDiscard();
}

void DatabasePanel::updateControls() {
    const bool idle = !session_->busy(), connected = session_->connected();
    connect_->setEnabled(idle && !connected && !session_->running());
    grid_->setEnabled(idle && connected);
    refresh_->setEnabled(idle && connected && !sqlMode_ && page_.value("kind") == "page");
    disconnect_->setEnabled(idle && connected);
    run_->setEnabled(idle && connected);
    stop_->setEnabled(!idle);
    apply_->setEnabled(idle && connected && model_->dirty());
    discard_->setEnabled(idle && model_->dirty());
    previous_->setEnabled(idle && connected && page_.value("offset").toInt() > 0);
    next_->setEnabled(idle && connected && page_.value("more").toBool());
    limit_->setEnabled(idle);
}

void DatabasePanel::load(int offset) {
    if (!session_->connected() || session_->busy())
        return;
    if (sqlMode_)
        session_->queryPage(offset, limit_->value());
    else if (!table_.isEmpty())
        session_->browse(table_, offset, limit_->value());
}

void DatabasePanel::configure() {
    if (!allowDiscard())
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(UiLanguage::tr("Database connection"));
    auto* form = new QFormLayout(&dialog);
    dialog.setObjectName("databaseConnectionDialog");
    auto* alias = new QLineEdit(UiLanguage::tr("Connection"));
    form->addRow(UiLanguage::tr("Connection name"), alias);
    auto* driver = new QComboBox;
    driver->setObjectName("databaseDriver");
    auto* plugins = new QLineEdit(QSettings().value("database/pluginPath").toString());
    auto refresh = [driver] {
        const auto previous = driver->currentText();
        driver->clear();
        driver->addItems(QSqlDatabase::drivers());
        if (driver->findText(previous) >= 0)
            driver->setCurrentText(previous);
    };
    if (!plugins->text().isEmpty())
        QCoreApplication::addLibraryPath(plugins->text());
    refresh();
    form->addRow(UiLanguage::tr("Driver"), driver);
    auto* database = new QLineEdit;
    database->setObjectName("databaseName");
    auto* choose = new QPushButton(UiLanguage::tr("Browse…"));
    auto* dbrow = new QHBoxLayout;
    dbrow->addWidget(database, 1);
    dbrow->addWidget(choose);
    form->addRow(UiLanguage::tr("Database / connection name"), dbrow);
    auto* host = new QLineEdit;
    form->addRow(UiLanguage::tr("Host"), host);
    auto* port = new QSpinBox;
    port->setRange(0, 65535);
    port->setSpecialValueText(UiLanguage::tr("Driver default"));
    form->addRow(UiLanguage::tr("Port"), port);
    auto* user = new QLineEdit;
    form->addRow(UiLanguage::tr("User"), user);
    auto* password = new QLineEdit;
    password->setEchoMode(QLineEdit::Password);
    form->addRow(UiLanguage::tr("Password"), password);
    auto* options = new QLineEdit;
    form->addRow(UiLanguage::tr("Driver options"), options);
    auto* timeout = new QSpinBox;
    timeout->setRange(1, 600);
    timeout->setValue(30);
    form->addRow(UiLanguage::tr("Request timeout, seconds"), timeout);
    auto* pluginBrowse = new QPushButton(UiLanguage::tr("Load driver plugins…"));
    form->addRow(UiLanguage::tr("Plugin directory"), plugins);
    form->addRow(pluginBrowse);
    auto* note = new QLabel(
        UiLanguage::tr("Drivers are discovered from Qt plugins. Passwords are not saved to disk. Grid edits "
                       "require transactional tables. Use a read-only account to browse production data."));
    note->setWordWrap(true);
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    connect(choose, &QPushButton::clicked, &dialog, [=, this] {
        const auto path = QFileDialog::getOpenFileName(this, UiLanguage::tr("Open database"));
        if (!path.isEmpty())
            database->setText(path);
    });
    connect(pluginBrowse, &QPushButton::clicked, &dialog, [=, this] {
        const auto path =
            QFileDialog::getExistingDirectory(this, UiLanguage::tr("Directory containing sqldrivers"));
        if (!path.isEmpty()) {
            plugins->setText(path);
            QCoreApplication::addLibraryPath(path);
            refresh();
        }
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(580, 450);
    if (dialog.exec() != QDialog::Accepted || driver->currentText().isEmpty())
        return;
    QSettings().setValue("database/pluginPath", plugins->text());
    emit titleChanged(alias->text().trimmed().isEmpty() ? driver->currentText() : alias->text().trimmed());
    session_->connectTo({driver->currentText(), database->text(), host->text(), user->text(),
                         password->text(), options->text(), plugins->text(),
                         port->value() == 0 ? -1 : port->value(), timeout->value()});
    password->clear();
}

void DatabasePanel::exportCsv() {
    if (model_->dirty() || model_->rowCount() == 0) {
        status_->setText(UiLanguage::tr("Apply or discard pending changes before exporting a page."));
        return;
    }
    const auto path =
        QFileDialog::getSaveFileName(this, UiLanguage::tr("Export loaded rows"), "rows.csv", "CSV (*.csv)");
    if (path.isEmpty())
        return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        status_->setText(file.errorString());
        return;
    }
    QTextStream stream(&file);
    auto quote = [](QString value) { return '"' + value.replace('"', "\"\"") + '"'; };
    QStringList headers;
    for (int c = 0; c < proxy_->columnCount(); ++c)
        headers << quote(proxy_->headerData(c, Qt::Horizontal).toString());
    stream << headers.join(',') << "\r\n";
    for (int r = 0; r < proxy_->rowCount(); ++r) {
        QStringList row;
        for (int c = 0; c < proxy_->columnCount(); ++c)
            row << quote(proxy_->index(r, c).data(Qt::DisplayRole).toString());
        stream << row.join(',') << "\r\n";
    }
    stream.flush();
    if (!file.commit())
        status_->setText(file.errorString());
    else
        status_->setText(UiLanguage::tr("Loaded rows exported."));
}
