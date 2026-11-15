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

#endif // QEMUARGS_H
