#include "dlg_ai_coach.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTextCursor>
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

void DlgAiCoach::appendResultDelta(const QString &delta)
{
    if (delta.isEmpty()) {
        return;
    }

    QScrollBar *sb = textEdit->verticalScrollBar();
    const int oldValue = sb ? sb->value() : 0;
    const int oldMax = sb ? sb->maximum() : 0;

    // Only auto-scroll while streaming if the user is already at (or very near) the bottom.
    const bool wasAtBottom = sb ? ((oldMax - oldValue) <= 2) : true;

    QTextCursor cursor(textEdit->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(delta);

    if (sb) {
        if (wasAtBottom) {
            sb->setValue(sb->maximum());
        } else {
            sb->setValue(oldValue);
        }
    }
}

void DlgAiCoach::copyToClipboard()
{
    QApplication::clipboard()->setText(textEdit->toPlainText());
}
