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
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QAction>

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

    // ── GPU 温度监控 ──
    auto *gpuGroup = new QGroupBox("显卡温度");
    auto *gpuLayout = new QVBoxLayout(gpuGroup);

    m_gpuTable = new QTableWidget(0, 7);
    m_gpuTable->setHorizontalHeaderLabels({"GPU", "型号", "PCI 槽位", "核心频率", "显存占用", "风扇", "温度"});
    m_gpuTable->horizontalHeader()->setStretchLastSection(true);
    m_gpuTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_gpuTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_gpuTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_gpuTable->verticalHeader()->hide();
    m_gpuTable->setAlternatingRowColors(true);
    m_gpuTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_gpuTable, &QTableWidget::customContextMenuRequested,
            this, &StatusPage::onGpuContextMenu);
    gpuLayout->addWidget(m_gpuTable);

    layout->addWidget(gpuGroup);

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
    layout->addLayout(tempRow);
    layout->addStretch();

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

// ── GPU 温度自动检测（GPU0..GPUx） ──

void StatusPage::readGpuTemperature(GpuInfo &g)
{
    // 通过 hwmon 按 PCI 槽位段（如 "0000:03:00.0"）匹配
    const QString pciTag = QString("0000:%1").arg(g.slot);
    QDir hwmonDir("/sys/class/hwmon");
    const auto hwmons = hwmonDir.entryList({"hwmon*"}, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto &h : hwmons) {
        const QString base = QString("/sys/class/hwmon/%1").arg(h);
        QFile nameFile(base + "/name");
        if (!nameFile.open(QIODevice::ReadOnly)) continue;
        const QString name = QString::fromUtf8(nameFile.readAll()).trimmed();
        nameFile.close();
        if (name != "amdgpu" && name != "nvidia") continue;
        (void)name;

        // hwmonN/device -> 0000:xx:xx.x 槽位
        QFileInfo devInfo(base + "/device");
        QString devPath = devInfo.isSymLink()
            ? devInfo.symLinkTarget() : base + "/device";
        if (!devPath.endsWith(pciTag)) continue;

        // 取 temp*_input：优先带 label 的 edge/junction（核心温度）
        QString best;
        const auto temps = QDir(base).entryList({"temp*_input"}, QDir::Files);
        for (const auto &t : temps) {
            QFile f(base + "/" + t);
            if (!f.open(QIODevice::ReadOnly)) continue;
            double v = QString::fromUtf8(f.readAll()).trimmed().toDouble();
            f.close();
            if (v <= 0) continue;

            QString labelPath = t;
            labelPath.replace("_input", "_label");
            QFile lf(base + "/" + labelPath);
            QString label;
            if (lf.open(QIODevice::ReadOnly)) {
                label = QString::fromUtf8(lf.readAll()).trimmed();
                lf.close();
            }
            // edge 或 junction 为核心温度，优先取
            if (label == "edge" || label == "junction") {
                g.temp = QString("%1°C").arg(v / 1000.0, 0, 'f', 0);
            } else if (label.isEmpty() || label.startsWith("temp")) {
                best = QString("%1°C").arg(v / 1000.0, 0, 'f', 0);
            }
        }
        if (g.temp.isEmpty() && !best.isEmpty())
            g.temp = best;
        if (g.temp.isEmpty())
            g.temp = "N/A";

        // 读取 GPU 核心频率（sclk，单位 Hz，转 MHz 显示）
        const auto freqs = QDir(base).entryList({"freq*_input"}, QDir::Files);
        for (const auto &f : freqs) {
            QFile ff(base + "/" + f);
            if (!ff.open(QIODevice::ReadOnly)) continue;
            double hz = QString::fromUtf8(ff.readAll()).trimmed().toDouble();
            ff.close();
            if (hz <= 0) continue;

            QString labelPath = f;
            labelPath.replace("_input", "_label");
            QFile lf2(base + "/" + labelPath);
            QString label;
            if (lf2.open(QIODevice::ReadOnly)) {
                label = QString::fromUtf8(lf2.readAll()).trimmed();
                lf2.close();
            }
            if (label == "sclk") {
                g.sclk = QString("~%1 MHz").arg(hz / 1e6, 0, 'f', 0);
                break;
            }
        }
        if (g.sclk.isEmpty())
            g.sclk = "N/A";

        // 读取风扇转速（RPM，单位转/分）
        const auto fans = QDir(base).entryList({"fan*_input"}, QDir::Files);
        for (const auto &f : fans) {
            QFile ff(base + "/" + f);
            if (!ff.open(QIODevice::ReadOnly)) continue;
            double rpm = QString::fromUtf8(ff.readAll()).trimmed().toDouble();
            ff.close();
            if (rpm > 0) {
                g.fan = QString("%1 RPM").arg(rpm, 0, 'f', 0);
                break;
            }
        }
        if (g.fan.isEmpty())
            g.fan = "—";
        return;
    }
    g.temp = "N/A";
    g.sclk = "N/A";
    g.fan = "—";
}

void StatusPage::checkGpuTools(GpuInfo &g)
{
    // 定位 /sys/class/drm/cardN/device 对应 PCI 槽位
    const QString pciTag = QString("0000:%1").arg(g.slot);
    QDir drmDir("/sys/class/drm");
    const auto cards = drmDir.entryList({"card*"}, QDir::Dirs | QDir::NoDotAndDotDot);
    QString devPath;
    for (const auto &c : cards) {
        // 跳过附带输出接口的目录（cardN-DP-1 等）
        if (c.contains('-')) continue;
        QFileInfo info(QString("/sys/class/drm/%1/device").arg(c));
        if (!info.isSymLink()) continue;
        if (info.symLinkTarget().endsWith(pciTag)) {
            devPath = QString("/sys/class/drm/%1/device").arg(c);
            break;
        }
    }
    if (devPath.isEmpty())
        return;  // 未找到匹配的 drm 设备，视为不可限频

    g.devPath = devPath;

    // 读取显存占用（bytes）
    auto readBytes = [](const QString &p) -> double {
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly)) return -1;
        double v = QString::fromUtf8(f.readAll()).trimmed().toDouble();
        f.close();
        return v;
    };
    double used = readBytes(devPath + "/mem_info_vram_used");
    double total = readBytes(devPath + "/mem_info_vram_total");
    if (used >= 0 && total > 0)
        g.vram = QString("%1 / %2 GiB")
                     .arg(used / 1073741824.0, 0, 'f', 2)
                     .arg(total / 1073741824.0, 0, 'f', 2);
    else
        g.vram = "—";

    // 优先：pp_od_clk_voltage —— 支持直接设 SCLK 上限（“s 1 <max>”）
    if (QFile::exists(devPath + "/pp_od_clk_voltage")) {
        g.hasOd = true;
        g.canLimit = true;
        // OD_SCLK 最大值档作为出厂上限
        QString content;
        QFile od(devPath + "/pp_od_clk_voltage");
        if (od.open(QIODevice::ReadOnly)) {
            content = QString::fromUtf8(od.readAll()).trimmed();
            od.close();
        }
        QRegularExpression mhzRe(R"(OD_SCLK:\s*((?:\d+:\s*\d+Mhz\s*)+))");
        auto m = mhzRe.match(content);
        if (m.hasMatch()) {
            QRegularExpression numRe(R"(\d+Mhz)");
            auto it = numRe.globalMatch(m.captured(1));
            QStringList nums;
            while (it.hasNext()) nums << it.next().captured(0);
            if (!nums.isEmpty())
                g.stockMax = nums.last(); // 如 "2200Mhz"
        }
        return;
    }

    // 一般 amdgpu：power_dpm_force_performance_level + pp_dpm_sclk
    // 用档位集合限频（写 pp_dpm_sclk 仅启用某些档位，从而封顶）
    if (QFile::exists(devPath + "/power_dpm_force_performance_level") &&
        QFile::exists(devPath + "/pp_dpm_sclk")) {
        QString content;
        QFile sclk(devPath + "/pp_dpm_sclk");
        if (sclk.open(QIODevice::ReadOnly)) {
            content = QString::fromUtf8(sclk.readAll()).trimmed();
            sclk.close();
        }
        // 收集带索引的档位行：如 "1: 500Mhz"，跳过 "S:" 深睡眠行
        QRegularExpression lineRe(R"(^\s*(\d+):\s+(\d+)Mhz\s*[\*]?\s*$)",
                                  QRegularExpression::MultilineOption);
        auto it = lineRe.globalMatch(content);
        for (;;) {
            auto m = it.next();
            if (!m.hasMatch()) break;
            g.limitBands << QString("%1:%2").arg(m.captured(1), m.captured(2)) ;
        }
        // stockMax = 最高档 MHz（limitBands 格式 "索引:MHz"）
        int best = 0;
        for (const auto &b : g.limitBands) {
            int v = b.section(':', 1).toInt();
            if (v > best) best = v;
        }
        g.stockMax = QString("%1Mhz").arg(best);
        g.canLimit = !g.limitBands.isEmpty();
        return; // 注意：R9700 档位含 "S:72" S 行被跳过，只剩 500/2350
    }
}

void StatusPage::limitGpuFreq(const GpuInfo &g, const QString &maxMhz)
{
    if (g.devPath.isEmpty()) return;

    QStringList script;
    script << "echo manual > " + g.devPath + "/power_dpm_force_performance_level";

    if (g.hasOd) {
        // OD：设置 SCLK 最大档为给定值
        script << "echo 's 1 " + maxMhz + "' > " + g.devPath + "/pp_od_clk_voltage";
        script << "echo 'c' > " + g.devPath + "/pp_od_clk_voltage";
    } else if (maxMhz.contains(',')) {
        // 档位索引集合（如 "0,1"）：仅启用这些档位
        QString bands = maxMhz;
        script << "echo '" + bands.replace(',', ' ') + "' > " + g.devPath + "/pp_dpm_sclk";
    } else {
        // 按 MHz 上限折算：仅启用 ≤ 上限的档位
        int target = maxMhz.toInt();
        QStringList allowed;
        QRegularExpression idxRe(R"(\d+)");
        for (const auto &b : g.limitBands) {
            // b 形如 "1:500" 或 "2:2350"
            auto mm = idxRe.match(b);
            int idx = 0, mhz = 0;
            if (mm.hasMatch()) {
                idx = mm.captured(0).toInt();
                auto rest = b.mid(mm.capturedEnd(0));
                auto mm2 = idxRe.match(rest);
                if (mm2.hasMatch()) mhz = mm2.captured(0).toInt();
            }
            if (mhz > 0 && mhz <= target)
                allowed << QString::number(idx);
        }
        if (!allowed.isEmpty())
            script << "echo '" + allowed.join(' ') + "' > " + g.devPath + "/pp_dpm_sclk";
    }

    auto *proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [proc, this](int, QProcess::ExitStatus) {
                proc->deleteLater();
                refresh(); // 刷新以反映新频率
            });
    proc->start("pkexec", {"sh", "-c", script.join(" && ")});
}

void StatusPage::resetGpuFreq(const GpuInfo &g)
{
    if (g.devPath.isEmpty()) return;

    QStringList script;
    // 恢复自动调度（不限制）
    script << "echo auto > " + g.devPath + "/power_dpm_force_performance_level";
    if (g.hasOd) {
        // OD 表恢复默认
        script << "echo 'r' > " + g.devPath + "/pp_od_clk_voltage";
        script << "echo 'c' > " + g.devPath + "/pp_od_clk_voltage";
    }

    auto *proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [proc, this](int, QProcess::ExitStatus) {
                proc->deleteLater();
                refresh();
            });
    proc->start("pkexec", {"sh", "-c", script.join(" && ")});
}

void StatusPage::onGpuContextMenu(const QPoint &pos)
{
    int row = m_gpuTable->rowAt(pos.y());
    if (row < 0 || row >= m_gpus.size()) return;

    // 值拷贝而非引用：menu.exec/QInputDialog 的嵌套事件循环中，
    // 定时 refresh() 会重建 m_gpus，引用会悬垂导致崩溃。
    GpuInfo g = m_gpus[row];
    QMenu menu(this);

    QAction *limitAct = menu.addAction("⚙️  编辑频率限制…");
    limitAct->setEnabled(g.canLimit);
    QAction *resetAct = menu.addAction("↩️  恢复默认（不限制）");
    resetAct->setEnabled(g.canLimit);

    menu.addSeparator();
    auto *infoAct = menu.addAction(g.canLimit
        ? QString("支持限制频率（最高 %1）").arg(g.stockMax)
        : "该 GPU 不支持修改频率");
    infoAct->setEnabled(false);

    QAction *chosen = menu.exec(m_gpuTable->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    if (chosen == limitAct) {
        if (g.hasOd) {
            // OD 卡：输入精确上限 MHz
            bool ok = false;
            QString cur = g.stockMax.isEmpty() ? "0" : g.stockMax.mid(0, g.stockMax.size() - 4);
            int base = cur.toInt();
            int max = base > 0 ? base : 3000;
            int val = QInputDialog::getInt(this, "限制 GPU 核心频率",
                QString("%1 (GPU%2) 当前最高 %3\n请输入频率上限 (MHz)：")
                    .arg(g.model).arg(row).arg(g.stockMax),
                max, 100, max, 50, &ok);
            if (ok)
                limitGpuFreq(g, QString::number(val));
        } else if (!g.limitBands.isEmpty()) {
            // 档位卡：下拉选择上限档位
            QStringList items;
            int i = 0;
            for (const auto &b : g.limitBands) {
                int mhz = b.section(':', 1).toInt();
                items << QString("限制到 %1 MHz").arg(mhz);
            }
            items << "恢复默认（不限制）";
            bool ok = false;
            QString pick = QInputDialog::getItem(
                this, "限制 GPU 核心频率",
                QString("%1 (GPU%2) 核心频率档位：\n请选择最低允许的档位（高于它的将被禁用）：")
                    .arg(g.model).arg(row),
                items, 0, false, &ok);
            if (!ok) return;
            if (pick == "恢复默认（不限制）") {
                resetGpuFreq(g);
                return;
            }
            int idx = items.indexOf(pick);
            if (idx >= 0 && idx < g.limitBands.size()) {
                // 只允许 ≤ 所选档位的档位（即只启用 0..idx）
                QStringList allowed;
                QRegularExpression idxRe(R"(\d+)");
                for (int k = 0; k <= idx; ++k) {
                    auto mm = idxRe.match(g.limitBands[k]);
                    if (mm.hasMatch())
                        allowed << mm.captured(0);
                }
                if (!allowed.isEmpty())
                    limitGpuFreq(g, allowed.join(','));
            }
        }
    } else if (chosen == resetAct) {
        resetGpuFreq(g);
    }
}

void StatusPage::fetchGpuTemps()
{
    // 第一步：lspci 列出所有显示控制器
    runCmd("lspci", {}, [this](const QString &out) {
        QList<GpuInfo> gpus;
        const auto lines = out.split('\n');
        for (const auto &l : lines) {
            if (!l.contains("VGA compatible controller") &&
                !l.contains("3D controller") &&
                !l.contains("Display controller"))
                continue;

            QStringList parts = l.split(' ', Qt::SkipEmptyParts);
            if (parts.size() < 2) continue;
            GpuInfo g;
            g.slot = parts[0];           // 如 "03:00.0"
            if (g.slot.endsWith(':')) g.slot.chop(1);

            // 型号：优先取设备描述里最后一个 [..] 括号子串
            QString desc = l.section(' ', 1, -1);
            desc = desc.section(':', 1, -1).trimmed(); // 去掉 controller 分类
            QRegularExpression bracketRe(R"(\[([^\]]+)\])");
            auto it = bracketRe.globalMatch(desc);
            QString bracketed;
            while (it.hasNext()) {
                auto m = it.next();
                QString c = m.captured(1);
                // 跳过供应商代码（形如 AMD/ATI、10de）
                if (!c.contains('/')) bracketed = c;
            }
            if (!bracketed.isEmpty())
                g.model = bracketed;
            else {
                // 去掉厂商前缀与 (rev …)，保留如 "Raphael"
                g.model = desc.remove(QRegularExpression(R"(\([^)]*\))"))
                              .remove(QRegularExpression(R"(Advanced Micro Devices, Inc\.)"))
                              .remove(QRegularExpression(R"(\[[^\]]*\])"))
                              .replace("  ", " ")
                              .trimmed();
                if (g.model.isEmpty()) g.model = desc; // 兜底用原始描述
            }
            gpus.append(g);
        }

        if (gpus.isEmpty()) {
            m_gpus.clear();
            m_gpuTable->setRowCount(1);
            m_gpuTable->setItem(0, 0, new QTableWidgetItem("—"));
            m_gpuTable->setItem(0, 1, new QTableWidgetItem("未检测到独立显卡"));
            m_gpuTable->setItem(0, 2, new QTableWidgetItem("—"));
            m_gpuTable->setItem(0, 3, new QTableWidgetItem("—"));
            m_gpuTable->setItem(0, 4, new QTableWidgetItem("—"));
            m_gpuTable->setItem(0, 5, new QTableWidgetItem("—"));
            m_gpuTable->setItem(0, 6, new QTableWidgetItem("—"));
            setLine(11, "N/A");
            return;
        }

        m_gpuTable->setRowCount(gpus.size());
        m_gpus = gpus;
        for (int i = 0; i < gpus.size(); ++i) {
            m_gpuTable->setItem(i, 0, new QTableWidgetItem(QString("GPU%1").arg(i)));
            m_gpuTable->setItem(i, 1, new QTableWidgetItem(gpus[i].model));
            m_gpuTable->setItem(i, 2, new QTableWidgetItem(gpus[i].slot));
            m_gpuTable->setItem(i, 3, new QTableWidgetItem("…"));
            m_gpuTable->setItem(i, 4, new QTableWidgetItem("…"));
            m_gpuTable->setItem(i, 5, new QTableWidgetItem("…"));
            m_gpuTable->setItem(i, 6, new QTableWidgetItem("…"));
        }

        // 第二步：逐块读温度与频率，并检测限频能力
        for (int i = 0; i < gpus.size(); ++i) {
            GpuInfo g = gpus[i];
            readGpuTemperature(g);
            checkGpuTools(g); // 期间会读显存
            if (auto *item = m_gpuTable->item(i, 6))
                item->setText(g.temp);
            if (auto *sclkItem = m_gpuTable->item(i, 3))
                sclkItem->setText(g.sclk);
            if (auto *fanItem = m_gpuTable->item(i, 5))
                fanItem->setText(g.fan);
            if (auto *vramItem = m_gpuTable->item(i, 4))
                vramItem->setText(g.vram);
            if (i == 0)
                setLine(11, g.temp);
            m_gpus[i] = g;
        }
    });
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

    // GPU temp（概览行取第一块可用 GPU）
    fetchGpuTemps();

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
