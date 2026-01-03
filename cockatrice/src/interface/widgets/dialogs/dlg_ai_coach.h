#ifndef DLG_AI_COACH_H
#define DLG_AI_COACH_H

#include <QDialog>

class QPlainTextEdit;
class QPushButton;

class DlgAiCoach : public QDialog
{
    Q_OBJECT
public:
    explicit DlgAiCoach(QWidget *parent = nullptr);

    void setStatusText(const QString &text);
    void setResultText(const QString &text);
    void appendResultDelta(const QString &delta);

private slots:
    void copyToClipboard();

private:
    QPlainTextEdit *textEdit;
    QPushButton *copyButton;
};

#endif // DLG_AI_COACH_H
