#include "SdkInstaller.hpp"
#include "UiLanguage.hpp"
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

SdkInstaller::SdkInstaller(const ToolDiscovery::DetectedSdk& chosen, const QString& project, QWidget* parent)
    : QDialog(parent), chosen_(chosen), root_(project) {
    setObjectName("sdkInstaller");
    tree_.configure(process_);
    connect(&process_, &QProcess::started, this, [this] {
        QString error;
        if (!tree_.attach(process_.processId(), error))
            state_->setText(UiLanguage::text(error));
    });
    setWindowTitle(UiLanguage::tr("Install language tools"));
    resize(780, 480);
    recipe_ = QJsonDocument::fromJson(QByteArray::fromStdString(chosen.language.installation)).object();
    const auto key =
        QString::fromLatin1(QCryptographicHash::hash(QByteArray::fromStdString(chosen.language.lspId +
                                                                               chosen.language.installation),
                                                     QCryptographicHash::Sha256)
                                .toHex()
                                .left(20));
    managed_ =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/language-tools/" + key;
    steps_ = recipe_.value("steps").toArray();
    server_ = expand(recipe_.value("server").toString());
    auto* layout = new QVBoxLayout(this);
    auto* explanation = new QLabel(UiLanguage::tr(
        "Review these local installation commands. Downloads begin only when you choose Install and connect. "
        "Tools are installed in the IDE's private directory; the existing SDK is not modified."));
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* location = new QLabel(managed_);
    location->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(location);
    output_ = new QPlainTextEdit;
    output_->setReadOnly(true);
    output_->setObjectName("sdkInstallOutput");
    for (const auto& value : steps_) {
        const auto step = value.toObject();
        QStringList command{expand(step.value("program").toString())};
        for (const auto& argument : step.value("arguments").toArray())
            command << expand(argument.toString());
        output_->appendPlainText(QString::fromUtf8(
            QJsonDocument(QJsonArray::fromStringList(command)).toJson(QJsonDocument::Compact)));
    }
    layout->addWidget(output_, 1);
    state_ = new QLabel(UiLanguage::tr("Waiting for your confirmation"));
    layout->addWidget(state_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    install_ = buttons->addButton(UiLanguage::tr("Install and connect"), QDialogButtonBox::ActionRole);
    install_->setObjectName("installAndConnect");
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &SdkInstaller::reject);
    connect(install_, &QPushButton::clicked, this, [this] {
        if (steps_.isEmpty())
            return;
        install_->setEnabled(false);
        QDir().mkpath(managed_);
        step_ = 0;
        auto environment = ToolDiscovery::environment();
        auto paths = ToolDiscovery::sdkPaths({{}, chosen_.root, {}});
        paths.append(ToolDiscovery::paths());
        environment.insert("PATH", paths.join(QDir::listSeparator()));
        if (!chosen_.language.sdkVariable.empty())
            environment.insert(QString::fromStdString(chosen_.language.sdkVariable), chosen_.root);
        process_.setProcessEnvironment(environment);
        process_.setWorkingDirectory(managed_);
        next();
    });
    const auto output = [this] {
        output_->appendPlainText(
            QString::fromUtf8(process_.readAllStandardOutput() + process_.readAllStandardError()));
    };
    connect(&process_, &QProcess::readyReadStandardOutput, this, output);
    connect(&process_, &QProcess::readyReadStandardError, this, output);
    connect(&process_, &QProcess::errorOccurred, this, [this] {
        state_->setText(process_.errorString());
        install_->setEnabled(true);
    });
    connect(&process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
                tree_.reset();
                if (code != 0 || status != QProcess::NormalExit) {
                    state_->setText(UiLanguage::tr("Installation failed. See the tool output above."));
                    install_->setEnabled(true);
                    return;
                }
                ++step_;
                next();
            });
}

SdkInstaller::~SdkInstaller() {
    process_.disconnect(this);
    tree_.terminate();
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(1000);
    }
}

QString SdkInstaller::expand(QString value) const {
    return value.replace("{managed}", managed_)
        .replace("{sdk}", chosen_.root)
        .replace("{compiler}", chosen_.compiler);
}

void SdkInstaller::next() {
    if (step_ >= steps_.size()) {
        if (!QFileInfo::exists(server_)) {
            state_->setText(UiLanguage::tr("The installation did not produce a language server."));
            return;
        }
        const auto compiler = expand(recipe_.value("tools").toObject().value("build").toString());
        if (!compiler.isEmpty() && QFileInfo::exists(compiler))
            chosen_.compiler = compiler;
        chosen_.server = server_;
        QStringList arguments;
        for (const auto& value : recipe_.value("serverArguments").toArray())
            arguments << value.toString();
        chosen_.language.suggestedServer = "server " + arguments.join(' ').toStdString();
        QString error;
        if (!ToolDiscovery::adopt(chosen_, root_, error)) {
            state_->setText(UiLanguage::text(error));
            return;
        }
        accept();
        return;
    }
    const auto step = steps_[step_].toObject();
    auto program = expand(step.value("program").toString());
    if (!QFileInfo(program).isAbsolute())
        program = QStandardPaths::findExecutable(
            program, process_.processEnvironment().value("PATH").split(QDir::listSeparator()));
    if (program.isEmpty()) {
        state_->setText(UiLanguage::tr("The package manager required by this adapter was not found."));
        install_->setEnabled(true);
        return;
    }
    QStringList arguments;
    for (const auto& value : step.value("arguments").toArray())
        arguments << expand(value.toString());
    state_->setText(UiLanguage::tr("Installing language tools…"));
    process_.start(program, arguments);
}

void SdkInstaller::reject() {
    process_.disconnect(this);
    tree_.terminate();
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(1000);
    }
    QDialog::reject();
}
