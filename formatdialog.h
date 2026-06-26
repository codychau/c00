#ifndef FORMATDIALOG_H
#define FORMATDIALOG_H

#include <QDialog>
#include <QLabel>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QPushButton>
#include <QProgressBar>
#include <QTextEdit>
#include <QProcess>

class FormatDialog : public QDialog
{
    Q_OBJECT

public:
    explicit FormatDialog(const QString &devPath,
                          const QString &devName,
                          const QString &currentFs,
                          const QString &mountPoint,
                          bool isMounted,
                          QWidget *parent = nullptr);
    ~FormatDialog() override;

    bool isRunning() const { return m_running; }

signals:
    void formatFinished(bool success);
    void formatProgress(int percent, const QString &status);
    void enteredBackground();

private slots:
    void showEvent(QShowEvent *event) override;
    void onStartFormat();
    void onBackgroundRun();
    void onCancel();

private:
    struct StepContext {
        QString targetDir;    // 备份目标路径
        QString newFsType;    // 目标文件系统
        QStringList mountOpts;
        QString devUuid;      // 旧 UUID (fstab)
    };

    // UI 元素
    QLabel          *m_infoLabel;
    QComboBox       *m_targetCombo;       // 备份目标分区
    QLabel          *m_targetSpaceLabel;
    QComboBox       *m_fsCombo;           // 文件系统选择
    QCheckBox       *m_noatimeCheck;      // noatime
    QCheckBox       *m_nodiratimeCheck;   // nodiratime
    QCheckBox       *m_discardCheck;      // discard/TRIM (SSD)
    QCheckBox       *m_lazyCheck;         // 强制卸载
    QSpinBox        *m_inodeSpin;         // inode size
    QPushButton     *m_startBtn;
    QPushButton     *m_bgBtn;             // 后台运行
    QPushButton     *m_cancelBtn;
    QProgressBar    *m_progress;
    QTextEdit       *m_log;

    // 设备信息
    QString          m_devPath;
    QString          m_devName;
    QString          m_currentFs;
    QString          m_mountPoint;
    bool             m_isMounted;
    bool             m_running = false;
    bool             m_backgroundMode = false;

    // 进程输出缓存，用于实时读取 stdout 的同时不丢失最终输出
    QString          m_procStdOut;

    // 流程控制
    enum Step {
        StepBackup,
        StepUnmount,
        StepFormat,
        StepMount,
        StepRestore,
        StepUpdateFstab,
        StepDone
    };
    Step             m_currentStep = StepBackup;
    StepContext      m_ctx;
    QProcess        *m_proc = nullptr;

    void appendLog(const QString &msg, const QString &color = "");
    void setProgressValue(int value);
    void setProgressText(const QString &text);
    void enableButtons(bool enabled);

    void startStep();
    void onProcessOutput();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);

    void finishWithError(const QString &reason);
    void doBackup();
    void doUnmount();
    void doFormat();
    void doUpdateFstab();
    void doMount();
    void doRestore();
    void doCleanup();

    // 工具函数
    void runCmd(const QString &cmd, const QStringList &args,
                bool usePkexec = true);
    void runCmdStep(const QString &title, const QString &cmd,
                    const QStringList &args, bool usePkexec = true);
};

#endif // FORMATDIALOG_H
