#ifndef SHUTDOWNDIALOG_H
#define SHUTDOWNDIALOG_H

#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>

class ShutdownDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ShutdownDialog(const QString &action, QWidget *parent = nullptr);
    QString getReason() const { return m_reasonEdit->text(); }
    bool isContinue() const { return m_continue; }

private slots:
    void onContinueClicked();
    void onCancelClicked();

private:
    bool m_continue = false;
    QString m_action;
    QLabel *m_label;
    QLineEdit *m_reasonEdit;
    QPushButton *m_continueBtn;
    QPushButton *m_cancelBtn;
};

#endif // SHUTDOWNDIALOG_H
