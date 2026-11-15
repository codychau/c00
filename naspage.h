#ifndef NASPAGE_H
#define NASPAGE_H

#include <QWidget>
#include <QVector>
#include <QStringList>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QVector>
#include "nasmodel.h"
#include "nasboardwidget.h"
#include "nascontroller.h"

class QCheckBox;
class QLabel;
class QButtonGroup;
class QTabWidget;
class QTableWidget;
class QPushButton;
class QRadioButton;
class QWidget;

// ─────────────────────────────────────────────────────────────
//  NAS 页面：挑分区 → 拖线组 RAID → 导出 iSCSI
//
//  分工：
//   NasBoardWidget  只管画与报"用户想连谁"，无副作用
//   NasController   只管生成/执行命令，不碰界面
//   NasPage         夹在中间：拦下危险操作、把结论翻译给人看
//
//  安全线（改动前请先读）：
//   - 拖线本身**不碰磁盘**，随便试
//   - 「创建阵列」和「导出 iSCSI」是两条独立确认：阵列建成了还不一定对外
//   - 破坏性命令必须逐条展示给用户，不接受一次性"我确认了全部"
// ─────────────────────────────────────────────────────────────
class NasPage : public QWidget
{
    Q_OBJECT
public:
    explicit NasPage(QWidget *parent = nullptr);

public:
    // 仅供离线行为探针使用：设定成员并走与点击完全相同的同步路径。
    // 刻意复用 onSlotClicked 之外的同一段逻辑，避免测试走的是另一条代码
    // 路径（那样测试通过也说明不了线上行为）。
    void setMemberDevicesForTest(const QVector<QString> &devices);

private slots:
    void refresh();                       // 重新枚举分区与环境
    void onSlotClicked(const QString &device, int slotIndex);
    void onSlotRejected(const QString &device, int slotIndex, const QString &why);
    void onEngineChanged(int index);
    void createArray();
    void exportIscsi();
    void onExportClicked();            // 按当前模式分派到 iSCSI / SMB
    void exportSmb();                  // 写 SMB 片段并 reload
    void removeSmb();
    void teardownIscsiInternal();                  // 删除片段并 reload
    void teardownIscsi();
    void showStepLog(int index, bool ok, const QString &output);
    void onAllFinished(bool success);

private:
    // 把画板上的连线收集成可执行的 Plan
    Nas::Plan currentPlan() const;
    void updateSummary();
    // 按成员数刷新导出区：恰好 1 个才允许 SMB；从 >1 掉到 1 时自动切 SMB
    void syncExportMode(int memberCount);
    void refreshSmbWarning();
    QWidget *buildExtensionsTab();
    void refreshTvDeps();
    void onTvScanClicked();
    QWidget *buildRaidFsTab();
    void refreshRaidFsStatus();
    void onMountFsClicked();
    void onUmountFsClicked();
    // 逐条展示命令并要用户确认；返回要执行的步骤（可能为空 = 用户取消）
    QVector<NasController::Step> confirmSteps(const QVector<NasController::Step> &steps,
                                              const QString &title);
    void setStatus(const QString &text, bool bad = false);

    NasBoardWidget *m_board = nullptr;
    QLabel *m_envLabel = nullptr;      // 环境是否齐备
    QLabel *m_status = nullptr;
    QComboBox *m_engine = nullptr;
    QLineEdit *m_volName = nullptr;    // 阵列/卷名（破坏性确认时要手输核对）
    QSpinBox *m_chunk = nullptr;       // 条带粒度
    QLineEdit *m_iqn = nullptr;
    QLineEdit *m_portal = nullptr;     // portal 监听地址（**不是**网段白名单）
    QLabel *m_portalNote = nullptr;
    QCheckBox *m_anonAcl = nullptr;    // 允许匿名 initiator（默认关）

    // ── iSCSI 参数（原「参数」组改名而来）──
    QWidget *m_iscsiBox = nullptr;     // 打包 iSCSI 各行，便于整体显隐

    // ── SMB 参数（仅恰好 1 个分区时可用）──
    QButtonGroup *m_modeGroup = nullptr;
    QRadioButton *m_modeIscsi = nullptr;
    QRadioButton *m_modeSmb = nullptr;
    QWidget *m_smbBox = nullptr;
    QLineEdit *m_smbName = nullptr;
    QLineEdit *m_smbPath = nullptr;
    QComboBox *m_smbAuth = nullptr;    // 需密码 / 匿名只读 / 匿名可写
    QLineEdit *m_smbUser = nullptr;
    QCheckBox *m_smbWrite = nullptr;
    QLabel *m_smbWarn = nullptr;       // guest 可写 / 命中系统路径的红字
    QLabel *m_modeHint = nullptr;      // 当前为什么能/不能选 SMB
    QPushButton *m_createBtn = nullptr;
    QPushButton *m_exportBtn = nullptr;
    QPushButton *m_teardownBtn = nullptr;
    QPushButton *m_refreshBtn = nullptr;

    NasController m_ctl;
    NasController::Env m_env;
    QVector<NasBoardWidget::Disk> m_disks;
    QStringList m_sysMounts;           // 刷新时算好的系统挂载点，SMB 护栏用
    QString m_pendingConfirmName;      // 需要手输核对的卷名
    // 最近一次成功导出的对象，撤销时要用同一组值，不能让用户改完再撤
    QString m_currentIqn;
    QString m_currentBlockDev;
    QString m_currentSmbShare;         // 最近成功导出的共享名，撤销要用同一个
    // 上一帧成员数。判断「刚从 >1 掉到 1」这个跳变，不能只看当前值：
    // 否则改引擎下拉（成员数不变）也会把用户显式选的 iSCSI 翻成 SMB。
    int m_lastMemberCount = 0;

    // ── 扩展服务（电视墙）──
    // 标签页把「导出服务」和「扩展服务」分开：扩展关掉不影响导出，
    // 两者的失败域必须隔离，不能让影视扫描出错连带挡住导出。
    QTabWidget *m_tabs = nullptr;
    QWidget    *m_svcTab = nullptr;
    QWidget    *m_extTab = nullptr;
    QWidget    *m_fsTab = nullptr;       // RAID FS 标签页
    QLabel     *m_tvRoot = nullptr;      // 当前扫描根（跟随所选分区）
    QLabel     *m_tvCount = nullptr;
    QLabel     *m_tvPill = nullptr;
    QLabel     *m_tvDeps = nullptr;
    QLabel     *m_tvNote = nullptr;
    QTableWidget *m_tvTable = nullptr;
    QPushButton *m_tvScan = nullptr;
    bool        m_tvScanned = false;

    // ── RAID FS（把 RAID 卷挂到 /mnt 下的目录）──
    // 与扩展服务同理：挂目录是可选出口，失败不能连带挡住 iSCSI 导出。
    QLineEdit *m_mountPoint = nullptr;
    QLabel    *m_fsPill = nullptr;       // 环境 + 是否已实现
    QLabel    *m_fsDeps = nullptr;
    QLabel    *m_fsNote = nullptr;
    QPushButton *m_umountBtn = nullptr;  // 撤销挂载
    QPushButton *m_mountFsBtn = nullptr; // ② 挂载到目录
};

#endif // NASPAGE_H
