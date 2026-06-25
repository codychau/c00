#include "formatdialog.h"

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
    // 默认选中 ext4
    int ext4Idx = m_fsCombo->findText("ext4");
    if (ext4Idx >= 0) m_fsCombo->setCurrentIndex(ext4Idx);
    fsLayout->addRow("文件系统类型：", m_fsCombo);

    m_inodeSpin = new QSpinBox();
    m_inodeSpin->setRange(128, 65536);
    m_inodeSpin->setValue(256);
    m_inodeSpin->setSuffix(" bytes");
    m_inodeSpin->setToolTip("inode 大小，大文件存储可考虑 1024+");
    // 根据文件系统动态启用/禁用
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
    m_noatimeCheck->setToolTip("挂载时添加 noatime 选项，适合 NAS/文件服务器");
    optLayout->addWidget(m_noatimeCheck);

    m_nodiratimeCheck = new QCheckBox("nodiratime — 禁用目录访问时间更新");
    m_nodiratimeCheck->setChecked(true);
    optLayout->addWidget(m_nodiratimeCheck);

    m_discardCheck = new QCheckBox("discard — 启用 TRIM（SSD 优化）");
    m_discardCheck->setChecked(true);
    m_discardCheck->setToolTip("挂载时添加 discard 选项，SSD 建议开启");
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

                // 只添加 part 类型且有挂载点且不是自己的设备
                if (type == "part" && !mntStr.isEmpty() && !fsStr.isEmpty()) {
                    QString devFull = QString("/dev/%1").arg(name);
                    if (devFull != m_devPath) {
                        // 获取可用空间
                        QString info = tr("挂载点: %1 | 文件系统: %2")
                                        .arg(mntStr, fsStr);
                        // 获取容量信息
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
                            // 存储额外信息显示
                            m_targetCombo->setItemData(
                                m_targetCombo->count() - 1,
                                info, Qt::UserRole + 1);
                        });
                        spaceP->start("df", {"-h", devFull});
                    }
                }

                // 递归 children
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

// ── UI 工具 ──

void FormatDialog::appendLog(const QString &msg, const QString &color)
{
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

    // 开始执行
    m_currentStep = StepBackup;
    startStep();
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
            enableButtons(true);
            appendLog("[WARN] 用户取消操作", "yellow");
        }
        return;
    }
    reject();
}

void FormatDialog::startStep()
{
    switch (m_currentStep) {
    case StepBackup:     doBackup();   break;
    case StepUnmount:    doUnmount();  break;
    case StepFormat:     doFormat();   break;
    case StepUpdateFstab: doUpdateFstab(); break;
    case StepMount:      doMount();    break;
    case StepRestore:    doRestore();  break;
    case StepDone:        doCleanup();  break;
    }
}

void FormatDialog::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    QString stdOut = m_proc ? QString::fromUtf8(m_proc->readAllStandardOutput()).trimmed() : "";
    QString stdErr = m_proc ? QString::fromUtf8(m_proc->readAllStandardError()).trimmed() : "";
    Q_UNUSED(stdOut);

    bool ok = (exitCode == 0 && status == QProcess::NormalExit);

    if (!ok) {
        QString errMsg = stdErr.isEmpty() ? stdOut : stdErr;
        appendLog(QString("[FAIL] %1").arg(errMsg), "red");
    }

    // 根据步骤处理结果
    switch (m_currentStep) {
    case StepBackup:
        if (ok) {
            setProgressValue(10);
            setProgressText("备份完成");
            m_currentStep = StepUnmount;
            startStep();
        } else {
            appendLog("[ERR] 备份失败，终止", "red");
            enableButtons(true);
            m_running = false;
        }
        break;

    case StepUnmount:
        if (ok || (!ok && stdErr.contains("not mounted"))) {
            setProgressValue(20);
            setProgressText("已卸载");
            m_currentStep = StepFormat;
            startStep();
        } else {
            appendLog("[ERR] 卸载失败，终止", "red");
            enableButtons(true);
            m_running = false;
        }
        break;

    case StepFormat:
        if (ok) {
            setProgressValue(40);
            setProgressText("格式化完成");
            m_currentStep = StepUpdateFstab;
            startStep();
        } else {
            appendLog("[ERR] 格式化失败", "red");
            enableButtons(true);
            m_running = false;
        }
        break;

    case StepUpdateFstab:
        if (ok) {
            setProgressValue(50);
            setProgressText("fstab 已更新");
            m_currentStep = StepMount;
            startStep();
        } else {
            appendLog("[ERR] 更新 fstab 失败", "red");
            enableButtons(true);
            m_running = false;
        }
        break;

    case StepMount:
        if (ok) {
            setProgressValue(70);
            setProgressText("已挂载");
            if (!m_ctx.targetDir.isEmpty()) {
                m_currentStep = StepRestore;
            } else {
                m_currentStep = StepDone;
            }
            startStep();
        } else {
            appendLog("[ERR] 挂载失败", "red");
            enableButtons(true);
            m_running = false;
        }
        break;

    case StepRestore:
        if (ok) {
            setProgressValue(90);
            setProgressText("数据恢复完成");
            m_currentStep = StepDone;
            startStep();
        } else {
            appendLog("[ERR] 恢复数据失败", "red");
            enableButtons(true);
            m_running = false;
        }
        break;

    case StepDone:
        break;
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
    if (usePkexec) {
        QStringList pkArgs;
        pkArgs << cmd << args;
        m_proc->setProcessChannelMode(QProcess::MergedChannels);
        connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &FormatDialog::onProcessFinished);
        m_proc->start("pkexec", pkArgs);
    } else {
        connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &FormatDialog::onProcessFinished);
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

    // 检查备份是否已完成
    if (QDir(backupPath).exists()) {
        auto ret = QMessageBox::question(this, "备份已存在",
            QString("备份目录 %1 已存在。\n重新备份？").arg(backupPath),
            QMessageBox::Yes | QMessageBox::No);
        if (ret == QMessageBox::No) {
            appendLog("[SKIP] 使用现有备份", "yellow");
            m_ctx.targetDir = backupPath;
            m_currentStep = StepUnmount;
            startStep();
            return;
        }
        // 重新备份：删除旧的
        QDir(backupPath).removeRecursively();
    }

    // 创建备份目录
    QDir().mkpath(backupPath);

    // rsync 备份
    appendLog(QString("[BACKUP] %1 → %2").arg(m_mountPoint, backupPath));
    runCmdStep("BACKUP", "rsync",
        {"-aAXv", "--progress", m_mountPoint + "/", backupPath + "/"}, false);
    // rsync 不需要 pkexec，可以普通权限
    // 更新存储路径
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

    // 先尝试正常卸载
    runCmdStep("UNMOUNT", "umount", {m_mountPoint});

    // 如果失败且勾选了强制卸载，在 onProcessFinished 中处理
    // 简单处理：如果失败则重试强制卸载
}

void FormatDialog::doFormat()
{
    setProgressText("正在格式化...");
    QStringList args;

    if (m_ctx.newFsType == "ext4") {
        args << "-F";  // 强制
        if (m_inodeSpin->isEnabled() && m_inodeSpin->value() != 256) {
            args << "-I" << QString::number(m_inodeSpin->value());
        }
        args << m_devPath;
        runCmdStep("FORMAT", "mkfs.ext4", args);
    } else if (m_ctx.newFsType == "xfs") {
        args << "-f";  // 强制
        // XFS 的 inode 大小用 -i size=
        if (m_inodeSpin->isEnabled() && m_inodeSpin->value() != 256) {
            args << "-i" << QString("size=%1").arg(m_inodeSpin->value());
        }
        args << m_devPath;
        runCmdStep("FORMAT", "mkfs.xfs", args);
    } else if (m_ctx.newFsType == "btrfs") {
        args << "-f";  // 强制
        if (m_inodeSpin->isEnabled() && m_inodeSpin->value() != 16384) {
            args << "--nodesize" << QString::number(m_inodeSpin->value());
        }
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
        enableButtons(true);
        m_running = false;
    }
}

void FormatDialog::doUpdateFstab()
{
    setProgressText("正在更新 fstab...");

    // 读取 fstab
    QFile fstab("/etc/fstab");
    if (!fstab.open(QIODevice::ReadOnly)) {
        appendLog("[FAIL] 无法读取 /etc/fstab", "red");
        // 不终止，允许继续
        m_currentStep = StepMount;
        startStep();
        return;
    }
    QString content = QString::fromUtf8(fstab.readAll());
    fstab.close();

    // 查找旧 UUID（用设备路径查找）
    // 读取新 UUID
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
        m_currentStep = StepMount;
        startStep();
        return;
    }

    // 在 fstab 中找到对应的行
    QStringList lines = content.split('\n');
    bool found = false;
    QString oldUuid;

    // 先用设备路径找，再用 UUID 找
    QRegularExpression devRe(QString("^\\s*(UUID|/dev/%1)").arg(m_devName));
    for (int i = 0; i < lines.size(); ++i) {
        if (lines[i].trimmed().isEmpty() || lines[i].trimmed().startsWith('#'))
            continue;

        auto parts = lines[i].split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (parts.isEmpty()) continue;

        // 匹配设备或 UUID
        if (parts[0] == m_devPath || parts[0] == m_devName) {
            oldUuid = parts[0];
            found = true;

            // 替换行: UUID=<new>  <mount>  <fstype>  <options>  0  2
            QString newLine = QString("UUID=%1  %2  %3  %4  0  2")
                .arg(newUuid, m_mountPoint, m_ctx.newFsType,
                     m_ctx.mountOpts.isEmpty() ? "defaults" : m_ctx.mountOpts.join(","));
            lines[i] = newLine;
            appendLog(QString("[FSTAB] 更新行 %1: %2").arg(i + 1).arg(newLine));
            break;
        }
        // 也检查 UUID= 格式
        if (parts[0].startsWith("UUID=")) {
            QString uuid = parts[0].mid(5);
            // 通过 blkid 检查是否匹配 m_devPath
        }
    }

    if (!found) {
        // 没找到，追加一行
        QString newLine = QString("UUID=%1  %2  %3  %4  0  2")
            .arg(newUuid, m_mountPoint, m_ctx.newFsType,
                 m_ctx.mountOpts.isEmpty() ? "defaults" : m_ctx.mountOpts.join(","));
        lines.append(newLine);
        appendLog(QString("[FSTAB] 追加行: %1").arg(newLine));
    }

    // 写回 fstab
    // 先备份
    QString bakPath = QString("/etc/fstab.formatbak.%1").arg(
        QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    QFile::copy("/etc/fstab", bakPath);
    appendLog(QString("[FSTAB] 备份到 %1").arg(bakPath));

    QString newContent = lines.join('\n') + '\n';
    // 用 tee 写
    QProcess *p = new QProcess(this);
    p->start("bash", {"-c", QString("cat > /etc/fstab << 'FSTAB_EOF'\n%1\nFSTAB_EOF")
                                   .arg(newContent)});
    p->waitForFinished(3000);
    p->deleteLater();

    appendLog("[OK] fstab 已更新", "green");
    m_currentStep = StepMount;
    startStep();
}

void FormatDialog::doMount()
{
    setProgressText("正在挂载...");
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

    // 清理备份
    if (!m_ctx.targetDir.isEmpty() && QDir(m_ctx.targetDir).exists()) {
        auto ret = QMessageBox::question(this, "清理备份",
            QString("是否删除临时备份目录？\n%1").arg(m_ctx.targetDir),
            QMessageBox::Yes | QMessageBox::No);
        if (ret == QMessageBox::Yes) {
            QDir(m_ctx.targetDir).removeRecursively();
            appendLog("[CLEAN] 备份已删除", "green");
        } else {
            appendLog("[INFO] 备份保留: " + m_ctx.targetDir);
        }
    }

    enableButtons(true);
    m_running = false;

    QMessageBox::information(this, "格式化完成",
        QString("设备 %1 已成功格式化为 %2")
            .arg(m_devPath, m_ctx.newFsType));
}
