// SMB 模型层离线断言：全是纯函数，重点是护栏是否真的挡住危险输入。
#include "nasmodel.h"
#include <QCoreApplication>
#include <cstdio>

static int fail = 0;
static void check(bool c, const QString &w)
{
    printf("  %s  %s\n", c ? "OK " : "!! ", qPrintable(w));
    if (!c) ++fail;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    printf("=== 共享名校验 ===\n");
    check(Nas::validateShareName("nas-data").isEmpty(), "nas-data 合法");
    check(!Nas::validateShareName("").isEmpty(), "空名拒绝");
    check(!Nas::validateShareName("IPC$").isEmpty(), "保留名 IPC$ 拒绝");
    check(!Nas::validateShareName("HOMES").isEmpty(), "保留名 HOMES 拒绝（大小写无关）");
    check(!Nas::validateShareName("homes").isEmpty(), "保留名 homes 拒绝（小写）");
    check(!Nas::validateShareName("my\\share").isEmpty(), "含反斜杠拒绝");
    check(!Nas::validateShareName("a/b").isEmpty(), "含斜杠拒绝");
    check(!Nas::validateShareName(QString(35, 'x')).isEmpty(), "超长拒绝");
    check(!Nas::validateShareName("secret$").isEmpty(), "以 $ 结尾拒绝");

    printf("\n=== 路径护栏 ===\n");
    QStringList sys = { "/", "/boot", "/home", "/boot/efi" };
    check(Nas::isRiskySharePath("/", sys), "共享根 / 判危险");
    check(Nas::isRiskySharePath("/home", sys), "共享 /home 判危险");
    check(Nas::isRiskySharePath("/home/cody", sys), "/home/cody 命中父挂载判危险");
    check(!Nas::isRiskySharePath("/mnt", sys), "非系统挂载 /mnt 不判危险（护栏不能过度）");
    check(!Nas::isRiskySharePath("/mnt/d", sys), "/mnt/d 不判危险");
    check(Nas::isRiskySharePath("", sys), "空路径判危险");
    // 反向：共享 / 会覆盖其下所有系统挂载（前面 / 已判危险，这里测父目录）
    // 过度报警也是 bug：护栏全红等于没护栏，所以非系统路径必须放行。
    // 之前 "/" 参与前缀匹配时会把所有绝对路径判危险，这条会失败。
    check(!Nas::isRiskySharePath("/mnt/d", QStringList{"/", "/home"}),
          "有 / 在系统挂载表里时，普通 /mnt/d 仍不判危险");
    check(Nas::isRiskySharePath("/home/cody/secrets", QStringList{"/home"}),
          "家目录子路径判危险");
    check(Nas::isRiskySharePath("/boot", QStringList{"/boot/efi"}),
          "共享 /boot 是其子挂载 efi 的父，判危险");

    printf("\n=== 路径合法性 ===\n");
    check(Nas::validateSharePath("/mnt/d").isEmpty(), "/mnt/d 合法");
    check(!Nas::validateSharePath("mnt/d").isEmpty(), "相对路径拒绝");
    check(!Nas::validateSharePath("/a;b").isEmpty(), "含分号拒绝");

    printf("\n=== share 段生成 ===\n");
    {
        Nas::SmbShare s;
        s.shareName = "nas"; s.path = "/mnt/d"; s.user = "cody";
        s.auth = Nas::SmbAuth::Password; s.writable = true;
        const QString sec = Nas::makeShareSection(s);
        check(sec.contains("[nas]"), "含段名 [nas]");
        check(sec.contains("path = /mnt/d"), "含 path");
        check(sec.contains("guest ok = no"), "Password 模式 guest ok = no");
        check(sec.contains("writable = yes"), "writable 透传");
        check(sec.contains("valid users = cody"), "valid users 写入");
    }
    {
        // GuestRO 即使误传 writable=true 也必须强制只读
        Nas::SmbShare s;
        s.shareName = "pub"; s.path = "/mnt/pub"; s.user = "nobody";
        s.auth = Nas::SmbAuth::GuestRO; s.writable = true;   // 故意的矛盾输入
        const QString sec = Nas::makeShareSection(s);
        check(sec.contains("read only = yes"), "GuestRO 强制只读");
        check(!sec.contains("writable = yes"), "GuestRO 不得出现 writable = yes");
        check(sec.contains("guest ok = yes"), "GuestRO guest ok = yes");
    }
    {
        // 注入：分号试图开启注释改写后面的配置
        Nas::SmbShare s;
        s.shareName = "x"; s.path = "/mnt/d; evil = true"; s.user = "cody";
        s.auth = Nas::SmbAuth::Password;
        const QString sec = Nas::makeShareSection(s);
        check(!sec.contains("path = /mnt/d; evil"),
              "路径分号被转义，未产生独立配置行");
    }

    printf("\n=== suggestShareName（默认值必须合法）===\n");
    {
        const QString n = Nas::suggestShareName("/mnt/d", "sda1");
        check(n == "d", QString("挂载点 /mnt/d 取末段 -> %1").arg(n));
        check(Nas::validateShareName(n).isEmpty(), "生成的默认共享名合法");
    }
    {
        // 无挂载点：退回分区名
        const QString n = Nas::suggestShareName("", "sdb1");
        check(n == "sdb1", QString("无挂载点退回分区名 -> %1").arg(n));
        check(Nas::validateShareName(n).isEmpty(), "退回名合法");
    }
    {
        // 命中保留名 HOMES 必须被规避
        const QString n = Nas::suggestShareName("/mnt/HOMES", "x");
        check(n.toUpper() != "HOMES", QString("保留名 HOMES 被改写 -> %1").arg(n));
        check(Nas::validateShareName(n).isEmpty(), "规避后仍合法");
    }
    {
        // 非法字符替换为 -，且首尾不留 -/.
        const QString n = Nas::suggestShareName("/mnt/a b/c*d", "x");
        check(!n.startsWith('-') && !n.endsWith('-') && !n.endsWith('.'),
              QString("首尾无 -/. -> %1").arg(n));
        check(Nas::validateShareName(n).isEmpty(), "脏输入生成名合法");
    }
    {
        // 以 $ 结尾会被 validateShareName 拒，suggest 必须加后缀规避
        const QString n = Nas::suggestShareName("/mnt/secret$", "x");
        check(Nas::validateShareName(n).isEmpty(),
              QString("以 $ 结尾的输入被规避 -> %1").arg(n));
    }
    {
        // 超长截到 31
        const QString n = Nas::suggestShareName(QString("/mnt/") + QString(50, 'x'), "x");
        check(n.size() <= 31, QString("超长被截断到 %1 字符").arg(n.size()));
    }

    printf("\n=== defaultSharePath ===\n");
    check(Nas::defaultSharePath("/mnt/d") == "/mnt/d", "已挂载返回挂载点");
    check(Nas::defaultSharePath("  ").isEmpty(), "未挂载返回空串（UI 据此提示）");

    printf("\n%s (%d 失败)\n", fail ? "!! 有失败" : "OK 全部通过", fail);
    return fail ? 1 : 0;
}
