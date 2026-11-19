#pragma once

#include "ProjectAgent.hpp"
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTextBrowser;

class AgentPanel final : public QWidget {
    Q_OBJECT
    ProjectAgent* agent_;
    QComboBox* provider_;
    QLineEdit* model_;
    QLabel* status_;
    QLabel* context_;
    QLabel* privacy_;
    QTextBrowser* discussion_;
    QPlainTextEdit* question_;
    QPlainTextEdit* preview_;
    QPushButton* send_;
    QPushButton* propose_;
    QPushButton* stop_;
    QPushButton* apply_;
    QPushButton* refresh_;
    QPushButton* configure_;
    QPushButton* clear_;
    AgentProposal proposal_;
    bool loadedProviders_ = false;
    void configure();
    void append(const QString& title, const QString& text);

  public:
    explicit AgentPanel(QWidget* parent = nullptr);

    ProjectAgent* agent() const {
        return agent_;
    }

    const AgentProposal& proposal() const {
        return proposal_;
    }

    void invalidateProposal();
    void applied();
    void reportError(const QString& error);

  signals:
    void requestSubmitted(const QString& question, const QString& provider, const QString& model,
                          bool proposeChanges);
    void applyRequested();
    void buildRequested();

  protected:
    void showEvent(QShowEvent* event) override;
};
