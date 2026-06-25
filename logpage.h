#ifndef LOGPAGE_H
#define LOGPAGE_H

#include <QWidget>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QComboBox>
#include <QTabWidget>
#include <QProcess>
#include <QTimer>

class LogPage : public QWidget
{
    Q_OBJECT

public:
    explicit LogPage(QWidget *parent = nullptr);

private slots:
    void refreshAppLog();
    void refreshSystemLog();
    void clearAppLog();

private:
    void appendProcessOutput(QProcess *proc, QPlainTextEdit *edit);

    QTabWidget   *m_tabs;
    QPlainTextEdit *m_appLogEdit;
    QPlainTextEdit *m_sysLogEdit;
    QPushButton    *m_clearBtn;
    QPushButton    *m_refreshBtn;
    QLabel         *m_status;
    QComboBox      *m_sysFilterCombo;
    QTimer         *m_autoRefresh;
};

#endif // LOGPAGE_H
