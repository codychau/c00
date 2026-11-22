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
#include <QProcess>          // 槽签名里要用 QProcess::ExitStatus
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
class QPlainTextEdit;
class QTimer;

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

    // 同样是探针专用：塞进一个假的探测结果，然后走与 refresh() 完全相同的
    // 那段同步代码（右上角缺失提示 + 页顶通栏按钮的显隐）。装齐之后按钮该
    // 消失这种结论，只有能喂出"装齐了"这个状态才测得到 —— 真机上不一定缺
    // 东西，靠真机状态去断言等于让探针看机器脸色。
    void setEnvForTest(const NasController::Env &env);

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

    // ── 页顶通栏「一键安装依赖的组件」（dnf / apt-get / pacman）──
    // 分四步是因为提权可能失败要重来，而装包是异步的：
    // 点击（确认） → 取密码 → 起进程 → 收输出/收尾。
    // 刻意**不走** NasController 那条 Step 队列：它按条 pkexec，
    // 而装包要"一次授权 + 全程输出可见"。
    void onInstallAllDepsClicked();
    void askInstallPassword();                  // 弹 sudo 密码框（可能要重来）
    void startInstall(const QString &password); // 真正起进程；空密码 = 免密
    void onInstallOutput();                     // 流式追加包管理器的输出
    void onInstallFinished(int exitCode, QProcess::ExitStatus status);

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
    QWidget *buildRaidFsBox();  // 导出页里「挂载到目录」那个出口框（非独立标签页）
    void refreshRaidFsStatus();
    void onMountFsClicked();
    void onUmountFsClicked();
    void onInstallFuseClicked();
    void appendInstallLog(const QString &chunk);  // 写日志框（\r 也算换行）
    // 把 m_env 翻译成右上角缺失提示 + 页顶按钮显隐。两处必须同一次算完，
    // 否则会在中间状态下互相打脸（按钮说"齐了"、红字还说"缺"）。
    void syncEnvAndInstallUi();
    // 装包补不了的几样（configfs 未挂载 / 内核没有 target 模块 / nasrs 不在），
    // 按当前出口过滤后返回，每条自带处置办法。守卫和那条说明共用它，
    // 免得两处对"还缺什么"给出不同答案。
    QStringList nonPackageBlockers() const;
    void refreshBlockNote();
    // 逐条展示命令并要用户确认；返回要执行的步骤（可能为空 = 用户取消）
    QVector<NasController::Step> confirmSteps(const QVector<NasController::Step> &steps,
                                              const QString &title);
    void setStatus(const QString &text, bool bad = false);

    NasBoardWidget *m_board = nullptr;
    QLabel *m_envLabel = nullptr;      // 环境是否齐备
    // 「装包补不了的那几样」的说明（configfs / 内核 target 模块 / nasrs 引擎）。
    // 刻意与右上角那条分开：那条说的是"缺组件"，一键安装能把它们清干净；
    // 这几样装包清不掉，混在一起就会变成"按钮消失了但还在提示缺东西"。
    QLabel *m_blockNote = nullptr;
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
    QRadioButton *m_modeMount = nullptr; // 第三个出口：挂成 /mnt 目录
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
    bool m_syncingExportMode = false; // syncExportMode 再入保护

    // ── 扩展服务（电视墙）──
    // 标签页把「导出服务」和「扩展服务」分开：扩展关掉不影响导出，
    // 两者的失败域必须隔离，不能让影视扫描出错连带挡住导出。
    QTabWidget *m_tabs = nullptr;
    QWidget    *m_svcTab = nullptr;
    QWidget    *m_extTab = nullptr;
    QWidget    *m_fsBox = nullptr;       // 挂载目录出口框（与 m_iscsiBox/m_smbBox 互斥显隐）
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
    QPushButton *m_installFuseBtn = nullptr; // 一键装 fuse3（仅 apt 系且缺时出现）

    // ── 页顶通栏「一键安装依赖的组件」──
    // 这一组的状态只有"在装 / 没在装"两种，全部围绕 m_installProc 转。
    QPushButton    *m_installAllBtn = nullptr;
    QPlainTextEdit *m_installLog    = nullptr;  // 默认隐藏，点下去才现身
    QProcess       *m_installProc   = nullptr;
    QTimer         *m_installTimer  = nullptr;  // 兜底：网络卡死不至于永远转圈
    NasController::InstallPlan m_installPlan;   // 确认过的那一批，收尾要照它报
    int  m_installAttempts = 0;                 // 密码错了几次（只许重来一次）
    bool m_installTimedOut = false;
};

#endif // NASPAGE_H
