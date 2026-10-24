#ifndef VIRTUALBOX_H
#define VIRTUALBOX_H

#include <QString>
#include <QStringList>

#include "vmconfig.h"

// VirtualBox 虚拟机后端: 用 VBoxManage CLI 完成 创建/挂载/启动/停止/状态检测。
// 与 QEMU 后端并列, 由 VMPage 按 vm.vmType 分叉调用。
class VirtualBox
{
public:
    // 同步执行一条 VBoxManage 命令 (startvm 也会立即返回, 故同样适用)。
    // 成功返回 0; out 捕获 stdout+stderr。
    static int run(const QStringList &args, QString *out = nullptr);

    // 是否存在该 VM (已注册)。
    static bool vmExists(const QString &name);

    // 是否运行中 (State: running)。
    static bool isRunning(const QString &name);

    // 确保 VM 就绪: 不存在则创建并配置好控制器/磁盘/ISO; 存在则只回写核心设置。
    // 返回是否成功; err 携带失败原因 (供弹窗)。
    static bool prepare(const VMConfig &vm, QString *err);

    // 启动 VM. headless=true 用无头模式, 否则弹 VirtualBox 自带窗口。
    static bool start(const QString &name, bool headless);

    // 停止. force=true 用 poweroff (强制), 否则 acpowerdown (正常关机)。
    static bool stop(const QString &name, bool force);
};

#endif // VIRTUALBOX_H
