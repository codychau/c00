#ifndef AIPAGE_H
#define AIPAGE_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QTimer>

class AIPage : public QWidget
{
    Q_OBJECT

public:
    explicit AIPage(QWidget *parent = nullptr);

private slots:
    void refresh();
    void checkService(const QString &name, const QString &systemdSvc,
                      const QString &procName, int port,
                      QLabel *statusLbl, QLabel *detailLbl);

private:
    QLabel *m_ollamaStatus;
    QLabel *m_ollamaDetail;
    QLabel *m_llamacppStatus;
    QLabel *m_llamacppDetail;
    QLabel *m_vllmStatus;
    QLabel *m_vllmDetail;
    QLabel *m_info;
    QPushButton *m_refreshBtn;
    QTimer *m_timer;
};

#endif // AIPAGE_H
