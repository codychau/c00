#ifndef QEMUARGS_H
#define QEMUARGS_H

#include <QString>
#include <QStringList>

#include "vmconfig.h"

// ── QEMU 命令行参数生成 (纯逻辑, 不含二进制名, 便于静态测试) ──
// sockPath = QMP socket 路径
QStringList buildQemuArgs(const VMConfig &vm, const QString &sockPath);

// 归一 PCI BDF: 缺域时补 0000: 前缀; 非法返回空串
QString normalizeBDF(const QString &bdf);

// "vid:pid" (如 046d:c53f) → "vendorid=0x046d,productid=0xc53f"; 非法返回空串
QString usbHostArg(const QString &vidpid);

// ── systemd 服务化执行 (虚机作为系统服务跑, 不依赖桌面会话) ──
// 单元名: vm-<slug>.service (systemd 只接受 [a-z0-9-_.] 等)
QString serviceUnitName(const QString &vmName);
// 钩子脚本的落盘路径 (pre/post): /var/lib/ServerToolbox/vm-<slug>-pre.sh
QString serviceScriptPath(const QString &vmName, bool post);
// 服务模式下 QMP socket 的固定路径 (root 起的 QEMU 要能被普通用户程序连上)
QString serviceSocketPath(const QString &vmName);
// 生成单元文件内容. args 是 buildQemuArgs(vm, serviceSocketPath(...)) 的输出,
// uid = 桌面会话用户 (音频后端 pa/pipewire 需要它的 runtime dir)
QString buildSystemdUnit(const VMConfig &vm, const QStringList &args,
                         const QString &sockPath, int uid);

#endif // QEMUARGS_H
