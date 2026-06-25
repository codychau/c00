#include "smartdialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QProcess>
#include <QRegularExpression>
#include <QPushButton>
#include <QFont>
#include "storagepage.h"

SmartDialog::SmartDialog(const QString &devPath, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QString("S.M.A.R.T — %1").arg(devPath));
    resize(620, 520);

    auto *layout = new QVBoxLayout(this);

    // ── 设备标题 ──
    m_titleLabel = new QLabel(devPath);
    QFont tf = m_titleLabel->font(); tf.setPointSize(14); tf.setBold(true);
    m_titleLabel->setFont(tf);
    layout->addWidget(m_titleLabel);

    // ── 健康 & 概要 ──
    auto *summaryGroup = new QGroupBox("健康状态");
    auto *summaryLayout = new QVBoxLayout(summaryGroup);

    m_healthLabel = new QLabel("检测中…");
    QFont hf = m_healthLabel->font(); hf.setPointSize(16); hf.setBold(true);
    m_healthLabel->setFont(hf);
    summaryLayout->addWidget(m_healthLabel);

    m_infoLabel = new QLabel("");
    m_infoLabel->setWordWrap(true);
    m_infoLabel->setStyleSheet("color: #666; font-size: 12px;");
    summaryLayout->addWidget(m_infoLabel);

    layout->addWidget(summaryGroup);

    // ── 属性表格 ──
    auto *attrGroup = new QGroupBox("SMART 属性");
    auto *attrLayout = new QVBoxLayout(attrGroup);

    m_attrTable = new QTableWidget(0, 5);
    m_attrTable->setHorizontalHeaderLabels({"属性", "值", "最差", "阈值", "原始值"});
    m_attrTable->horizontalHeader()->setStretchLastSection(true);
    m_attrTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_attrTable->verticalHeader()->hide();
    m_attrTable->setAlternatingRowColors(true);
    attrLayout->addWidget(m_attrTable);

    layout->addWidget(attrGroup);

    // ── 原始输出（可折叠） ──
    m_rawView = new QTextEdit();
    m_rawView->setReadOnly(true);
    m_rawView->setMaximumHeight(160);
    m_rawView->setPlaceholderText("原始 smartctl 输出...");
    m_rawView->setStyleSheet("font-family: monospace; font-size: 11px;");
    attrLayout->addWidget(m_rawView);

    // 展开/折叠按钮
    auto *toggleBtn = new QPushButton("显示原始输出");
    toggleBtn->setCheckable(true);
    connect(toggleBtn, &QPushButton::toggled, this, [this, toggleBtn](bool checked) {
        m_rawView->setVisible(checked);
        toggleBtn->setText(checked ? "隐藏原始输出" : "显示原始输出");
    });
    m_rawView->setVisible(false);
    attrLayout->addWidget(toggleBtn);

    layout->addWidget(attrGroup);

    // ── 关闭按钮 ──
    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch();
    auto *closeBtn = new QPushButton("关闭");
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
    btnRow->addWidget(closeBtn);
    layout->addLayout(btnRow);

    // ── 加载数据 ──
    loadSmartData(devPath);
}

void SmartDialog::loadSmartData(const QString &devPath)
{
    auto *p = new QProcess(this);
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, p](int, QProcess::ExitStatus) {
                QString out = QString::fromUtf8(p->readAllStandardOutput()).trimmed();
                QString err = QString::fromUtf8(p->readAllStandardError()).trimmed();
                p->deleteLater();

                if (out.isEmpty() && !err.isEmpty()) {
                    m_healthLabel->setText("⚠️ 无法获取");
                    m_infoLabel->setText(err);
                    m_rawView->setPlainText(err);
                    return;
                }

                m_rawView->setPlainText(out);
                parseAndDisplay(out);
            });
    p->start("pkexec", {"smartctl", "-a", devPath});
}

void SmartDialog::parseAndDisplay(const QString &raw)
{
    // ── 设备信息 ──
    QString model, serial, fw;

    // NVMe format
    QRegularExpression::PatternOption rm = QRegularExpression::MultilineOption;
    auto reModel  = QRegularExpression(R"(^Model\s*Number:\s+(.+)$)", rm);
    auto reSerial = QRegularExpression(R"(^Serial\s*Number:\s+(.+)$)", rm);
    auto reFW     = QRegularExpression(R"(^Firmware\s*(Version|Revision):\s+(.+)$)", rm);

    // ATA format
    auto reModelATA  = QRegularExpression(R"(^Device\s*Model:\s+(.+)$)", rm);
    auto reSerialATA = QRegularExpression(R"(^Serial\s*Number:\s+(.+)$)", rm);

    auto m1 = reModel.match(raw);
    if (m1.hasMatch())       model = m1.captured(1).trimmed();
    else {
        auto m1a = reModelATA.match(raw);
        if (m1a.hasMatch())  model = m1a.captured(1).trimmed();
    }

    auto m2 = reSerial.match(raw);
    if (m2.hasMatch())       serial = m2.captured(1).trimmed();
    else {
        auto m2a = reSerialATA.match(raw);
        if (m2a.hasMatch()) serial = m2a.captured(1).trimmed();
    }

    auto m3 = reFW.match(raw);
    if (m3.hasMatch()) fw = m3.captured(2).trimmed();

    m_titleLabel->setText(QString("%1\n%2 | %3 | FW: %4")
                          .arg(m_titleLabel->text(), model, serial, fw));

    // ── 健康状态 ──
    auto reHealth = QRegularExpression(R"(SMART overall-health self-assessment test result:\s+(.+))", rm);
    auto mh = reHealth.match(raw);
    QString health = mh.hasMatch() ? mh.captured(1).trimmed() : "未知";

    if (health.toUpper() == "PASSED") {
        m_healthLabel->setText("🟢 " + health);
        m_healthLabel->setStyleSheet("color: #22c55e; font-size: 16px; font-weight: bold;");
    } else if (health.toUpper().contains("FAIL")) {
        m_healthLabel->setText("🔴 " + health);
        m_healthLabel->setStyleSheet("color: #ef4444; font-size: 16px; font-weight: bold;");
    } else {
        m_healthLabel->setText("🟡 " + health);
        m_healthLabel->setStyleSheet("color: #eab308; font-size: 16px; font-weight: bold;");
    }

    // ── 关键指标 ──
    QStringList infoLines;
    int temp = -1, hours = -1;

    // NVMe temperature
    auto reTemp = QRegularExpression(R"(^Temperature:\s+(\d+)\s+Celsius)", rm);
    auto mt = reTemp.match(raw);
    if (mt.hasMatch()) temp = mt.captured(1).toInt();

    // ATA temperature (from attribute 194)
    auto reAttrLine = QRegularExpression(
        R"(^\s*(\d+)\s+(\S+(?:\s+\S+)*)\s+\S+\s+(\d+)\s+(\d+)\s+(\d+)\s+\S+\s+\S+\s+\S+\s+([0-9a-fA-Fx]+|\d+))", rm);

    // Power-on hours
    auto rePHours = QRegularExpression(R"(^Power.?.?[Oo]n Hours:\s+([\d,]+))", rm);
    auto mph = rePHours.match(raw);
    if (mph.hasMatch()) {
        hours = mph.captured(1).remove(',').toInt();
        infoLines << QString("通电时间: %1 小时").arg(hours);
    }

    // NVMe: Percentage Used
    auto rePct = QRegularExpression(R"(Percentage Used:\s+(\d+)\s*%)", rm);
    auto mpct = rePct.match(raw);
    if (mpct.hasMatch())
        infoLines << QString("寿命消耗: %1%").arg(mpct.captured(1));

    // NVMe: Data Written
    auto reDW = QRegularExpression(R"(Data Units Written:\s+([\d,]+)\s+\[([\d.]+) \w+\])", rm);
    auto mdw = reDW.match(raw);
    if (mdw.hasMatch())
        infoLines << QString("总写入: %1 TB").arg(mdw.captured(2));

    // Media Errors
    auto reME = QRegularExpression(R"((?:Media|Error Information Log Entries):\s+(\d+))", rm);
    auto mme = reME.match(raw);
    if (mme.hasMatch())
        infoLines << QString("介质错误: %1").arg(mme.captured(1));

    if (temp >= 0)
        infoLines.prepend(QString("温度: %1°C").arg(temp));

    if (infoLines.isEmpty())
        infoLines << "（无详细指标）";

    m_infoLabel->setText(infoLines.join("  |  "));

    // ── 属性表格 ──
    m_attrTable->setRowCount(0);

    // NVMe 属性
    QRegularExpression nvmeAttrRe(
        R"(^(Critical Warning|Temperature|Available Spare(?:\s+Threshold)?|Percentage Used|"
        R"(?:Data Units Read|Data Units Written)|Host (?:Read|Write) Commands|"
        R"(?:Power Cycles|Power On Hours|Unsafe Shutdowns)|"
        R"(?:Media and Data Integrity Errors|Error Information Log Entries))\s*:\s+(.+)$)", rm);

    for (const auto &line : raw.split('\n')) {
        // NVMe 行
        auto nm = nvmeAttrRe.match(line.trimmed());
        if (nm.hasMatch()) {
            int row = m_attrTable->rowCount();
            m_attrTable->insertRow(row);
            m_attrTable->setItem(row, 0, new QTableWidgetItem(nm.captured(1).trimmed()));
            QString val = nm.captured(2).trimmed();
            m_attrTable->setItem(row, 1, new QTableWidgetItem(val));

            // 如果值包含百分比或数字，提取
            static QRegularExpression pctRe(R"((\d+)\s*%)");
            auto pm = pctRe.match(val);
            if (pm.hasMatch()) {
                // 用百分比部分显示
            }

            for (int c = 1; c < 5; ++c) {
                auto *it = m_attrTable->item(row, c);
                if (!it && c == 1) {
                    m_attrTable->setItem(row, c, new QTableWidgetItem(val));
                } else if (!it) {
                    m_attrTable->setItem(row, c, new QTableWidgetItem("-"));
                }
            }
            continue;
        }

        // ATA SMART 属性行（匹配数字开头的属性行）
        auto am = reAttrLine.match(line);
        if (am.hasMatch()) {
            int row = m_attrTable->rowCount();
            m_attrTable->insertRow(row);

            QString id   = am.captured(1);
            QString name = am.captured(2).trimmed();
            QString value = am.captured(3);
            QString worst = am.captured(4);
            QString thresh = am.captured(5);
            QString raw_val = am.captured(6);

            m_attrTable->setItem(row, 0, new QTableWidgetItem(
                QString("#%1 %2").arg(id, name)));

            // 高亮关注属性
            bool warn = false;
            if (name.contains("Reallocated_Sector_Ct") && raw_val.toLongLong() > 0)
                warn = true;
            if (name.contains("Current_Pending_Sector") && raw_val.toLongLong() > 0)
                warn = true;
            if (name.contains("Offline_Uncorrectable") && raw_val.toLongLong() > 0)
                warn = true;

            m_attrTable->setItem(row, 1, new QTableWidgetItem(value));
            m_attrTable->setItem(row, 2, new QTableWidgetItem(worst));
            m_attrTable->setItem(row, 3, new QTableWidgetItem(thresh));

            QString displayRaw = raw_val;
            // Power-on hours 特殊处理
            if (name == "Power_On_Hours" && !raw_val.startsWith("0x")) {
                hours = raw_val.toLongLong();
                displayRaw = QString("%1 小时").arg(hours);
            }
            // Temperature 特殊处理
            if (name == "Temperature_Celsius" && !raw_val.startsWith("0x")) {
                temp = raw_val.toInt();
                displayRaw = QString("%1°C").arg(temp);
            }

            m_attrTable->setItem(row, 4, new QTableWidgetItem(displayRaw));

            if (warn) {
                QColor warnColor("#fee2e2");
                for (int c = 0; c < 5; ++c) {
                    if (auto *it = m_attrTable->item(row, c))
                        it->setBackground(warnColor);
                }
            }
        }
    }

    m_attrTable->resizeColumnsToContents();
}
