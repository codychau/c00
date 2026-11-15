#ifndef NASCONTROLLER_H
#define NASCONTROLLER_H

#include <QObject>
#include <QVector>
#include "nasmodel.h"

class QProcess;

// ─────────────────────────────────────────────────────────────
//  NasController：把 Nas::Plan 变成现实。
//
//  两条路线，最后都汇到同一个 iSCSI 导出层：
//
//    mdadm 路线   mdadm --create /dev/mdX → LIO block 后端 → iSCSI
//    nasrs 路线   两个 backing 文件 → losetup → LIO block 后端 → iSCSI
//
//  设计约定（重要）：
//   1) 所有会改磁盘的动作都必须先生成一条"待执行命令"，由页面弹确认框，
//      用户点头才真跑。本类**不会自己启动任何破坏性进程**。
//   2) 提权统一走 pkexec（项目里 mount/service/smartctl 都是这套）。
//   3) 命令一律用 QStringList 传参，绝不拼 shell 字符串 —— 设备名和用户
//      输入的卷名会进 argv，拼字符串等于把注入口留在最危险的地方。
// ─────────────────────────────────────────────────────────────
class NasController : public QObject
{
    Q_OBJECT
public:
    // 一条待执行命令：页面展示给用户看过再跑
    struct Step
    {
        QString program;
        QStringList args;
        QString describe;      // 给用户看的一句话说明
        bool destructive = false;
        // 要写进子进程 stdin 的行（targetcli 的命令序列走这里，
        // 避免拼 shell 字符串 —— 那是把注入口留在最需要 root 的地方）
        QStringList stdinLines;
    };

    // 注意：targetcli saveconfig 会把 CHAP 密码**明文**写进
    // /etc/rtslib-fb-target/saveconfig.json，且该文件默认权限不是 600。
    // 所以本工具不提供"保存含 CHAP 的配置"这一步，除非调用方显式要。

    // 运行环境探测结果（缺什么就在 UI 上直接说，不要让用户猜）
    struct Env
    {
        bool mdadm = false;
        bool targetCli = false;   // targetcli / targetcli-fb
        bool configfsMounted = false;
        bool kernelModules = false;   // target_core_mod 等模块文件在不在
        bool losetup = false;
        bool nasrsBinary = false;     // 自研引擎可执行文件是否找到
    // RAID FS 走 FUSE，与 iSCSI 那条完全不是一套依赖：不要并进
    // readyForMdadm 的 missing，否则没装 fuse 的用户连 iSCSI 都被提示未就绪。
    bool aptAvailable = false;    // apt-get 在不在（安装按钮的另一半条件）
    bool aptFamily  = false;    // 发行版是否 ubuntu/debian 系（os-release 判定）
    bool fuseDevice = false;      // /dev/fuse 在不在
    bool fuseMount  = false;      // mount.fuse / fusermount3 在不在
// SMB 相关：Samba 是可选功能，不能并进 readyForMdadm 的 missing 里，
// 否则没装 Samba 的用户连 RAID 都会被提示环境未就绪。
bool smbd       = false;      // smbd 在不在
bool testparm   = false;      // 能否离线校验 smb.conf
bool smbService = false;      // systemd 是否有 smbd.service
        QStringList missing;          // 人类可读的缺失项
        bool readyForSmb() const { return smbd && testparm && smbService; }
    // 注意这里只表示"环境具备挂 FUSE 的条件"，不表示 raidfs 能用：
    // nasrs 目前还没有实现导出（见 planRaidfsMount 的说明）。
    bool readyForFuse() const { return nasrsBinary && fuseDevice && fuseMount; }
    // 能不能"一键装 fuse"：只在 fuse 助手还缺、且确实是 apt 系、且 apt 在时成立。
    // 注意这里不含 fuseDevice —— /dev/fuse 缺不是 apt 能补的（那是内核 fuse 模块），
    // 真缺它时该提示"加载 fuse 模块"而不是装包。装包只解决 fusermount3。
    bool canInstallFuse() const { return !fuseMount && aptFamily && aptAvailable; }
bool readyForMdadm() const { return mdadm && targetCli && configfsMounted; }
    };

    explicit NasController(QObject *parent = nullptr);

    // ── 探测 ──
    Env probe() const;
    static QString nasrsBinaryPath();     // 打包/源码两种布局都找一遍

    // ── 命令生成（纯生成，不执行）──
    QVector<Step> planMdadmCreate(const Nas::Plan &plan, const QString &arrayName) const;
    QVector<Step> planRaid0RsCreate(const Nas::Plan &plan, const QString &volumeName,
                                    qint64 backingSizeBytes) const;
    // iSCSI 导出。走 targetcli 而不是手写 configfs，理由见 .cpp 顶部注释。
    // portalIp 空 → 不加 portals 行（targetcli 默认 0.0.0.0:3260，等于全网开放）
    // allowAnon   → 开 LIO 的 Demo Mode；任何能连上 3260 的主机都能读写
    QVector<Step> planIscsiExport(const QString &backingPath,
                                  const QString &iqn,
                                  const QString &portalIp,
                                  bool allowAnon) const;
    QVector<Step> planIscsiTeardown(const QString &iqn,
                                    const QString &backingPath) const;
    QVector<Step> planStop(const QString &deviceOrVolume) const;

// RAID FS：同一个 nasrs 引擎做 RAID0，但不导成 iSCSI，而是挂到 /mnt/<目录>，
// 让影视服务（Jellyfin / 电视墙）直接扫目录。用户要的正是这个：iSCSI 那条
// 走到块设备就结束了，影视服务拿不到文件。
//
// 诚实说明：这两条命令链在当前 nasrs 上**跑不通**。nasrs 现在只算布局、
// 打印容量就退出（退出码 3），没有任何导出通路 —— iSCSI 那条也没接
// targetcli。所以命令是给"引擎实现之后"用的，UI 必须同屏显示"未实现"，
// 不能让它看起来像点一下就能挂载。
QVector<Step> planRaidfsMount(const Nas::Plan &plan, const QString &mountPoint) const;
// 卸载。umount 对未挂载目标会失败，这一步本身就是那道检查。
QVector<Step> planRaidfsUnmount(const QString &mountPoint) const;
    // 一键装 fuse3：仅在 Env::canInstallFuse() 为真时由页面调用。
    QVector<Step> planInstallFuse() const;

// SMB：把单个分区路径作为共享导出。写入 /etc/samba/smb.d 下的独立片段，
// 不直接改主 smb.conf（便于撤销、也避免和发行版自带配置打架）。
// shareSection 由 Nas::makeShareSection 生成（已做 ini 转义）。
// 返回的命令**不含** smbpasswd：改用户密码永远是单独的手动步骤，
// 不混进这条链，免得一次确认顺带把用户 Samba 密码改了。
QVector<Step> planSmbExport(const QString &shareName,
                            const QString &shareSection) const;
// 撤销：删掉该片段并 reload。文件名按共享名推导，可预测、好核对。
QVector<Step> planSmbRemove(const QString &shareName) const;

    // ── 执行 ──
    // 按序执行；任一步失败立即停止并把 stderr 交给 stepFailed。
    // 调用方保证 steps 已被用户确认过。
    void run(const QVector<Step> &steps);
    void cancel();
    bool busy() const { return m_queueRunning; }

    // 只读查询（不改动任何东西）
    QStringList existingArrays() const;      // /proc/mdstat 里的 md*
    QString mdstatText() const;

signals:
    void stepStarted(int index, const QString &describe);
    void stepFinished(int index, bool ok, const QString &output);
    void allFinished(bool success);
    // 一步失败：index + stderr 摘要，页面负责显示与后续建议
    void stepFailed(int index, const QString &program, const QString &error);

private:
    void pumpQueue();
    // 工具名 → 绝对路径（PATH 缺 sbin 时的兜底，见 .cpp 的 findTool）
    QString resolvedProgram(const QString &name) const;

    QVector<Step> m_queue;
    int m_queueIndex = 0;
    bool m_queueRunning = false;
    bool m_cancelRequested = false;
    QProcess *m_process = nullptr;
};

#endif // NASCONTROLLER_H
