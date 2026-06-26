#include "storagedevicepage.h"
#include "smartdialog.h"
#include "formatdialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDialog>
#include <QLineEdit>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QDir>

// 自定义角色：存储设备类型
static constexpr int RoleType   = Qt::UserRole + 1;
static constexpr int RoleName   = Qt::UserRole + 2;
static constexpr int RoleFsType = Qt::UserRole + 3;
static constexpr int RoleMount  = Qt::UserRole + 4;

// 前向声明
static int countAllItems(QTreeWidgetItem *root);

StorageDevicePage::StorageDevicePage(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    // ── 后台格式化提示条 ──
    m_formatBanner = new QLabel(
        "⚠️  有格式化任务正在进行，请等待结束...");
    m_formatBanner->setStyleSheet(
        "QLabel { background: #fef3c7; color: #92400e; font-weight: bold; "
        "padding: 12px 16px; border-radius: 4px; border: 1px solid #fbbf24; "
        "font-size: 14px; }");
    m_formatBanner->setVisible(false);
    layout->addWidget(m_formatBanner);

    // ── 树形列表 ──
    m_tree = new QTreeWidget();
    m_tree->setColumnCount(5);
    m_tree->setHeaderLabels({"设备", "大小", "类型", "文件系统", "挂载点"});
    m_tree->header()->setStretchLastSection(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setAlternatingRowColors(true);
    m_tree->setAnimated(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setAllColumnsShowFocus(true);
    connect(m_tree, &QTreeWidget::itemSelectionChanged,
            this, &StorageDevicePage::onSelectionChanged);
    layout->addWidget(m_tree, 1);

    // ── 底部按钮行 ──
    auto *row = new QHBoxLayout();
    m_status = new QLabel("双击设备行查看详情");
    m_status->setStyleSheet("color: #888;");
    row->addWidget(m_status, 1);

    m_formatBtn = new QPushButton("🔧 格式化");
    m_formatBtn->setVisible(false);
    m_formatBtn->setToolTip("格式化此分区");
    m_formatBtn->setStyleSheet(
        "QPushButton { background: #dc2626; color: white; padding: 4px 16px; "
        "border-radius: 4px; font-weight: bold; }"
        "QPushButton:hover { background: #b91c1c; }");
    connect(m_formatBtn, &QPushButton::clicked, this, &StorageDevicePage::showFormatDialog);
    row->addWidget(m_formatBtn);

    m_mountBtn = new QPushButton("📂 挂载");
    m_mountBtn->setVisible(false);
    m_mountBtn->setToolTip("挂载此分区到指定目录");
    connect(m_mountBtn, &QPushButton::clicked, this, &StorageDevicePage::showMountDialog);
    row->addWidget(m_mountBtn);

    m_smartBtn = new QPushButton("🛡 S.M.A.R.T");
    m_smartBtn->setVisible(false);
    m_smartBtn->setToolTip("查看硬盘健康状态");
    connect(m_smartBtn, &QPushButton::clicked, this, &StorageDevicePage::showSmart);
    row->addWidget(m_smartBtn);

    m_refreshBtn = new QPushButton("刷新");
    connect(m_refreshBtn, &QPushButton::clicked, this, &StorageDevicePage::refresh);
    row->addWidget(m_refreshBtn);
    layout->addLayout(row);

    refresh();
}

StorageDevicePage::~StorageDevicePage()
{
    // 确保清理后台运行的格式化对话框
    if (m_formatDlg) {
        m_formatDlg->close();
        m_formatDlg->deleteLater();
        m_formatDlg = nullptr;
    }
}

void StorageDevicePage::runCmd(const QString &cmd, const QStringList &args,
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

static int countAllItems(QTreeWidgetItem *root)
{
    int n = 0;
    for (int i = 0; i < root->childCount(); ++i) {
        auto *child = root->child(i);
        n += 1 + countAllItems(child);
    }
    return n;
}

// ── 递归建树 ──
QTreeWidgetItem *StorageDevicePage::addDevice(
    QTreeWidgetItem *parent,
    const QString &name, const QString &size,
    const QString &type, const QString &fstype,
    const QString &mount)
{
    auto *item = parent
        ? new QTreeWidgetItem(parent)
        : new QTreeWidgetItem(m_tree);

    item->setText(0, name);
    item->setText(1, size);
    item->setText(2, type);
    item->setText(3, fstype.isEmpty() ? "" : fstype);
    item->setText(4, mount.isEmpty() ? "" : mount);

    // 存储原始数据
    item->setData(0, RoleType,   type);
    item->setData(0, RoleName,   name);
    item->setData(0, RoleFsType, fstype);
    item->setData(0, RoleMount,  mount);

    // 磁盘节点加粗
    if (type == "disk") {
        QFont f = item->font(0);
        f.setBold(true);
        for (int c = 0; c < 5; ++c)
            item->setFont(c, f);
    }

    return item;
}

void StorageDevicePage::buildTree(const QString &json)
{
    m_tree->clear();

    QJsonParseError err;
    auto doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError) {
        m_status->setText("解析失败");
        return;
    }

    // 递归解析 lsblk JSON
    std::function<void(const QJsonArray &, QTreeWidgetItem *)> parseDevices;
    parseDevices = [this, &parseDevices](const QJsonArray &arr,
                                          QTreeWidgetItem *parent) {
        for (const auto &val : arr) {
            auto obj = val.toObject();
            QString name   = obj.value("name").toString();
            QString size   = obj.value("size").toString();
            QString type   = obj.value("type").toString();
            auto fstype    = obj.value("fstype");
            auto mount     = obj.value("mountpoint");

            auto *item = addDevice(parent,
                name, size, type,
                fstype.isNull() ? "" : fstype.toString(),
                mount.isNull()  ? "" : mount.toString());

            auto children = obj.value("children").toArray();
            if (!children.isEmpty()) {
                parseDevices(children, item);
                item->setExpanded(true);
            }
        }
    };

    auto devices = doc.object().value("blockdevices").toArray();
    parseDevices(devices, nullptr);

    for (int c = 0; c < 4; ++c)
        m_tree->resizeColumnToContents(c);

    m_status->setText(
        QString("共 %1 个块设备").arg(countAllItems(m_tree->invisibleRootItem())));
}

void StorageDevicePage::setFormatRunning(bool running)
{
    m_formatRunning = running;
    m_formatBanner->setVisible(running);
    m_tree->setEnabled(!running);
    m_refreshBtn->setEnabled(!running);
    m_status->setText(running
        ? "🔴 格式化任务后台运行中..."
        : "就绪");
}

void StorageDevicePage::refresh()
{
    m_smartBtn->setVisible(false);
    m_mountBtn->setVisible(false);
    m_formatBtn->setVisible(false);
    m_status->setText("正在检测块设备…");

    runCmd("lsblk", {"-J", "-o", "NAME,SIZE,TYPE,FSTYPE,MOUNTPOINT"},
           [this](const QString &out) {
        buildTree(out);
    });
}

void StorageDevicePage::onSelectionChanged()
{
    auto *item = m_tree->currentItem();
    if (!item || m_formatRunning) {
        m_smartBtn->setVisible(false);
        m_mountBtn->setVisible(false);
        m_formatBtn->setVisible(false);
        return;
    }

    QString type   = item->data(0, RoleType).toString();
    QString mount  = item->data(0, RoleMount).toString();
    QString fstype = item->data(0, RoleFsType).toString();

    // SMART: 仅 disk 类型
    m_smartBtn->setVisible(type == "disk");

    // 挂载按钮: 有文件系统、未挂载、且不是 disk 本身
    bool canMount = !fstype.isEmpty() && mount.isEmpty() && type != "disk";
    m_mountBtn->setVisible(canMount);

    // 格式化按钮: part 类型且有文件系统
    bool canFormat = type == "part" && !fstype.isEmpty();
    m_formatBtn->setVisible(canFormat);
}

void StorageDevicePage::showSmart()
{
    auto *item = m_tree->currentItem();
    if (!item) return;

    QString type = item->data(0, RoleType).toString();
    if (type != "disk") return;

    QString name = item->data(0, RoleName).toString();
    SmartDialog dlg(QString("/dev/%1").arg(name), this);
    dlg.exec();
}

void StorageDevicePage::showMountDialog()
{
    auto *item = m_tree->currentItem();
    if (!item) return;

    QString devName = item->data(0, RoleName).toString();
    QString fstype  = item->data(0, RoleFsType).toString();
    QString mount   = item->data(0, RoleMount).toString();
    QString type    = item->data(0, RoleType).toString();

    if (fstype.isEmpty() || !mount.isEmpty() || type == "disk")
        return;

    QString devPath = QString("/dev/%1").arg(devName);

    // ── 自定义弹窗：路径输入 + 浏览按钮 ──
    QDialog dlg(this);
    dlg.setWindowTitle("挂载分区");
    dlg.resize(460, 140);
    auto *dlgLayout = new QVBoxLayout(&dlg);
    auto *form = new QFormLayout();

    auto *pathEdit = new QLineEdit(QString("/mnt/%1").arg(devName));
    pathEdit->setPlaceholderText("挂载点路径，如 /mnt/data");
    auto *browseBtn = new QPushButton("选择路径…");
    auto *pathRow = new QHBoxLayout();
    pathRow->addWidget(pathEdit, 1);
    pathRow->addWidget(browseBtn);
    QObject::connect(browseBtn, &QPushButton::clicked, [&]() {
        QString dir = QFileDialog::getExistingDirectory(&dlg, "选择挂载点",
            pathEdit->text().isEmpty() ? "/mnt" : pathEdit->text());
        if (!dir.isEmpty()) pathEdit->setText(dir);
    });
    form->addRow(QString("设备: %1 (%2)").arg(devPath, fstype), pathRow);
    dlgLayout->addLayout(form);

    auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    dlgLayout->addWidget(btns);

    if (dlg.exec() != QDialog::Accepted) return;
    QString mnt = pathEdit->text().trimmed();
    if (mnt.isEmpty()) return;

    // ── 校验 ──
    if (!mnt.startsWith('/')) {
        QMessageBox::warning(this, "路径无效", "挂载点必须是绝对路径（以 / 开头）");
        return;
    }

    if (mnt == "/") {
        QMessageBox::warning(this, "路径无效", "不能挂载到根目录");
        return;
    }

    // 不能是已挂载的路径
    QFile mountsFile("/proc/mounts");
    if (mountsFile.open(QIODevice::ReadOnly)) {
        QString all = QString::fromUtf8(mountsFile.readAll());
        mountsFile.close();
        for (const auto &line : all.split('\n')) {
            auto parts = line.split(' ', Qt::SkipEmptyParts);
            if (parts.size() >= 2 && parts[1] == mnt) {
                QMessageBox::warning(this, "挂载点已占用",
                    QString("「%1」已被挂载").arg(mnt));
                return;
            }
        }
    }

    // ── 创建目录（如果不存在）──
    QDir dir(mnt);
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            QMessageBox::warning(this, "目录创建失败",
                QString("无法创建目录 %1\n请检查权限").arg(mnt));
            return;
        }
    } else {
        if (!dir.isEmpty()) {
            auto ret = QMessageBox::question(this, "目录非空",
                QString("「%1」目录非空，仍然挂载？").arg(mnt),
                QMessageBox::Yes | QMessageBox::No);
            if (ret != QMessageBox::Yes) return;
        }
    }

    // ── 执行挂载 ──
    auto *p = new QProcess(this);
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, p, devPath, mnt](int exitCode, QProcess::ExitStatus) {
        QString stderr = QString::fromUtf8(p->readAllStandardError()).trimmed();
        p->deleteLater();

        if (exitCode == 0) {
            QMessageBox::information(this, "挂载成功",
                QString("%1 → %2\n挂载成功！").arg(devPath, mnt));
            refresh();
            QProcess::startDetached("xdg-open", {mnt});
        } else {
            QMessageBox::warning(this, "挂载失败",
                QString("无法挂载 %1 到 %2:\n%3").arg(devPath, mnt, stderr));
        }
    });
    p->start("pkexec", {"mount", devPath, mnt});
}

void StorageDevicePage::showFormatDialog()
{
    auto *item = m_tree->currentItem();
    if (!item) return;

    QString devName = item->data(0, RoleName).toString();
    QString fstype  = item->data(0, RoleFsType).toString();
    QString mount   = item->data(0, RoleMount).toString();
    QString type    = item->data(0, RoleType).toString();

    if (type != "part" || fstype.isEmpty()) return;

    QString devPath = QString("/dev/%1").arg(devName);
    bool isMounted = !mount.isEmpty();

    // 如果有旧的对话框在运行，提示
    if (m_formatDlg && m_formatDlg->isRunning()) {
        QMessageBox::information(this, "格式化进行中",
            "已有一个格式化任务正在运行，请等待完成。");
        return;
    }

    // 清理旧对话框
    if (m_formatDlg) {
        m_formatDlg->deleteLater();
        m_formatDlg = nullptr;
    }

    // 创建对话框（堆上，支持后台运行后不销毁）
    m_formatDlg = new FormatDialog(devPath, devName, fstype, mount, isMounted, this);
    m_formatDlg->setAttribute(Qt::WA_DeleteOnClose, false); // 我们自己管理

    connect(m_formatDlg, &FormatDialog::formatFinished,
            this, &StorageDevicePage::onFormatFinished);

    // 非后台模式：exec 阻塞等待
    m_formatDlg->exec();

    // 如果 dialog 没有进入后台模式（正常关闭），清理
    if (!m_formatDlg->isRunning()) {
        setFormatRunning(false);
        m_formatDlg->deleteLater();
        m_formatDlg = nullptr;
        refresh();
    }
}

void StorageDevicePage::onFormatFinished(bool success)
{
    if (success) {
        m_status->setText("✅ 格式化任务完成");
    } else {
        m_status->setText("❌ 格式化任务失败");
    }

    // 清理对话框
    m_formatRunning = false;
    if (m_formatDlg) {
        m_formatDlg->deleteLater();
        m_formatDlg = nullptr;
    }

    // 恢复正常界面
    setFormatRunning(false);
    refresh();
}
