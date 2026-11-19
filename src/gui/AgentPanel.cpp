#include "AgentPanel.hpp"
#include "UiLanguage.hpp"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QShowEvent>
#include <QSplitter>
#include <QTextBrowser>
#include <QVBoxLayout>

AgentPanel::AgentPanel(QWidget* parent) : QWidget(parent) {
    setObjectName("projectAgentPanel");
    agent_ = new ProjectAgent(this);
    auto* layout = new QVBoxLayout(this);
    auto* providers = new QHBoxLayout;
    provider_ = new QComboBox;
    provider_->setObjectName("agentProvider");
    provider_->addItem(UiLanguage::tr("Bundled Qwen"), QJsonObject{});
    providers->addWidget(provider_, 1);
    refresh_ = new QPushButton(UiLanguage::tr("Refresh providers"));
    configure_ = new QPushButton(UiLanguage::tr("Configure…"));
    providers->addWidget(refresh_);
    providers->addWidget(configure_);
    layout->addLayout(providers);
    model_ = new QLineEdit;
    model_->setObjectName("agentModel");
    model_->setPlaceholderText(UiLanguage::tr("Provider model"));
    model_->setVisible(false);
    layout->addWidget(model_);
    privacy_ = new QLabel(UiLanguage::tr("Local inference. Project context stays on this computer."));
    privacy_->setWordWrap(true);
    privacy_->setTextFormat(Qt::PlainText);
    layout->addWidget(privacy_);
    context_ = new QLabel(
        UiLanguage::tr("Context includes the active buffer, related files, project layout and diagnostics."));
    context_->setObjectName("agentContextSummary");
    context_->setWordWrap(true);
    context_->setTextFormat(Qt::PlainText);
    layout->addWidget(context_);
    auto* split = new QSplitter(Qt::Vertical);
    discussion_ = new QTextBrowser;
    discussion_->setObjectName("agentDiscussion");
    discussion_->setOpenExternalLinks(false);
    discussion_->setOpenLinks(false);
    split->addWidget(discussion_);
    preview_ = new QPlainTextEdit;
    preview_->setObjectName("agentPatchPreview");
    preview_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    preview_->setReadOnly(true);
    preview_->setLineWrapMode(QPlainTextEdit::NoWrap);
    preview_->setPlaceholderText(UiLanguage::tr("Proposed changes appear here before they are applied."));
    split->addWidget(preview_);
    split->setSizes({300, 200});
    layout->addWidget(split, 1);
    auto* changes = new QHBoxLayout;
    apply_ = new QPushButton(UiLanguage::tr("Apply proposal"));
    apply_->setObjectName("agentApplyProposal");
    apply_->setEnabled(false);
    auto* build = new QPushButton(UiLanguage::tr("Build to verify"));
    build->setObjectName("agentVerifyBuild");
    clear_ = new QPushButton(UiLanguage::tr("New discussion"));
    changes->addWidget(apply_);
    changes->addWidget(build);
    changes->addWidget(clear_);
    layout->addLayout(changes);
    question_ = new QPlainTextEdit;
    question_->setObjectName("agentQuestion");
    question_->setPlaceholderText(
        UiLanguage::tr("Ask about this code or describe a fix. Ctrl+Enter sends the request."));
    question_->setMaximumHeight(110);
    layout->addWidget(question_);
    auto* actions = new QHBoxLayout;
    status_ = new QLabel(UiLanguage::tr("Ready"));
    status_->setObjectName("agentStatus");
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    actions->addWidget(status_, 1);
    stop_ = new QPushButton(UiLanguage::tr("Stop"));
    stop_->setEnabled(false);
    send_ = new QPushButton(UiLanguage::tr("Ask"));
    send_->setObjectName("agentSendRequest");
    propose_ = new QPushButton(UiLanguage::tr("Propose fix"));
    propose_->setObjectName("agentProposeFix");
    actions->addWidget(stop_);
    actions->addWidget(send_);
    actions->addWidget(propose_);
    layout->addLayout(actions);
    connect(configure_, &QPushButton::clicked, this, &AgentPanel::configure);
    connect(refresh_, &QPushButton::clicked, agent_->beamcore(), &BeamcoreClient::requestProviders);
    connect(provider_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        const auto info = provider_->currentData().toJsonObject();
        const bool remote = !info.isEmpty();
        model_->setVisible(remote);
        model_->setText(info.value("model").toString());
        privacy_->setText(
            remote ? UiLanguage::tr(
                         "Submitting sends the selected project context to this provider through Beamcore.")
                   : UiLanguage::tr("Local inference. Project context stays on this computer."));
    });
    connect(agent_->beamcore(), &BeamcoreClient::providersReceived, this,
            [this](const QJsonArray& providers) {
                const auto selected = provider_->currentData().toJsonObject().value("name").toString();
                while (provider_->count() > 1)
                    provider_->removeItem(1);
                for (const auto& value : providers) {
                    const auto info = value.toObject();
                    provider_->addItem(info.value("name").toString(), info);
                    if (info.value("name").toString() == selected)
                        provider_->setCurrentIndex(provider_->count() - 1);
                }
                loadedProviders_ = true;
                if (!agent_->busy())
                    status_->setText(UiLanguage::tr("Providers loaded from Beamcore."));
            });
    connect(agent_->beamcore(), &BeamcoreClient::failed, this, [this](const QString& error) {
        if (!agent_->busy())
            status_->setText(UiLanguage::text(error));
    });
    const auto submit = [this](bool proposeChanges) {
        if (agent_->busy() || question_->toPlainText().trimmed().isEmpty())
            return;
        invalidateProposal();
        const auto text = question_->toPlainText().trimmed();
        if (text.size() > 4000) {
            reportError(UiLanguage::tr("Enter a request of up to 4000 characters."));
            return;
        }
        append(UiLanguage::tr("You"), text);
        const auto provider = provider_->currentData().toJsonObject().value("name").toString();
        emit requestSubmitted(text, provider, model_->text().trimmed(), proposeChanges);
        if (agent_->busy())
            question_->clear();
    };
    connect(send_, &QPushButton::clicked, this, [submit] { submit(false); });
    connect(propose_, &QPushButton::clicked, this, [submit] { submit(true); });
    auto* sendShortcut = new QShortcut(QKeySequence("Ctrl+Return"), question_);
    sendShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(sendShortcut, &QShortcut::activated, this, [submit] { submit(false); });
    connect(stop_, &QPushButton::clicked, agent_, &ProjectAgent::cancel);
    connect(apply_, &QPushButton::clicked, this, &AgentPanel::applyRequested);
    connect(build, &QPushButton::clicked, this, &AgentPanel::buildRequested);
    connect(clear_, &QPushButton::clicked, this, [this] {
        agent_->clearHistory();
        discussion_->clear();
        question_->clear();
        invalidateProposal();
    });
    connect(agent_, &ProjectAgent::busyChanged, this, [this](bool busy) {
        send_->setEnabled(!busy);
        propose_->setEnabled(!busy);
        stop_->setEnabled(busy);
        provider_->setEnabled(!busy);
        model_->setEnabled(!busy);
        refresh_->setEnabled(!busy);
        configure_->setEnabled(!busy);
        clear_->setEnabled(!busy);
        apply_->setEnabled(!busy && !proposal_.edits.isEmpty());
    });
    connect(agent_, &ProjectAgent::statusChanged, status_, &QLabel::setText);
    connect(agent_, &ProjectAgent::failed, this, &AgentPanel::reportError);
    connect(agent_, &ProjectAgent::contextReady, this, [this](const QStringList& paths) {
        context_->setText(UiLanguage::tr("Context files: ") + paths.join(", "));
    });
    connect(agent_, &ProjectAgent::answerReady, this, [this](const QString& answer) {
        append(UiLanguage::tr("Assistant"), answer);
        status_->setText(UiLanguage::tr("Response ready."));
    });
    connect(agent_, &ProjectAgent::proposalReady, this,
            [this](const AgentProposal& proposal, const QString& preview) {
                proposal_ = proposal;
                append(UiLanguage::tr("Proposed fix"), proposal.summary);
                preview_->setPlainText(preview);
                apply_->setEnabled(true);
                status_->setText(
                    UiLanguage::tr("Review the changes, then apply them to the editor buffers."));
            });
}

void AgentPanel::append(const QString& title, const QString& text) {
    discussion_->append("<b>" + title.toHtmlEscaped() + "</b><pre style=\"white-space:pre-wrap\">" +
                        text.toHtmlEscaped() + "</pre>");
}

void AgentPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!loadedProviders_ && !agent_->busy() && !BeamcoreClient::executable().isEmpty())
        agent_->beamcore()->requestProviders();
}

void AgentPanel::invalidateProposal() {
    proposal_ = {};
    apply_->setEnabled(false);
    preview_->clear();
}

void AgentPanel::applied() {
    invalidateProposal();
    status_->setText(UiLanguage::tr("Changes applied to editor buffers. Save and build to verify them."));
}

void AgentPanel::reportError(const QString& error) {
    append(UiLanguage::tr("Agent"), UiLanguage::text(error));
    status_->setText(UiLanguage::text(error));
}

void AgentPanel::configure() {
    QDialog dialog(this);
    dialog.setWindowTitle(UiLanguage::tr("Beamcore integration"));
    auto* form = new QFormLayout(&dialog);
    QSettings settings;
    auto* executable = new QLineEdit(settings.value("agent/beamcoreExecutable").toString());
    executable->setPlaceholderText("beamcore");
    auto* browse = new QPushButton(UiLanguage::tr("Browse…"));
    auto* row = new QHBoxLayout;
    row->addWidget(executable, 1);
    row->addWidget(browse);
    form->addRow(UiLanguage::tr("Beamcore executable"), row);
    auto* config = new QLineEdit(settings.value("agent/beamcoreConfig").toString());
    config->setPlaceholderText(QDir::homePath() + "/.beamcore/config.dets");
    form->addRow(UiLanguage::tr("Beamcore provider configuration"), config);
    auto* note = new QLabel(UiLanguage::tr("Configure API providers in Beamcore. Veyra reads an isolated "
                                           "copy of its configuration and does not change the original."));
    note->setWordWrap(true);
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    connect(browse, &QPushButton::clicked, &dialog, [this, executable] {
        const auto file = QFileDialog::getOpenFileName(this, UiLanguage::tr("Beamcore executable"));
        if (!file.isEmpty())
            executable->setText(file);
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(650, 220);
    if (dialog.exec() != QDialog::Accepted)
        return;
    agent_->beamcore()->stop();
    settings.setValue("agent/beamcoreExecutable", executable->text().trimmed());
    settings.setValue("agent/beamcoreConfig", config->text().trimmed());
    loadedProviders_ = false;
    agent_->beamcore()->requestProviders();
}
