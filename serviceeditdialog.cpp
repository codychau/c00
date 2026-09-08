#include "serviceeditdialog.h"
#include "logger.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QMessageBox>
#include <QFontDatabase>

ServiceEditDialog::ServiceEditDialog(const QString &serviceName,
                                     const QString &filePath,
                                     const QString &scope,
                                     QWidget *parent)
    : QDialog(parent)
    , m_serviceName(serviceName)
    , m_filePath(filePath)
    , m_scope(scope)
{
    setWindowTitle(QString("编辑服务：%1").arg(serviceName));
    setMinimumSize(560, 480);
    resize(720, 560);

    auto *layout = new QVBoxLayout(this);

    auto *info = new QLabel(QString("文件：%1　（%2 级）").arg(filePath, scope));
    info->setStyleSheet("color: #666;");
    info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(info);

    m_editor = new QPlainTextEdit();
    m_editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(m_editor, 1);

    m_status = new QLabel("");
    m_status->setStyleSheet("color: #666;");
    layout->addWidget(m_status);

    auto *btnRow = new QHBoxLayout();
    m_saveBtn = new QPushButton("保存");
    m_saveBtn->setStyleSheet(
        "QPushButton { background: #16a34a; color: white; padding: 8px 24px; "
        "border-radius: 4px; font-weight: bold; }"
        "QPushButton:hover { background: #15803d; }"
        "QPushButton:disabled { background: #9ca3af; }");
    connect(m_saveBtn, &QPushButton::clicked, this, &ServiceEditDialog::onSave);
    btnRow->addWidget(m_saveBtn);

    m_cancelBtn = new QPushButton("取消");
    m_cancelBtn->setStyleSheet(
        "QPushButton { background: #6b7280; color: white; padding: 8px 24px; "
        "border-radius: 4px; }"
        "QPushButton:hover { background: #4b5563; }");
    connect(m_cancelBtn, &QPushButton::clicked, this, &ServiceEditDialog::onCancel);
    btnRow->addWidget(m_cancelBtn);

    btnRow->addStretch();
    layout->addLayout(btnRow);

    loadFile();
}

ServiceEditDialog::~ServiceEditDialog()
{
    if (!m_tempPath.isEmpty())
        QFile::remove(m_tempPath);
}

void ServiceEditDialog::loadFile()
{
    QFile f(m_filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        m_status->setText(QString("⚠️  无法读取文件：%1").arg(f.errorString()));
        m_status->setStyleSheet("color: #e74c3c;");
        m_saveBtn->setEnabled(false);
        m_cancelBtn->setText("关闭");
        return;
    }
    m_initialContent = QString::fromUtf8(f.readAll());
    m_editor->setPlainText(m_initialContent);
    Logger::log("SERVICE", QString("打开编辑 %1 (%2)").arg(m_filePath, m_scope));
}

void ServiceEditDialog::onSave()
{
    QString content = m_editor->toPlainText();
    m_saveBtn->setEnabled(false);
    m_cancelBtn->setEnabled(false);

    if (m_scope == "user") {
        writeDirect();
    } else {
        writeViaRoot();
    }
}

void ServiceEditDialog::writeDirect()
{
    QFile f(m_filePath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (f.write(m_editor->toPlainText().toUtf8()) >= 0) {
            f.close();
            reloadUnit(true);
            return;
        }
    }
    // 用户级文件也可能位于需要 root 的位置（如 /etc/systemd/user），回退到提权写入
    writeViaRoot();
}

void ServiceEditDialog::writeViaRoot()
{
    auto *tmp = new QTemporaryFile("/tmp/service-edit-XXXXXX", this);
    tmp->setAutoRemove(false);
    if (!tmp->open()) {
        saveFailed(QString("无法创建临时文件：%1").arg(tmp->errorString()));
        return;
    }
    tmp->write(m_editor->toPlainText().toUtf8());
    tmp->flush();
    m_tempPath = tmp->fileName();
    tmp->close();

    m_status->setText("正在写入（需要管理员授权）…");
    m_status->setStyleSheet("color: #666;");

    m_proc = new QProcess(this);
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) {
                if (code != 0) {
                    saveFailed("写入失败（未获得管理员授权或写入出错）");
                    return;
                }
                reloadUnit(false);
            });
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (m_proc->state() == QProcess::NotRunning)
            saveFailed("无法启动提权写入进程：pkexec");
    });
    m_proc->start("pkexec", {"install", "-m", "0644", m_tempPath, m_filePath});
}

void ServiceEditDialog::reloadUnit(bool asUser)
{
    m_status->setText("正在重新加载 unit 配置…");
    m_status->setStyleSheet("color: #666;");

    m_proc = new QProcess(this);
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) {
                if (code != 0) {
                    saveFailed("配置文件已写入，但 unit 重新加载失败");
                    return;
                }
                if (!m_tempPath.isEmpty()) {
                    QFile::remove(m_tempPath);
                    m_tempPath.clear();
                }
                m_status->setText("✅ 已保存并重新加载");
                m_status->setStyleSheet("color: #16a34a;");
                Logger::log("SERVICE", QString("服务 %1 配置已保存").arg(m_serviceName));
                emit saved(m_serviceName);
                accept();
            });

    QStringList args;
    if (asUser) {
        args << "--user" << "daemon-reload";
        m_proc->start("systemctl", args);
    } else {
        args << "systemctl" << "daemon-reload";
        m_proc->start("pkexec", args);
    }
}

void ServiceEditDialog::saveFailed(const QString &msg)
{
    m_status->setText(QString("⚠️  %1").arg(msg));
    m_status->setStyleSheet("color: #e74c3c;");
    m_saveBtn->setEnabled(true);
    m_cancelBtn->setEnabled(true);
    if (!m_tempPath.isEmpty()) {
        QFile::remove(m_tempPath);
        m_tempPath.clear();
    }
}

void ServiceEditDialog::onCancel()
{
    if (m_editor->toPlainText() != m_initialContent) {
        auto ret = QMessageBox::question(
            this, "放弃修改",
            "您对该服务文件的修改尚未保存，确定放弃吗？");
        if (ret != QMessageBox::Yes)
            return;
    }
    reject();
}