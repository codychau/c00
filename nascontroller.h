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
        QStringList missing;          // 人类可读的缺失项
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
