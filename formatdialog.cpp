#include "formatdialog.h"
#include "logger.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QFormLayout>
#include <QFile>
#include <QDir>
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>

// ── 可用文件系统选项 ──
static const QStringList FS_OPTIONS = {
    "ext4", "xfs", "btrfs", "f2fs", "ntfs-3g"
};

FormatDialog::FormatDialog(const QString &devPath,
                           const QString &devName,
                           const QString &currentFs,
                           const QString &mountPoint,
                           bool isMounted,
                           QWidget *parent)
    : QDialog(parent)
    , m_devPath(devPath)
    , m_devName(devName)
    , m_currentFs(currentFs)
    , m_mountPoint(mountPoint)
    , m_isMounted(isMounted)
{
    setWindowTitle(QString("格式化 - %1").arg(devPath));
    resize(600, 560);
    setMinimumWidth(500);

    auto *mainLayout = new QVBoxLayout(this);

    // ── 设备信息 ──
    m_infoLabel = new QLabel();
    m_infoLabel->setWordWrap(true);
    m_infoLabel->setStyleSheet("background: #1e293b; color: #e2e8f0; "
                                "padding: 12px; border-radius: 6px; "
                                "font-family: monospace; font-size: 13px;");
    {
        QString statusStr = m_isMounted
            ? QString("已挂载于 %1").arg(m_mountPoint)
            : "未挂载";
        m_infoLabel->setText(QString(
            "设备: %1\n"
            "当前文件系统: %2\n"
            "状态: %3")
            .arg(devPath, currentFs, statusStr));
    }
    mainLayout->addWidget(m_infoLabel);

    // ── 备份/迁移选项 ──
    auto *backupGroup = new QGroupBox("📦 数据迁移");
    auto *backupLayout = new QVBoxLayout(backupGroup);

    m_targetCombo = new QComboBox();
    m_targetCombo->addItem("（不迁移，直接格式化）", "");
    m_targetCombo->setToolTip("选择数据迁移的目标设备/分区");
    connect(m_targetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        QString info = m_targetCombo->itemData(idx, Qt::UserRole + 1).toString();
        m_targetSpaceLabel->setText(info);
    });
    backupLayout->addWidget(new QLabel("目标设备（备份源数据到此分区）："));
    backupLayout->addWidget(m_targetCombo);

    m_targetSpaceLabel = new QLabel("选择目标查看可用空间");
    m_targetSpaceLabel->setStyleSheet("color: #666; font-size: 11px;");
    backupLayout->addWidget(m_targetSpaceLabel);

    m_lazyCheck = new QCheckBox("如果设备正忙，强制卸载 (umount -l)");
    m_lazyCheck->setChecked(false);
    backupLayout->addWidget(m_lazyCheck);

    mainLayout->addWidget(backupGroup);

    // ── 文件系统选择 ──
    auto *fsGroup = new QGroupBox("💿 文件系统设置");
    auto *fsLayout = new QFormLayout(fsGroup);

    m_fsCombo = new QComboBox();
    for (const auto &fs : FS_OPTIONS)
        m_fsCombo->addItem(fs);
    int ext4Idx = m_fsCombo->findText("ext4");
    if (ext4Idx >= 0) m_fsCombo->setCurrentIndex(ext4Idx);
    fsLayout->addRow("文件系统类型：", m_fsCombo);

    m_inodeSpin = new QSpinBox();
    m_inodeSpin->setRange(128, 65536);
    m_inodeSpin->setValue(256);
    m_inodeSpin->setSuffix(" bytes");
    m_inodeSpin->setToolTip("inode 大小，大文件存储可考虑 1024+");
    connect(m_fsCombo, &QComboBox::currentTextChanged, this, [this](const QString &fs) {
        m_inodeSpin->setEnabled(fs == "ext4" || fs == "xfs" || fs == "btrfs");
        m_discardCheck->setEnabled(fs == "ext4" || fs == "xfs" || fs == "btrfs");
    });
    fsLayout->addRow("Inode 大小：", m_inodeSpin);

    mainLayout->addWidget(fsGroup);

    // ── 优化选项 ──
    auto *optGroup = new QGroupBox("⚡ 优化选项");
    auto *optLayout = new QVBoxLayout(optGroup);

    m_noatimeCheck = new QCheckBox("noatime — 禁用访问时间更新，减少写入");
    m_noatimeCheck->setChecked(true);
    optLayout->addWidget(m_noatimeCheck);

    m_nodiratimeCheck = new QCheckBox("nodiratime — 禁用目录访问时间更新");
    m_nodiratimeCheck->setChecked(true);
    optLayout->addWidget(m_nodiratimeCheck);

    m_discardCheck = new QCheckBox("discard — 启用 TRIM（SSD 优化）");
    m_discardCheck->setChecked(true);
    optLayout->addWidget(m_discardCheck);

    mainLayout->addWidget(optGroup);

    // ── 进度与日志 ──
    m_progress = new QProgressBar();
    m_progress->setVisible(false);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(true);
    mainLayout->addWidget(m_progress);

    m_log = new QTextEdit();
    m_log->setReadOnly(true);
    m_log->setMaximumHeight(150);
    m_log->setStyleSheet("font-family: monospace; font-size: 11px; "
                          "background: #0f172a; color: #e2e8f0;");
    m_log->setPlaceholderText("操作日志...");
    m_log->setVisible(false);
    mainLayout->addWidget(m_log);

    // ── 底栏按钮 ──
    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch();

    m_bgBtn = new QPushButton("▶ 后台运行");
    m_bgBtn->setToolTip("隐藏此窗口，在后台继续执行格式化流程");
    m_bgBtn->setVisible(false);
    connect(m_bgBtn, &QPushButton::clicked, this, &FormatDialog::onBackgroundRun);
    btnRow->addWidget(m_bgBtn);

    m_cancelBtn = new QPushButton("取消");
    connect(m_cancelBtn, &QPushButton::clicked, this, &FormatDialog::onCancel);
    btnRow->addWidget(m_cancelBtn);

    m_startBtn = new QPushButton("开始格式化");
    m_startBtn->setStyleSheet(
        "QPushButton { background: #dc2626; color: white; padding: 8px 24px; "
        "border-radius: 4px; font-weight: bold; }"
        "QPushButton:hover { background: #b91c1c; }"
        "QPushButton:disabled { background: #6b7280; }");
    connect(m_startBtn, &QPushButton::clicked, this, &FormatDialog::onStartFormat);
    btnRow->addWidget(m_startBtn);

    mainLayout->addLayout(btnRow);

    // ── 加载目标分区列表 ──
    appendLog("[INFO] 正在扫描可用分区...", "cyan");
    auto *p = new QProcess(this);
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, p](int, QProcess::ExitStatus) {
        QString out = QString::fromUtf8(p->readAllStandardOutput());
        p->deleteLater();

        QJsonParseError err;
        auto doc = QJsonDocument::fromJson(out.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError) {
            appendLog("[WARN] 无法解析分区信息", "yellow");
            return;
        }

        // 递归遍历 lsblk 输出
        std::function<void(const QJsonArray &)> addParts;
        addParts = [this, &addParts](const QJsonArray &arr) {
            for (const auto &val : arr) {
                auto obj = val.toObject();
                QString name   = obj.value("name").toString();
                QString type   = obj.value("type").toString();
                auto fstype    = obj.value("fstype");
                auto mount     = obj.value("mountpoint");

                QString fsStr  = fstype.isNull() ? "" : fstype.toString();
                QString mntStr = mount.isNull()  ? "" : mount.toString();

                if (type == "part" && !mntStr.isEmpty() && !fsStr.isEmpty()) {
                    QString devFull = QString("/dev/%1").arg(name);
                    if (devFull != m_devPath) {
                        QString info = tr("挂载点: %1 | 文件系统: %2")
                                        .arg(mntStr, fsStr);
                        auto *spaceP = new QProcess(this);
                        connect(spaceP, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                                this, [this, spaceP, devFull, info, mntStr](int, QProcess::ExitStatus) mutable {
                            QString dfOut = QString::fromUtf8(spaceP->readAllStandardOutput()).trimmed();
                            spaceP->deleteLater();

                            auto dfLines = dfOut.split('\n');
                            if (dfLines.size() >= 2) {
                                auto cols = dfLines[1].split(QRegularExpression("\\s+"));
                                if (cols.size() >= 4) {
                                    QString avail = cols[3];
                                    info.append(QString(" | 可用: %1").arg(avail));
                                }
                            }
                            m_targetCombo->addItem(
                                QString("%1 (%2)").arg(devFull, info),
                                QVariant::fromValue(mntStr));
                            m_targetCombo->setItemData(
                                m_targetCombo->count() - 1,
                                info, Qt::UserRole + 1);
                        });
                        spaceP->start("df", {"-h", devFull});
                    }
                }

                auto children = obj.value("children").toArray();
                if (!children.isEmpty())
                    addParts(children);
            }
        };

        auto devices = doc.object().value("blockdevices").toArray();
        addParts(devices);

        appendLog("[OK] 分区扫描完成", "green");
    });
    p->start("lsblk", {"-J", "-o", "NAME,TYPE,FSTYPE,MOUNTPOINT"});
}

FormatDialog::~FormatDialog()
{
    if (m_proc && m_proc->state() != QProcess::NotRunning) {
        m_proc->kill();
        m_proc->waitForFinished(3000);
    }
}

// ── UI 工具 ──

void FormatDialog::appendLog(const QString &msg, const QString &color)
{
    // 写入 Logger（日志管理系统可查看）
    Logger::log("FORMAT", msg);

    // 更新 UI
    m_log->setVisible(true);
    QString colored = color.isEmpty()
        ? msg
        : QString("<span style='color:%1;'>%2</span>").arg(color, msg.toHtmlEscaped());
    m_log->append(colored);
}

void FormatDialog::setProgressValue(int value)
{
    m_progress->setValue(value);
}

void FormatDialog::setProgressText(const QString &text)
{
    m_progress->setFormat(text);
}

void FormatDialog::enableButtons(bool enabled)
{
    m_startBtn->setEnabled(enabled);
    m_bgBtn->setVisible(!enabled && m_running);
    m_cancelBtn->setEnabled(enabled);
}

// ── 操作控制 ──

void FormatDialog::onStartFormat()
{
    if (m_running) return;
    m_running = true;
    enableButtons(false);

    // 收集配置
    m_ctx.newFsType = m_fsCombo->currentText();
    m_ctx.targetDir = m_targetCombo->currentData().toString();
    m_ctx.mountOpts.clear();

    if (m_noatimeCheck->isChecked())  m_ctx.mountOpts << "noatime";
    if (m_nodiratimeCheck->isChecked()) m_ctx.mountOpts << "nodiratime";
    if (m_discardCheck->isChecked())  m_ctx.mountOpts << "discard";
    if (m_inodeSpin->isEnabled()) {
        m_ctx.mountOpts << QString("inode_size=%1").arg(m_inodeSpin->value());
    }

    // 最终确认
    QStringList summary;
    summary << "格式化 " + m_devPath + " 为 " + m_ctx.newFsType;
    if (!m_ctx.targetDir.isEmpty())
        summary << "备份到 " + m_ctx.targetDir;
    if (!m_ctx.mountOpts.isEmpty())
        summary << "挂载选项: " + m_ctx.mountOpts.join(", ");

    auto ret = QMessageBox::question(this, "确认格式化",
        QString("即将执行以下操作：\n  · %1\n\n⚠️ 此操作不可逆！\n确定继续？")
            .arg(summary.join("\n  · ")),
        QMessageBox::Yes | QMessageBox::No);
    if (ret != QMessageBox::Yes) {
        m_running = false;
        enableButtons(true);
        return;
    }

    m_log->clear();
    appendLog("=== 开始格式化流程 ===", "cyan");
    m_progress->setVisible(true);

    m_currentStep = StepBackup;
    startStep();
}

void FormatDialog::onBackgroundRun()
{
    m_backgroundMode = true;
    appendLog("[INFO] 转入后台运行，窗口已隐藏", "cyan");
    Logger::log("FORMAT", "格式化任务转入后台运行");
    hide();
}

void FormatDialog::onCancel()
{
    if (m_running && m_proc) {
        auto ret = QMessageBox::question(this, "确认取消",
            "正在执行格式化流程，确定取消？\n（当前操作将被终止）",
            QMessageBox::Yes | QMessageBox::No);
        if (ret == QMessageBox::Yes) {
            m_proc->kill();
            m_running = false;
            m_backgroundMode = false;
            enableButtons(true);
            appendLog("[WARN] 用户取消操作", "yellow");
            Logger::log("FORMAT", "格式化任务被用户取消");
        }
        return;
    }
    reject();
}

void FormatDialog::startStep()
{
    switch (m_currentStep) {
    case StepBackup:      doBackup();     break;
    case StepUnmount:     doUnmount();    break;
    case StepFormat:      doFormat();     break;
    case StepMount:       doMount();      break;
    case StepRestore:     doRestore();    break;
    case StepUpdateFstab: doUpdateFstab(); break;
    case StepDone:        doCleanup();    break;
    }
}

void FormatDialog::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    QString stdOut = m_proc ? QString::fromUtf8(m_proc->readAllStandardOutput()).trimmed() : "";
    QString errMsg = m_proc ? QString::fromUtf8(m_proc->readAllStandardError()).trimmed() : "";
    Q_UNUSED(stdOut);

    bool ok = (exitCode == 0 && status == QProcess::NormalExit);

    if (!ok) {
        appendLog(QString("[FAIL] %1").arg(errMsg.isEmpty() ? stdOut : errMsg), "red");
    }

    switch (m_currentStep) {
    case StepBackup:
        if (ok) {
            setProgressValue(10);
            setProgressText("备份完成");
            m_currentStep = StepUnmount;
            startStep();
        } else {
            appendLog("[ERR] 备份失败，终止", "red");
            finishWithError("备份失败");
        }
        break;

    case StepUnmount:
        if (ok || errMsg.contains("not mounted", Qt::CaseInsensitive)) {
            setProgressValue(20);
            setProgressText("已卸载");
            m_currentStep = StepFormat;
            startStep();
        } else {
            // 如果勾选了强制卸载，尝试 umount -l
            if (m_lazyCheck->isChecked()) {
                appendLog("[RETRY] 尝试强制卸载...", "yellow");
                runCmdStep("UNMOUNT-L", "umount", {"-l", m_mountPoint});
                // 这个回调会在该命令完成后再次被调用
            } else {
                appendLog("[ERR] 卸载失败，终止", "red");
                finishWithError("卸载失败（设备正忙）");
            }
        }
        break;

    case StepFormat:
        if (ok) {
            setProgressValue(40);
            setProgressText("格式化完成");
            m_currentStep = StepMount;
            startStep();
        } else {
            appendLog("[ERR] 格式化失败", "red");
            finishWithError("格式化失败");
        }
        break;

    case StepMount:
        if (ok) {
            setProgressValue(60);
            setProgressText("已挂载");
            if (!m_ctx.targetDir.isEmpty()) {
                m_currentStep = StepRestore;
            } else {
                m_currentStep = StepUpdateFstab;
            }
            startStep();
        } else {
            appendLog("[ERR] 挂载失败，系统仍可使用旧 fstab 启动", "red");
            finishWithError("挂载失败");
        }
        break;

    case StepRestore:
        if (ok) {
            setProgressValue(80);
            setProgressText("数据恢复完成");
            m_currentStep = StepUpdateFstab;
            startStep();
        } else {
            appendLog("[ERR] 恢复数据失败，fstab 未被修改，系统仍可正常启动", "red");
            finishWithError("数据恢复失败");
        }
        break;

    case StepUpdateFstab:
        if (ok) {
            setProgressValue(90);
            setProgressText("fstab 已更新");
            m_currentStep = StepDone;
            startStep();
        } else {
            appendLog("[ERR] 更新 fstab 失败，系统仍可使用旧 fstab 启动", "red");
            finishWithError("更新 fstab 失败");
        }
        break;

    case StepDone:
        break;
    }
}

void FormatDialog::finishWithError(const QString &reason)
{
    appendLog(QString("[ERR] %1").arg(reason), "red");
    Logger::log("FORMAT", QString("格式化任务失败: %1").arg(reason));
    enableButtons(true);
    m_running = false;

    if (m_backgroundMode) {
        emit formatFinished(false);
    }
}

// ── 各步骤实现 ──

void FormatDialog::runCmd(const QString &cmd, const QStringList &args,
                          bool usePkexec)
{
    if (m_proc) {
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &FormatDialog::onProcessFinished);

    if (usePkexec) {
        QStringList pkArgs;
        pkArgs << cmd << args;
        m_proc->start("pkexec", pkArgs);
    } else {
        m_proc->start(cmd, args);
    }
}

void FormatDialog::runCmdStep(const QString &title, const QString &cmd,
                              const QStringList &args, bool usePkexec)
{
    appendLog(QString("[%1] %2 %3").arg(title, cmd, args.join(" ")));
    runCmd(cmd, args, usePkexec);
}

void FormatDialog::doBackup()
{
    if (m_ctx.targetDir.isEmpty()) {
        appendLog("[SKIP] 不迁移数据，跳过备份", "yellow");
        m_currentStep = StepUnmount;
        startStep();
        return;
    }

    setProgressText("正在备份数据...");
    QString backupPath = m_ctx.targetDir + "/format_backup_" + m_devName;

    if (QDir(backupPath).exists()) {
        appendLog("[SKIP] 使用现有备份", "yellow");
        m_ctx.targetDir = backupPath;
        m_currentStep = StepUnmount;
        startStep();
        return;
    }

    QDir().mkpath(backupPath);

    appendLog(QString("[BACKUP] %1 → %2").arg(m_mountPoint, backupPath));
    runCmdStep("BACKUP", "rsync",
        {"-aAXv", "--progress", m_mountPoint + "/", backupPath + "/"}, false);
    m_ctx.targetDir = backupPath;
}

void FormatDialog::doUnmount()
{
    if (!m_isMounted) {
        appendLog("[SKIP] 未挂载，跳过卸载", "yellow");
        m_currentStep = StepFormat;
        startStep();
        return;
    }

    setProgressText("正在卸载...");
    runCmdStep("UNMOUNT", "umount", {m_mountPoint});
}

void FormatDialog::doFormat()
{
    setProgressText("正在格式化...");
    QStringList args;

    if (m_ctx.newFsType == "ext4") {
        args << "-F";
        if (m_inodeSpin->isEnabled() && m_inodeSpin->value() != 256)
            args << "-I" << QString::number(m_inodeSpin->value());
        args << m_devPath;
        runCmdStep("FORMAT", "mkfs.ext4", args);
    } else if (m_ctx.newFsType == "xfs") {
        args << "-f";
        if (m_inodeSpin->isEnabled() && m_inodeSpin->value() != 256)
            args << "-i" << QString("size=%1").arg(m_inodeSpin->value());
        args << m_devPath;
        runCmdStep("FORMAT", "mkfs.xfs", args);
    } else if (m_ctx.newFsType == "btrfs") {
        args << "-f";
        if (m_inodeSpin->isEnabled() && m_inodeSpin->value() != 16384)
            args << "--nodesize" << QString::number(m_inodeSpin->value());
        args << m_devPath;
        runCmdStep("FORMAT", "mkfs.btrfs", args);
    } else if (m_ctx.newFsType == "f2fs") {
        args << "-f" << m_devPath;
        runCmdStep("FORMAT", "mkfs.f2fs", args);
    } else if (m_ctx.newFsType == "ntfs-3g") {
        args << "-f" << "-Q" << m_devPath;
        runCmdStep("FORMAT", "mkfs.ntfs", args);
    } else {
        appendLog(QString("[ERR] 不支持的文件系统: %1").arg(m_ctx.newFsType), "red");
        finishWithError("不支持的文件系统");
    }
}

void FormatDialog::doUpdateFstab()
{
    setProgressText("正在更新 fstab...");

    QFile fstab("/etc/fstab");
    if (!fstab.open(QIODevice::ReadOnly)) {
        appendLog("[FAIL] 无法读取 /etc/fstab", "red");
        Logger::log("FORMAT", "无法读取 /etc/fstab，跳过更新");
        m_currentStep = StepDone;
        startStep();
        return;
    }
    QString content = QString::fromUtf8(fstab.readAll());
    fstab.close();

    QString newUuid;
    {
        auto *p = new QProcess(this);
        p->start("blkid", {"-s", "UUID", "-o", "value", m_devPath});
        p->waitForFinished(5000);
        newUuid = QString::fromUtf8(p->readAllStandardOutput()).trimmed();
        p->deleteLater();
    }

    if (newUuid.isEmpty()) {
        appendLog("[WARN] 无法获取新 UUID，跳过 fstab 更新", "yellow");
        m_currentStep = StepDone;
        startStep();
        return;
    }

    QStringList lines = content.split('\n');
    bool found = false;

    for (int i = 0; i < lines.size(); ++i) {
        if (lines[i].trimmed().isEmpty() || lines[i].trimmed().startsWith('#'))
            continue;

        auto parts = lines[i].split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (parts.isEmpty()) continue;

        if (parts[0] == m_devPath || parts[0] == m_devName ||
            parts[0] == QString("UUID=%1").arg(m_ctx.devUuid) ||
            parts[0] == QString("UUID=%1").arg(newUuid)) {
            found = true;

            QString newLine = QString("UUID=%1  %2  %3  %4  0  2")
                .arg(newUuid, m_mountPoint, m_ctx.newFsType,
                     m_ctx.mountOpts.isEmpty() ? "defaults" : m_ctx.mountOpts.join(","));
            lines[i] = newLine;
            appendLog(QString("[FSTAB] 更新行 %1: %2").arg(i + 1).arg(newLine));
            break;
        }
    }

    if (!found) {
        QString newLine = QString("UUID=%1  %2  %3  %4  0  2")
            .arg(newUuid, m_mountPoint, m_ctx.newFsType,
                 m_ctx.mountOpts.isEmpty() ? "defaults" : m_ctx.mountOpts.join(","));
        lines.append(newLine);
        appendLog(QString("[FSTAB] 追加行: %1").arg(newLine));
    }

    // 生成新的 fstab 内容（不含换行符，runCmdStep 会 appendLog 命令文本）
    QString newContent = lines.join('\n') + '\n';

    // 先备份旧 fstab
    QString bakPath = QString("/etc/fstab.formatbak.%1").arg(
        QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    QFile::copy("/etc/fstab", bakPath);
    appendLog(QString("[FSTAB] 备份到 %1").arg(bakPath));

    // 用 pkexec 写入新 fstab，错误会走 onProcessFinished
    runCmdStep("FSTAB", "bash", {"-c", QString("cat > /etc/fstab << 'FSTAB_EOF'\n%1\nFSTAB_EOF")
                                        .arg(newContent)});
}

void FormatDialog::doMount()
{
    setProgressText("正在挂载...");

    // 确保挂载点存在
    if (!QDir(m_mountPoint).exists()) {
        QDir().mkpath(m_mountPoint);
        appendLog(QString("[INFO] 创建挂载点: %1").arg(m_mountPoint));
    }

    runCmdStep("MOUNT", "mount", {m_mountPoint});
}

void FormatDialog::doRestore()
{
    if (m_ctx.targetDir.isEmpty() || !QDir(m_ctx.targetDir).exists()) {
        appendLog("[SKIP] 无备份数据，跳过恢复", "yellow");
        m_currentStep = StepDone;
        startStep();
        return;
    }

    setProgressText("正在恢复数据...");
    appendLog(QString("[RESTORE] %1 → %2").arg(m_ctx.targetDir, m_mountPoint));
    runCmdStep("RESTORE", "rsync",
        {"-aAXv", "--progress", m_ctx.targetDir + "/", m_mountPoint + "/"}, false);
}

void FormatDialog::doCleanup()
{
    setProgressValue(100);
    setProgressText("全部完成！");
    appendLog("=== 格式化完成 ===", "green");
    Logger::log("FORMAT", QString("格式化任务完成: %1 → %2").arg(m_devPath, m_ctx.newFsType));

    // 清理备份（仅在非后台模式或仍可见时询问）
    if (!m_backgroundMode && !m_ctx.targetDir.isEmpty() && QDir(m_ctx.targetDir).exists()) {
        auto ret = QMessageBox::question(this, "清理备份",
            QString("是否删除临时备份目录？\n%1").arg(m_ctx.targetDir),
            QMessageBox::Yes | QMessageBox::No);
        if (ret == QMessageBox::Yes) {
            QDir(m_ctx.targetDir).removeRecursively();
            appendLog("[CLEAN] 备份已删除", "green");
        } else {
            appendLog("[INFO] 备份保留: " + m_ctx.targetDir);
        }
    } else if (m_backgroundMode && !m_ctx.targetDir.isEmpty()) {
        appendLog("[INFO] 备份保留: " + m_ctx.targetDir);
    }

    m_running = false;
    enableButtons(true);

    if (m_backgroundMode) {
        // 后台模式：发信号，不弹窗
        emit formatFinished(true);
        // 自动关闭对话框
        accept();
    } else {
        QMessageBox::information(this, "格式化完成",
            QString("设备 %1 已成功格式化为 %2")
                .arg(m_devPath, m_ctx.newFsType));
    }
}
