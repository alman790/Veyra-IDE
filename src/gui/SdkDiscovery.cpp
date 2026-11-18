#include "MainWindow.hpp"
#include "SdkInstaller.hpp"
#include "ToolDiscovery.hpp"
#include "UiLanguage.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QSaveFile>
#include <QTableWidget>
#include <QVBoxLayout>

void MainWindow::showSdkDiscovery() {
    QDialog dialog(this);
    dialog.setObjectName("sdkDiscoveryDialog");
    dialog.setWindowTitle(UiLanguage::tr("Installed SDKs"));
    dialog.resize(900, 500);
    auto* layout = new QVBoxLayout(&dialog);
    auto* explanation = new QLabel(
        UiLanguage::tr("Detected tools are listed below. Nothing is installed or connected until you choose "
                       "Connect. The selected tools will run locally with this project's permissions."));
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* table = new QTableWidget(0, 4);
    table->setObjectName("detectedSdks");
    table->setHorizontalHeaderLabels({UiLanguage::tr("Language"), UiLanguage::tr("Compiler / runtime"),
                                      UiLanguage::tr("Language server"), UiLanguage::tr("SDK directory")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(table, 1);
    auto detected = ToolDiscovery::detect();
    const auto populate = [&] {
        table->setRowCount(0);
        for (const auto& item : detected) {
            const int row = table->rowCount();
            table->insertRow(row);
            const QStringList values{
                QString::fromStdString(item.language.name),
                item.compiler.isEmpty()
                    ? item.runtime
                    : item.compiler + (item.runtime.isEmpty() || item.runtime == item.compiler
                                           ? QString()
                                           : "\n" + item.runtime),
                item.server.isEmpty() ? UiLanguage::tr("Not found; configure a local LSP") : item.server,
                item.root};
            for (int column = 0; column < values.size(); ++column) {
                auto* value = new QTableWidgetItem(values[column]);
                value->setToolTip(values[column]);
                table->setItem(row, column, value);
                table->setRowHeight(row, values[1].contains('\n') ? 58 : 36);
            }
        }
    };
    populate();
    auto* status = new QLabel;
    status->setWordWrap(true);
    layout->addWidget(status);
    if (detected.empty())
        status->setText(UiLanguage::tr("No SDK was found on PATH. Add an SDK directory in Settings."));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto* connectSdk =
        buttons->addButton(UiLanguage::tr("Connect selected SDK"), QDialogButtonBox::AcceptRole);
    connectSdk->setObjectName("connectDetectedSdk");
    connectSdk->setEnabled(!detected.empty());
    auto* installTools =
        buttons->addButton(UiLanguage::tr("Install missing language tools…"), QDialogButtonBox::ActionRole);
    installTools->setObjectName("installDetectedLanguageTools");
    connect(installTools, &QPushButton::clicked, &dialog, [&] {
        const int row = table->currentRow();
        if (row < 0 || row >= detected.size())
            return;
        const auto recipe =
            QJsonDocument::fromJson(QByteArray::fromStdString(detected[row].language.installation)).object();
        if (recipe.value("steps").toArray().isEmpty()) {
            status->setText(UiLanguage::tr("This adapter has no automatic installation recipe. Select an "
                                           "installed language server in Settings."));
            return;
        }
        SdkInstaller installer(detected[row], workspaceRoot_, &dialog);
        if (installer.exec() == QDialog::Accepted) {
            syncLanguage(true);
            dialog.accept();
        }
    });
    auto* directory =
        buttons->addButton(UiLanguage::tr("Choose SDK directory…"), QDialogButtonBox::ActionRole);
    connect(directory, &QPushButton::clicked, &dialog, [&] {
        const auto root = QFileDialog::getExistingDirectory(&dialog, UiLanguage::tr("Choose SDK directory"));
        if (root.isEmpty())
            return;
        const auto additional = ToolDiscovery::detect(root);
        if (additional.isEmpty()) {
            status->setText(
                UiLanguage::tr("No declared tool was found in this SDK. Supply a veyra-languages.json "
                               "adapter or configure it in Settings."));
            return;
        }
        detected.append(additional);
        populate();
        table->selectRow(detected.size() - 1);
        connectSdk->setEnabled(true);
        status->clear();
    });
    layout->addWidget(buttons);
    if (!detected.empty())
        table->selectRow(0);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(connectSdk, &QPushButton::clicked, &dialog, [&] {
        const int row = table->currentRow();
        if (row < 0 || row >= detected.size())
            return;
        const auto& chosen = detected[row];
        QString error;
        if (!ToolDiscovery::adopt(chosen, workspaceRoot_, error)) {
            status->setText(UiLanguage::text(error));
            return;
        }
        syncLanguage(true);
        updateProjectTree();
        dialog.accept();
    });
    dialog.exec();
}
