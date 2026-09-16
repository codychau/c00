#include "statuspage.h"
#include "shutdowndialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QScrollArea>
#include <QShowEvent>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QTemporaryFile>
#include <QLineEdit>
#include <QMessageBox>
#include <QInputDialog>

// ── 列索引 ──
enum DiskCol { ColDev = 0, ColModel, ColTemp, ColSize, ColUsed, ColMount, ColDiskCount };

StatusPage::StatusPage(QWidget *parent)
    : QWidget(parent)
{
    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *container = new QWidget();
    auto *layout = new QVBoxLayout(container);
    layout->setSpacing(10);
    layout->setContentsMargins(16, 16, 16, 16);

    // ── 标题 ──
    auto *title = new QLabel("📊 系统状态仪表");
    QFont f = title->font(); f.setPointSize(14); f.setBold(true);
    title->setFont(f);
    layout->addWidget(title);

    // ── 系统信息网格 ──
    auto *sysGroup = new QGroupBox("系统概览");
    auto *gridOuter = new QVBoxLayout(sysGroup);
    auto *grid = new QGridLayout();
    grid->setVerticalSpacing(6);

    const char *keys[] = {
        "系统", "内核", "运行时间", "CPU 使用率",
        "内存", "交换分区", "根分区", "Home 分区",
        "网络", "CPU 温度", "NVMe 温度", "GPU 温度"
    };
    for (int i = 0; i < 12; ++i) {
        auto *k = new QLabel(QString("%1：").arg(keys[i]));
        k->setStyleSheet("font-weight: bold; color: #555;");
        k->setFixedWidth(100);
        m_labels[i] = new QLabel("—");
        grid->addWidget(k, i / 2, (i % 2) * 2);
        grid->addWidget(m_labels[i], i / 2, (i % 2) * 2 + 1);
    }
    gridOuter->addLayout(grid);
    layout->addWidget(sysGroup);

    // ── 磁盘监控 ──
    auto *diskGroup = new QGroupBox("磁盘状态");
    auto *diskLayout = new QVBoxLayout(diskGroup);

    m_diskTable = new QTableWidget(0, ColDiskCount);
    m_diskTable->setHorizontalHeaderLabels({"设备", "型号", "温度", "容量", "已用", "挂载点"});
    m_diskTable->horizontalHeader()->setStretchLastSection(true);
    m_diskTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_diskTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_diskTable->verticalHeader()->hide();
    m_diskTable->setAlternatingRowColors(true);
    diskLayout->addWidget(m_diskTable);

    m_diskStatus = new QLabel("检测中…");
    m_diskStatus->setStyleSheet("color: #888; font-size: 12px;");
    diskLayout->addWidget(m_diskStatus);

    layout->addWidget(diskGroup);
    layout->addStretch();

    scroll->setWidget(container);
    outerLayout->addWidget(scroll);

    // 温度读取按钮
    auto *tempRow = new QHBoxLayout();
    m_tempBtn = new QPushButton("🌡  读取温度");
    connect(m_tempBtn, &QPushButton::clicked, this, &StatusPage::fetchAllTemps);
    tempRow->addWidget(m_tempBtn);

    m_shutdownBtn = new QPushButton("🔌 关机");
    connect(m_shutdownBtn, &QPushButton::clicked, this, [this]() {
        ShutdownDialog dialog("关机", this);
        if (dialog.exec() == QDialog::Accepted && dialog.isContinue()) {
            auto *proc = new QProcess(this);
            connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                    this, [proc](int, QProcess::ExitStatus) { proc->deleteLater(); });
            proc->start("pkexec", {"shutdown", "-h", "now"});
        }
    });
    tempRow->addWidget(m_shutdownBtn);

    m_restartBtn = new QPushButton("🔄 重启");
    connect(m_restartBtn, &QPushButton::clicked, this, [this]() {
        ShutdownDialog dialog("重启", this);
        if (dialog.exec() == QDialog::Accepted && dialog.isContinue()) {
            auto *proc = new QProcess(this);
            connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                    this, [proc](int, QProcess::ExitStatus) { proc->deleteLater(); });
            proc->start("pkexec", {"reboot"});
        }
    });
    tempRow->addWidget(m_restartBtn);

    auto *exitBtn = new QPushButton("🚪 退出程序");
    connect(exitBtn, &QPushButton::clicked, qApp, []() { qApp->quit(); });
    tempRow->addWidget(exitBtn);

    tempRow->addStretch();
    diskLayout->addLayout(tempRow);

    // ── 定时刷新 ──
    m_timer = new QTimer(this);
    m_timer->setInterval(8000);
    connect(m_timer, &QTimer::timeout, this, &StatusPage::refresh);
}

void StatusPage::runCmd(const QString &cmd, const QStringList &args,
                        std::function<void(const QString &)> cb)
{
    auto *p = new QProcess(this);
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [p, cb](int, QProcess::ExitStatus) {
                cb(QString::fromUtf8(p->readAllStandardOutput()).trimmed());
                p->deleteLater();
            });
    p->start(cmd, args);
}

void StatusPage::setLine(int row, const QString &val, const QString &suffix)
{
    if (row < 0 || row >= 12) return;
    m_labels[row]->setText(val + suffix);
}

void StatusPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (!m_hasShown) {
        m_hasShown = true;
        m_timer->start();
        refresh();
    }
}

// ── 获取单块盘的温度 ──
void StatusPage::fetchDiskTemp(const QString &devName, int tableRow)
{
    QString devPath = QString("/dev/%1").arg(devName);
    const QString cacheKey = devName;

    auto *proc = new QProcess();
    QObject::connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     proc, [proc, devName, devPath, row = tableRow, cacheKey, this](
                               int, QProcess::ExitStatus) {
        proc->deleteLater();

        QString out = QString::fromUtf8(proc->readAllStandardOutput());
        QString tempStr;

        // NVMe: Temperature: XX Celsius
        static QRegularExpression nvmeRe(R"(Temperature:\s+(\d+)\s+Celsius)");
        auto nm = nvmeRe.match(out);
        if (nm.hasMatch()) {
            tempStr = QString("%1°C").arg(nm.captured(1));
        }

        // ATA: Temperature_Celsius + raw value
        //  194 Temperature_Celsius     0x0022   107   100   000    Old_age   Always   -       45
        //  末尾为温度值，且可能带 (Min/Max …) 后缀
        if (tempStr.isEmpty()) {
            static QRegularExpression ataRe(
                R"(Temperature_Celsius\s+\S+\s+\S+\s+\S+\s+\S+\s+\S+\s+\S+\s+\S+\s+(-?\d+))");
            auto am = ataRe.match(out);
            if (am.hasMatch()) {
                tempStr = QString("%1°C").arg(am.captured(1));
            }
        }

        // SCSI/USB: Current Temperature
        if (tempStr.isEmpty()) {
            static QRegularExpression scsiRe(R"(Current Temperature:\s+(\d+))");
            auto sm = scsiRe.match(out);
            if (sm.hasMatch()) {
                tempStr = QString("%1°C").arg(sm.captured(1));
            }
        }

        if (tempStr.isEmpty()) tempStr = "N/A";

        if (auto *item = m_diskTable->item(row, ColTemp)) {
            item->setText(tempStr);
            // 高温警告
            int degreePos = tempStr.indexOf(QString("°"));
            if (degreePos > 0) {
                int t = tempStr.left(degreePos).toInt();
                if (t > 60)
                    item->setForeground(QColor("#ef4444"));
                else if (t > 50)
                    item->setForeground(QColor("#eab308"));
            }
        }
        m_tempCache[cacheKey] = tempStr;
    });
    proc->start("sudo", {"-n", "smartctl", "-A", devPath});
}

bool StatusPage::hasPasswordlessSmartctl() const
{
    QProcess check;
    check.start("sudo", {"-n", "smartctl", "-V"});
    if (!check.waitForFinished(5000)) return false;
    return check.exitStatus() == QProcess::NormalExit && check.exitCode() == 0;
}

bool StatusPage::setupSmartctlSudo(const QString &password)
{
    QString user = qEnvironmentVariable("USER");
    if (user.isEmpty()) user = "root";
    QString content = QString("%1 ALL=(root) NOPASSWD: /usr/bin/smartctl\n").arg(user);

    auto *tmp = new QTemporaryFile("/tmp/smartctl-sudoers-XXXXXX", this);
    tmp->setAutoRemove(true);
    if (!tmp->open()) return false;
    tmp->write(content.toUtf8());
    tmp->flush();
    QString tmpPath = tmp->fileName();
    tmp->close();

    QString cmd = QString("install -o root -g root -m 0440 '%1' /etc/sudoers.d/smartctl")
                      .arg(tmpPath);

    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start("sudo", {"-S", "-k", "-p", "", "sh", "-c", cmd});
    if (!p.waitForStarted(5000)) return false;
    p.write((password + "\n").toUtf8());
    p.closeWriteChannel();
    if (!p.waitForFinished(15000)) return false;
    QString errOut = QString::fromUtf8(p.readAll()).trimmed();
    bool ok = p.exitCode() == 0;

    // 无论成败都清理临时文件
    QFile::remove(tmpPath);
    delete tmp;

    if (!ok) {
        QMessageBox::warning(this, "智能磁盘免密配置失败",
            QString("无法写入 /etc/sudoers.d/smartctl。\n\n%1").arg(errOut));
        return false;
    }
    m_smartSetupDone = true;
    m_diskStatus->setText("✅ 已配置 smartctl 免密读取");
    return true;
}

void StatusPage::fetchAllTemps()
{
    // 首次：若 sudo -n smartctl 不可用，则询问一次管理员密码并配置免密
    if (!m_smartSetupDone && !m_smartSetupAsked) {
        m_smartSetupAsked = true;

        if (hasPasswordlessSmartctl()) {
            m_smartSetupDone = true; // 已配置过，直接读取
        } else {
            bool ok = false;
            QString password = QInputDialog::getText(
                this, "首次授权（一次性）",
                "读取硬盘温度需要调用 smartctl。\n"
                "请输入管理员(root)密码，将自动配置免密权限（仅一次），\n"
                "之后每次打开本软件都能直接显示硬盘温度。",
                QLineEdit::Password, QString(), &ok);
            if (ok) setupSmartctlSudo(password);
            else m_diskStatus->setText("未授权，硬盘温度可能无法读取");
        }
    }

    m_diskStatus->setText("读取温度中…");
    for (int i = 0; i < m_diskTable->rowCount(); ++i) {
        auto *item = m_diskTable->item(i, ColDev);
        if (item) {
            m_diskTable->setItem(i, ColTemp, new QTableWidgetItem("…"));
            fetchDiskTemp(item->text(), i);
        }
    }
}

void StatusPage::refresh()
{
    // ───── 系统信息 ─────

    // OS
    runCmd("sh", {"-c", "grep PRETTY_NAME /etc/os-release | cut -d= -f2- | tr -d '\"'"},
           [this](const QString &out) { setLine(0, out); });
    runCmd("uname", {"-r"}, [this](const QString &out) { setLine(1, out); });
    runCmd("uptime", {"-p"}, [this](const QString &out) {
        setLine(2, out.mid(3).trimmed());
    });

    // CPU
    runCmd("sh", {"-c", "cat /proc/stat | head -1"},
           [this](const QString &out) {
        static quint64 prevIdle = 0, prevTotal = 0;
        auto parts = out.split(' ', Qt::SkipEmptyParts);
        if (parts.size() < 5) return;
        quint64 user  = parts[1].toULongLong();
        quint64 nice  = parts[2].toULongLong();
        quint64 sys   = parts[3].toULongLong();
        quint64 idle  = parts[4].toULongLong();
        quint64 total = user + nice + sys + idle;

        if (prevTotal > 0) {
            quint64 dTotal = total - prevTotal;
            quint64 dIdle  = idle  - prevIdle;
            if (dTotal > 0) {
                double pct = 100.0 * (dTotal - dIdle) / dTotal;
                setLine(3, QString("%1%").arg(pct, 0, 'f', 1));
            }
        }
        prevIdle   = idle;
        prevTotal  = total;
    });

    // Memory
    runCmd("free", {"-h"}, [this](const QString &out) {
        auto lines = out.split('\n');
        if (lines.size() >= 2) {
            auto p = lines[1].split(QRegularExpression("\\s+"));
            if (p.size() >= 3) setLine(4, QString("%1 / %2").arg(p[2], p[1]));
        }
        if (lines.size() >= 3) {
            auto p = lines[2].split(QRegularExpression("\\s+"));
            if (p.size() >= 3) setLine(5, QString("%1 / %2").arg(p[2], p[1]));
        }
    });

    // Root & home
    auto dfLine = [this](int row, const QString &mnt) {
        runCmd("df", {"-h", mnt}, [this, row](const QString &out) {
            auto lines = out.split('\n');
            if (lines.size() < 2) return;
            auto p = lines[1].split(QRegularExpression("\\s+"));
            if (p.size() >= 4)
                setLine(row, QString("%1 / %2").arg(p[2], p[1]));
        });
    };
    dfLine(6, "/");
    dfLine(7, "/home");

    // Network
    runCmd("sh", {"-c", "ip -4 -o addr show | grep -v lo | awk '{print $4}' | head -1"},
           [this](const QString &out) {
               setLine(8, out.isEmpty() ? "无连接" : out);
           });

    // CPU temperature
    runCmd("sh", {"-c", "sensors -j 2>/dev/null | grep -oP '\"temp\\d+_input\":\\s*[\\d.]+' | head -1"},
           [this](const QString &out) {
               auto parts = out.split(':');
               if (parts.size() == 2)
                   setLine(9, QString("%1°C").arg(parts[1].trimmed().toDouble(), 0, 'f', 1));
               else
                   setLine(9, "N/A");
           });

    // NVMe temp via sensors
    runCmd("sh", {"-c", "sensors 2>/dev/null | grep 'Composite' | awk '{print $2}' | head -1"},
           [this](const QString &out) {
               if (!out.isEmpty()) setLine(10, out);
               else setLine(10, "N/A");
           });

    // GPU temp
    runCmd("sh", {"-c", "sensors amdgpu-* 2>/dev/null | grep 'edge' | awk '{print $2}' | head -1"},
           [this](const QString &out) {
               if (!out.isEmpty()) setLine(11, out);
               else setLine(11, "N/A");
           });

    // ───── 磁盘监控 ─────

    m_diskStatus->setText("正在检测磁盘…");

    runCmd("lsblk", {"-J", "-o", "NAME,SIZE,TYPE,MODEL,MOUNTPOINT"},
           [this](const QString &out) {
        QJsonParseError err;
        auto doc = QJsonDocument::fromJson(out.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError) {
            m_diskStatus->setText("磁盘检测失败");
            return;
        }

        // 只收集 type=disk 的顶层设备
        struct DiskInfo { QString name, size, model, mount; };
        QVector<DiskInfo> disks;
        std::function<void(const QJsonArray &)> collectDisks;
        collectDisks = [&](const QJsonArray &arr) {
            for (const auto &val : arr) {
                auto obj = val.toObject();
                if (obj.value("type").toString() == "disk") {
                    DiskInfo d;
                    d.name  = obj.value("name").toString();
                    d.size  = obj.value("size").toString();
                    d.model = obj.value("model").toString().trimmed();
                    // 收集子分区的挂载点
                    QStringList mounts;
                    std::function<void(const QJsonArray &)> collectMounts;
                    collectMounts = [&](const QJsonArray &children) {
                        for (const auto &c : children) {
                            auto co = c.toObject();
                            auto mnt = co.value("mountpoint");
                            if (!mnt.isNull() && !mnt.toString().isEmpty())
                                mounts << mnt.toString();
                            auto grand = co.value("children").toArray();
                            if (!grand.isEmpty()) collectMounts(grand);
                        }
                    };
                    auto children = obj.value("children").toArray();
                    if (!children.isEmpty()) collectMounts(children);
                    d.mount = mounts.isEmpty() ? "" : mounts.join(", ");
                    disks.append(d);
                }
                auto children = obj.value("children").toArray();
                if (!children.isEmpty()) collectDisks(children);
            }
        };
        collectDisks(doc.object().value("blockdevices").toArray());

        // 填充表格
        m_diskTable->setRowCount(disks.size());
        for (int i = 0; i < disks.size(); ++i) {
            const auto &d = disks[i];
            m_diskTable->setItem(i, ColDev,   new QTableWidgetItem(d.name));
            m_diskTable->setItem(i, ColModel, new QTableWidgetItem(d.model.isEmpty() ? "—" : d.model));
            // 优先复用缓存的温度，避免每次刷新清空
            m_diskTable->setItem(i, ColTemp,  new QTableWidgetItem(
                m_tempCache.value(d.name, "…")));
            m_diskTable->setItem(i, ColSize,  new QTableWidgetItem(d.size));

            // 已用空间 / 挂载点：用 df 取
            QString mountPath = d.mount;
            m_diskTable->setItem(i, ColUsed,  new QTableWidgetItem(
                mountPath.isEmpty() ? "未挂载" : "…"));
            m_diskTable->setItem(i, ColMount, new QTableWidgetItem(
                mountPath.isEmpty() ? "—" : mountPath));
        }

        m_diskStatus->setText(QString("共 %1 块物理磁盘").arg(disks.size()));

        // 首次检测到磁盘后自动读取一次温度（含一次性免密授权）
        if (!m_disksInited) {
            m_disksInited = true;
            fetchAllTemps();
        }

        // 已用空间：定时刷新（df 轻量，不需要 root）
        for (int i = 0; i < disks.size(); ++i) {
            const auto &d = disks[i];
            if (!d.mount.isEmpty()) {
                QStringList mountParts = d.mount.split(", ");
                for (const auto &mp : mountParts) {
                    if (!mp.isEmpty()) {
                        runCmd("df", {"-h", mp}, [this, i, mp](const QString &dfOut) {
                            auto lines = dfOut.split('\n');
                            if (lines.size() < 2) return;
                            auto p = lines[1].split(QRegularExpression("\\s+"));
                            if (p.size() >= 4) {
                                m_diskTable->setItem(i, ColUsed,
                                    new QTableWidgetItem(
                                        QString("%1 / %2").arg(p[2], p[1])));
                            }
                        });
                        break;
                    }
                }
            }
        }
    });
}
