#ifndef NASCONTROLLER_H
#define NASCONTROLLER_H

#include <QObject>
#include <QVector>
#include "nasmodel.h"

class QProcess;

//  ─────────────────────────────────────────────────────────────
//  NasController：把 Nas::Plan 变成现实。
//
//  三条路线，出口各不相同：
//
//    mdadm 路线    mdadm --create /dev/mdX → LIO block 后端 → iSCSI
//    mergerfs 路线 几个已挂载的分区 → mergerfs（第三方 FUSE）→ 一个目录
//                  出口只有 SMB 与"挂到 /mnt 下的目录"，没有块设备可导 iSCSI
//    nasrs 镜像    主副本目录 → nasrs mirror 单向拷 → 副本目录，出口只有 SMB
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

    // 一键安装的成品：页面只管展示 rows/commands、把 script 交给 sudo。
    // 之所以不复用 Step —— Step 那条队列走 pkexec 且一次一条，而装包需要
    // "一次授权、全程输出可见"，执行方式不同，硬套只会两边都别扭。
    struct InstallPlan
    {
        QString   pmName;      // 界面用：apt-get / dnf / pacman
        QStringList rows;      // 「缺的组件 → 要装的包」，一行一条，只用于展示
        QStringList packages;  // 真正要装的包名（全部来自 Nas::nasDepSpecs 常量表）
        QStringList blocked;   // 这家装不了的组件（人话名），只展示不执行
        QStringList commands;  // 会跑的几条命令，与 script 同源，展示用
        QString   script;      // 交给 sudo bash -c 的完整脚本
        bool valid() const { return !packages.isEmpty() && !script.isEmpty(); }
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
        bool nasrsBinary = false;     // 自研引擎可执行文件是否找到（镜像路线用）
        bool mergerfsBinary = false;  // 第三方 mergerfs 是否找到（RAID0 那格现在用它）
    // FUSE 走 mergerfs，与 iSCSI 那条完全不是一套依赖：不要并进 iSCSI 那批
    // 缺失项里，否则没装 fuse 的用户连 iSCSI 都会被提示"环境未就绪"。
    bool aptAvailable = false;    // apt-get 在不在（安装按钮的另一半条件）
    bool aptFamily  = false;    // 发行版是否 ubuntu/debian 系（os-release 判定）
    bool fuseDevice = false;      // /dev/fuse 在不在
    bool fuseMount  = false;      // mount.fuse / fusermount3 在不在
// SMB 相关：Samba 是可选功能，不能并进 RAID/iSCSI 那批缺失项里，
// 否则没装 Samba 的用户连 RAID 都会被提示环境未就绪。
    bool smbd       = false;      // smbd 在不在
bool testparm   = false;      // 能否离线校验 smb.conf
bool smbService = false;      // systemd 是否有 smbd.service
    // ── 一键安装（页顶那个通栏按钮）用 ──
    // 用哪家包管理器。None = 认不出 dnf/apt-get/pacman 里的任何一家，
    // 这时按钮点下去只会解释"为什么没法装"，不会去猜一个命令跑。
    Nas::Pkg pkg = Nas::Pkg::None;
    QStringList missingDeps;      // 缺的依赖，按探测用的可执行名（喂给包名表）
    QStringList missingDepLabels; // 同一批的人话名，界面直接显示
        QStringList missing;          // 人类可读的缺失项
        bool readyForSmb() const { return smbd && testparm && smbService; }
    // mergerfs 那条能不能跑：二进制 + /dev/fuse + fusermount3，三个都要。
    // 与 nasrs 无关（镜像路线不碰 FUSE，RAID0 那格也不碰 nasrs 了）。
    bool readyForMerger() const { return mergerfsBinary && fuseDevice && fuseMount; }
    // 能不能"一键装 fuse"：只在 fuse 助手还缺、且确实是 apt 系、且 apt 在时成立。
    // 注意这里不含 fuseDevice —— /dev/fuse 缺不是 apt 能补的（那是内核 fuse 模块），
    // 真缺它时该提示"加载 fuse 模块"而不是装包。装包只解决 fusermount3。
    bool canInstallFuse() const { return !fuseMount && aptFamily && aptAvailable; }
    // 一键安装该不该真去装：认得出包管理器、且确实有缺的依赖。
    // 「有缺」也算在这里，是为了让按钮点下去不会什么都没发生又不说明。
    bool hasMissingDeps() const { return !missingDeps.isEmpty(); }
    // 曾经有个 readyForMdadm() = mdadm && targetCli && configfsMounted，被
    // 「① 创建阵列」当门卫。它把**导出 iSCSI** 的前置条件（targetcli、configfs）
    // 算进了建阵列的头上，于是没挂 configfs 的机器连 RAID 都建不了，理由是句
    // 八竿子打不着的话。现在两个动作各查各的：建阵列看引擎在不在
    // （naspage.cpp::createArray），导出看 targetcli 在不在（exportIscsi）。
    };

    explicit NasController(QObject *parent = nullptr);

    // ── 探测 ──
    Env probe() const;
    static QString nasrsBinaryPath();     // 打包/源码两种布局都找一遍
    // mergerfs 是发行版装的第三方二进制，找法与 nasrs 同一套：程序旁边
    // （AppImage 里是 usr/bin）→ ../lib → /usr/local/bin → /usr/bin → PATH。
    // 与 nasrs 共用一份发现逻辑是错的：mergerfs 由包管理器装到系统目录，
    // 不会出现在 AppImage 的包内路径，而 nasrs 必须随包走。
    static QString mergerfsBinaryPath();

    // ── 命令生成（纯生成，不执行）──
    QVector<Step> planMdadmCreate(const Nas::Plan &plan, const QString &arrayName) const;
    // mergerfs：① 建挂载目录（/mnt/<卷名>），② 把几个 source 挂成那个目录。
    // 分两步是因为 ① 不挂任何东西（建目录可撤销），② 才会在目录上盖一个
    // 文件系统 —— 遮内容这件事必须走确认，不能在建目录时就顺手做掉。
    QVector<Step> planMergerfsCreate(const Nas::Plan &plan, const QString &mountPoint) const;
    QVector<Step> planMergerfsMount(const Nas::Plan &plan, const QString &mountPoint) const;
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
// planRaidfsMount 已经删掉：它是自研 RAID0 的"未实现导出"占位（nasrs 只算布局
// 就退出码 3）。现在挂载这一格走 planMergerfsMount，那条命令真能跑。
// 卸载。umount 对未挂载目标会失败，这一步本身就是那道检查。
QVector<Step> planRaidfsUnmount(const QString &mountPoint) const;

    // ── 软件镜像（nasrs RAID1）───────────────────────────────
    //
    // 与上面两条路线最要紧的差别：**除了建目录，全程不需要 root**。
    // 没有内核模块、没有 targetcli、没有 nbd，所以这条链是能真跑通的，
    // 不像 raidfs 那样只是把"引擎实现之后"的命令先摆在这儿。

    // ① 建两个副本目录：<挂载点>/<卷名>.nas-mirror，一主一备。
    // 提权只到 install -d 为止，并把属主给当前用户（-o/-g）：分区挂载点通常
    // 是 root 所有，不 chown 的话后面以普通用户同步时会撞上 EACCES，
    // 而那句报错看不出根因在目录属主上。
    QVector<Step> planMirrorCreate(const Nas::Plan &plan) const;

    // 一轮同步。**故意不套 pkexec**：镜像写的就是用户自己的目录，
    // 用 root 跑会把副本里的文件全变成 root 所有，回头 Samba 写不进去，
    // 还得再补一轮 chown —— 为省事提权，最后往往更麻烦。
    QVector<Step> planMirrorSync(const QString &primaryDir,
                                 const QString &secondaryDir,
                                 bool deleteExtras) const;

    // 状态查询（只读，与 existingArrays() 同类：不提权、不改任何东西）。
    struct MirrorStatus
    {
        bool parsed = false;        // JSON 是否读出来了（≠ 同步成功）
        bool ok = false;            // 引擎自己说这一轮没有失败
        quint64 srcFiles = 0, dstFiles = 0;
        quint64 lagFiles = 0, lagBytes = 0;         // 副本还差多少
        quint64 extraFiles = 0, extraBytes = 0;     // 副本上多出来的
        quint64 copiedFiles = 0, copiedBytes = 0;   // 本轮做了什么
        quint64 deleted = 0, failed = 0, torn = 0;  // torn = 拷贝期间源文件还在写
        quint64 remainingFiles = 0, remainingBytes = 0;
        QStringList messages;                        // 引擎带回来的逐条说明
        QString error;                               // 进程层/解析层的错，人话
        bool inSync() const { return parsed && lagFiles == 0; }
        QString summaryLine() const;                 // 一行话，界面直接显示
    };
    // 真跑一次 `nasrs mirror --report --json`。同步阻塞，最多 timeoutMs 毫秒 ——
    // 大目录树要扫盘，没有这道闸界面就会整个冻住（这是本类里唯一的阻塞调用）。
    MirrorStatus queryMirrorStatus(const QString &primaryDir,
                                   const QString &secondaryDir,
                                   int timeoutMs = 15000) const;
    // 解析单独抽出来当纯函数：探针可以不起进程、直接喂一段 JSON 验字段映射。
    static MirrorStatus parseMirrorStatus(const QByteArray &json);
    // 一键装 fuse3：仅在 Env::canInstallFuse() 为真时由页面调用。
    QVector<Step> planInstallFuse() const;

    // 页顶「一键安装依赖的组件」用。输入是刚探到的 Env（不再自己重探一次：
    // 用户在界面上看到的缺失项，必须和真去装的那批是同一批）。
    // 纯生成：不起进程、不碰网络，包名全部来自 Nas::nasDepSpecs() 常量表，
    // 再过一遍包名白名单，任何不合规矩的名字都会让整批作废而不是"清洗后照跑"。
    InstallPlan planInstallDeps(const Env &env) const;

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
