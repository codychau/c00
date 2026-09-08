#include "shutdowndialog.h"
#include "logger.h"

#include <QMessageBox>
#include <QProcess>
#include <QDateTime>

ShutdownDialog::ShutdownDialog(const QString &action, QWidget *parent)
    : QDialog(parent)
    , m_action(action)
{
    setWindowTitle(QString("确认%1").arg(action));
    resize(400, 200);
    setMinimumWidth(350);

    auto *mainLayout = new QVBoxLayout(this);

    m_label = new QLabel(QString("您选择了%1，请输入%1原因（可选）：").arg(action));
    m_label->setWordWrap(true);
    mainLayout->addWidget(m_label);

    m_reasonEdit = new QLineEdit();
    m_reasonEdit->setPlaceholderText(QString("请输入%1原因，留空则不记录具体原因...").arg(action));
    mainLayout->addWidget(m_reasonEdit);

    auto *btnLayout = new QHBoxLayout();

    m_continueBtn = new QPushButton("继续");
    m_continueBtn->setStyleSheet(
        "QPushButton { background: #16a34a; color: white; padding: 8px 24px; "
        "border-radius: 4px; font-weight: bold; }"
        "QPushButton:hover { background: #15803d; }");
    connect(m_continueBtn, &QPushButton::clicked, this, &ShutdownDialog::onContinueClicked);
    btnLayout->addWidget(m_continueBtn);

    m_cancelBtn = new QPushButton("取消");
    m_cancelBtn->setStyleSheet(
        "QPushButton { background: #6b7280; color: white; padding: 8px 24px; "
        "border-radius: 4px; }"
        "QPushButton:hover { background: #4b5563; }");
    connect(m_cancelBtn, &QPushButton::clicked, this, &ShutdownDialog::onCancelClicked);
    btnLayout->addWidget(m_cancelBtn);

    btnLayout->addStretch();
    mainLayout->addLayout(btnLayout);
}

void ShutdownDialog::onContinueClicked()
{
    QString tag = (m_action == "关机" || m_action == "关机") ? "SHUTDOWN" : "RESTART";
    
    if (m_reasonEdit->text().trimmed().isEmpty()) {
        Logger::log(tag, "执行关机/重启操作（未填写原因）");
    } else {
        Logger::log(tag, QString("执行关机/重启操作，原因：%1").arg(m_reasonEdit->text().trimmed()));
    }

    m_continue = true;
    accept();
}

void ShutdownDialog::onCancelClicked()
{
    m_continue = false;
    reject();
}
