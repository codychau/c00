#include "pcidialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QProcess>
#include <QDialogButtonBox>
#include <QRegularExpression>
#include <QCheckBox>

PCIDialog::PCIDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("选择 PCI 设备");
    resize(700, 500);

    auto *layout = new QVBoxLayout(this);

    // 搜索框
    auto *searchRow = new QHBoxLayout();
    auto *searchIcon = new QLabel("🔍");
    searchRow->addWidget(searchIcon);
    m_searchEdit = new QLineEdit();
    m_searchEdit->setPlaceholderText("搜索设备... (支持 BDF / 描述 / 驱动名)");
    connect(m_searchEdit, &QLineEdit::textChanged, this, &PCIDialog::applyFilter);
    searchRow->addWidget(m_searchEdit, 1);
    layout->addLayout(searchRow);

    // 提示
    auto *hint = new QLabel("点击行切换选中 | 已绑定 vfio-pci 驱动的设备标为绿色");
    hint->setStyleSheet("color: #888; font-size: 11px;");
    layout->addWidget(hint);

    // 表格
    m_table = new QTableWidget(0, 4);
    m_table->setHorizontalHeaderLabels({"", "BDF 地址", "设备描述", "内核驱动"});
    m_table->setColumnWidth(0, 30);
    m_table->setColumnWidth(1, 120);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->verticalHeader()->hide();
    m_table->setAlternatingRowColors(true);
    connect(m_table, &QTableWidget::cellClicked, this, &PCIDialog::toggleRow);
    layout->addWidget(m_table);

    // 状态
    m_statusLabel = new QLabel("正在扫描 PCI 设备...");
    m_statusLabel->setStyleSheet("color: #888;");
    layout->addWidget(m_statusLabel);

    // 按钮
    auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(btns, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(btns);

    // 加载设备
    loadPCIDevices();
    applyFilter("");
}

// ── 解析 lspci 输出 ──
void PCIDialog::loadPCIDevices()
{
    m_allDevices.clear();

    // 1) lspci -D 获取 BDF + 描述
    QProcess p1;
    p1.start("lspci", {"-D"});
    p1.waitForFinished(5000);
    QStringList lines = QString::fromUtf8(p1.readAllStandardOutput()).split('\n',
        Qt::SkipEmptyParts);

    // 2) lspci -D -k 获取内核驱动信息
    QProcess p2;
    p2.start("lspci", {"-D", "-k"});
    p2.waitForFinished(5000);
    QStringList klines = QString::fromUtf8(p2.readAllStandardOutput()).split('\n',
        Qt::SkipEmptyParts);

    // 建立 BDF -> driver 映射
    QHash<QString, QString> driverMap;
    QString currentBdf;
    for (const auto &kl : klines) {
        if (kl.isEmpty()) { currentBdf.clear(); continue; }
        // 行首是 BDF 地址开头
        static QRegularExpression bdfRe(R"(^([0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-9a-fA-F]) )");
        auto m = bdfRe.match(kl);
        if (m.hasMatch()) {
            currentBdf = m.captured(1);
        } else if (!currentBdf.isEmpty()) {
            // driver 行
            static QRegularExpression drvRe(R"(Kernel driver in use:\s*(\S+))");
            auto dm = drvRe.match(kl);
            if (dm.hasMatch()) {
                driverMap[currentBdf] = dm.captured(1);
            }
        }
    }

    // 解析设备列表
    static QRegularExpression lineRe(
        R"(^([0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-9a-fA-F])\s+(.*)$)");

    for (const auto &line : lines) {
        auto m = lineRe.match(line);
        if (!m.hasMatch()) continue;

        PCIDeviceInfo dev;
        dev.bdf    = m.captured(1);
        dev.desc   = m.captured(2).trimmed();
        dev.driver = driverMap.value(dev.bdf);
        m_allDevices.append(dev);
    }
}

// ── 按搜索词过滤并填充表格 ──
void PCIDialog::applyFilter(const QString &text)
{
    m_filtered.clear();

    QString keyword = text.trimmed().toLower();
    for (auto &dev : m_allDevices) {
        if (keyword.isEmpty()
            || dev.bdf.contains(keyword)
            || dev.desc.toLower().contains(keyword)
            || dev.driver.toLower().contains(keyword))
        {
            // 保留选中状态
            m_filtered.append(dev);
        }
    }

    populateTable();
    updateStatus();
}

void PCIDialog::populateTable()
{
    m_table->setRowCount(m_filtered.size());

    for (int i = 0; i < m_filtered.size(); ++i) {
        const auto &dev = m_filtered[i];

        // 勾选框 (用 ☐/☑ 字符模拟)
        auto *checkItem = new QTableWidgetItem(dev.selected ? "☑" : "☐");
        checkItem->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(i, 0, checkItem);

        m_table->setItem(i, 1, new QTableWidgetItem(dev.bdf));
        m_table->setItem(i, 2, new QTableWidgetItem(dev.desc));
        m_table->setItem(i, 3, new QTableWidgetItem(dev.driver));

        // 已绑 vfio-pci — 绿色标记
        if (dev.driver == "vfio-pci") {
            for (int c = 0; c < 4; ++c) {
                if (auto *it = m_table->item(i, c))
                    it->setForeground(QColor("#22c55e"));
            }
        }

        // 选中行加高亮背景
        if (dev.selected) {
            for (int c = 0; c < 4; ++c) {
                if (auto *it = m_table->item(i, c))
                    it->setBackground(QColor("#1e3a5f"));
            }
        }
    }
}

void PCIDialog::toggleRow(int row, int)
{
    if (row < 0 || row >= m_filtered.size()) return;

    m_filtered[row].selected = !m_filtered[row].selected;

    // 同步到 m_allDevices
    for (auto &dev : m_allDevices) {
        if (dev.bdf == m_filtered[row].bdf) {
            dev.selected = m_filtered[row].selected;
            break;
        }
    }

    populateTable();
    updateStatus();
}

void PCIDialog::updateStatus()
{
    int count = 0;
    for (const auto &dev : m_allDevices) {
        if (dev.selected) count++;
    }
    m_statusLabel->setText(
        QString("共 %1 个设备, 已选 %2 个").arg(m_allDevices.size()).arg(count));
}

void PCIDialog::selectBDF(const QString &bdf)
{
    for (auto &dev : m_allDevices) {
        if (dev.bdf == bdf) {
            dev.selected = true;
            return;
        }
    }
}

QStringList PCIDialog::selectedBDFs() const
{
    QStringList list;
    for (const auto &dev : m_allDevices) {
        if (dev.selected) list.append(dev.bdf);
    }
    return list;
}
