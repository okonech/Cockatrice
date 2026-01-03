#include "dlg_ai_coach.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

DlgAiCoach::DlgAiCoach(QWidget *parent) : QDialog(parent), textEdit(new QPlainTextEdit(this))
{
    setWindowTitle(tr("AI Coach"));
    setModal(false);
    resize(720, 520);

    textEdit->setReadOnly(true);
    textEdit->setLineWrapMode(QPlainTextEdit::WidgetWidth);

    auto *buttons = new QDialogButtonBox(this);
    copyButton = buttons->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    auto *closeButton = buttons->addButton(QDialogButtonBox::Close);

    connect(copyButton, &QPushButton::clicked, this, &DlgAiCoach::copyToClipboard);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(textEdit);
    layout->addWidget(buttons);

    setLayout(layout);

    setStatusText(tr("Preparing request…"));
}

void DlgAiCoach::setStatusText(const QString &text)
{
    textEdit->setPlainText(text);
}

void DlgAiCoach::setResultText(const QString &text)
{
    textEdit->setPlainText(text);
}

void DlgAiCoach::copyToClipboard()
{
    QApplication::clipboard()->setText(textEdit->toPlainText());
}
