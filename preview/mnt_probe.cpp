#include "nasmodel.h"
#include <QCoreApplication>
#include <cstdio>

static int fails = 0, passes = 0;
static void check(bool ok, const QString &what)
{
    if (ok) { ++passes; printf("  OK   %s\n", qPrintable(what)); }
    else    { ++fails; printf("  !!   %s\n", qPrintable(what)); }
}
static void acc(const QString &p, const QString &why)
{ check(Nas::validateMountPoint(p).isEmpty(), why + " 应被接受 -> " + p); }
static void rej(const QString &p, const QString &why)
{ check(!Nas::validateMountPoint(p).isEmpty(), why + " 应被拒 -> " + p); }

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    printf("\n=== 接受 ===\n");
    acc("/mnt/raid", "直接子目录");
    acc("/mnt/media", "常见命名");
    acc("/mnt/raid0", "带数字");
    acc("/mnt/my.media", "带点");
    acc("/mnt/a_b-c", "带下划线短横");

    printf("\n=== 拒绝：范围锁在 /mnt 直接子目录 ===\n");
    rej("/mnt", "mnt 本身（会把别的挂载全遮住）");
    rej("/", "根");
    rej("/mnt/a/b", "两级子目录（超出约定范围）");
    rej("/data/raid", "不在 /mnt 下");
    rej("/home/cody/raid", "在 home 下");
    rej("/var/lib/raid", "在系统目录下");

    printf("\n=== 拒绝：规范化绕不过去 ===\n");
    rej("/mnt/../etc", "用 .. 跳出 /mnt");
    rej("/mnt/../../etc/passwd", "多层 ..");
    rej("/mnt//raid", "含 //");
    rej("/mnt/./raid", "含 .");
    rej("mnt/raid", "相对路径");
    rej(QString(), "空串");

    printf("\n=== 拒绝：会进 shell 与单元名的怪字符 ===\n");
    rej("/mnt/raid;rm -rf /", "分号注入");
    rej("/mnt/a b", "空格");
    rej("/mnt/-raid", "以短横开头（会被当命令选项）");
    rej("/mnt/.raid", "以点开头（隐藏目录）");
    rej("/mnt/raid$(id)", "命令替换");
    rej("/mnt/raid`id`", "反引号");

    printf("\n=== 默认挂载点 ===\n");
    check(Nas::defaultMountPoint("nas-data") == "/mnt/nas-data",
          QString("卷名保留短横（合法字符不该被改） -> ") + Nas::defaultMountPoint("nas-data"));
    check(Nas::defaultMountPoint("raid0") == "/mnt/raid0", "干净卷名直接拼");
    check(Nas::defaultMountPoint("///").isEmpty(), "全怪字符卷名返回空（不猜路径）");
    check(Nas::defaultMountPoint(QString()).isEmpty(), "空卷名返回空");
    // 默认值必须自己能过校验：UI 预填的就是它，预填非法路径等于埋雷
    const QString vols[] = { "nas0", "a.b", "x-y" };
    for (const QString &v : vols)
        check(Nas::validateMountPoint(Nas::defaultMountPoint(v)).isEmpty(),
              QString("默认值自带合法性 -> ") + Nas::defaultMountPoint(v));

    // 守住"清洗字符集 ⊆ 校验字符集"这个不变式：卷名再怪，只要函数返回了
    // 非空值，就必须能过校验。这是 defaultMountPoint 不做二次校验的前提。
    const QString weird[] = { "a", "A1", "a.b.c", "a-b_c", "9lives",
                              "nas-data", "raid/0", "..", "--x", "  ", "!!",
                              "名字", "a b", "x;rm", "tab\tsep" };
    for (const QString &v : weird) {
        const QString mp = Nas::defaultMountPoint(v);
        check(mp.isEmpty() || Nas::validateMountPoint(mp).isEmpty(),
              QString("卷名要么给不出挂载点、要么给出合法的：") +
              QString("[" + v + "] -> [" + mp + "]"));
    }

    printf("\n=== 冲突检测 ===\n");
    const QStringList sys = { "/mnt/e", "/mnt/d", "/", "/boot/efi" };
    check(Nas::mountPointConflict("/mnt/raid", sys).isEmpty(), "全新目录无冲突");
    check(!Nas::mountPointConflict("/mnt/e", sys).isEmpty(), "已挂载路径被拦");
    check(!Nas::mountPointConflict("/mnt/e/raid", sys).isEmpty(), "落在已挂载分区之内被拦");
    // 根盘不算冲突：/mnt/x 本来就是要新建的目录，所有路径都在根盘之内，
    // 拦它就等于一律拦。"挂到已有非空目录会遮内容"由 mountPointBusy 负责。
    check(Nas::mountPointConflict("/mnt/x", QStringList{ "/" }).isEmpty(),
          "根盘在列表时新建 /mnt/x 不误报");
    // 各守各的门：路径合法性由 validateMountPoint 把关，冲突由 conflict 把关。
    // "/" 在第一道就被拒，所以不该指望第二道也报 —— 断言要落在正确的门上。
    check(!Nas::validateMountPoint("/").isEmpty(), "想挂到 / 被第一道拦（路径合法性）");
    check(!Nas::mountPointConflict("/mnt/d", QStringList{ "/mnt/d" }).isEmpty(),
          "第二道仍能拦同名挂载");
    // mountPointBusy：非空目录要拦，隐藏文件也算内容
    check(Nas::mountPointBusy("/mnt/raid", QStringList{}).isEmpty(), "空目录/不存在都不拦");
    check(!Nas::mountPointBusy("/mnt/raid", QStringList{ "movies.mkv" }).isEmpty(), "有内容被拦");
    check(!Nas::mountPointBusy("/mnt/raid", QStringList{ ".cache" }).isEmpty(), "只有隐藏文件也算有内容");
    check(Nas::mountPointConflict("/mnt/raid", QStringList{}).isEmpty(), "空列表不误报");

    // ── aptInstallable：决定要不要摆一个会跑 root 命令的按钮 ──
    {
        const QString ubuntu = "PRETTY_NAME=\"Ubuntu 26.04.1 LTS\"\nNAME=\"Ubuntu\"\nID=ubuntu\nID_LIKE=debian\n";
        check(Nas::aptInstallable(ubuntu), "Ubuntu(ID=ubuntu) 判为可 apt");

        const QString debian = "ID=debian\n";
        check(Nas::aptInstallable(debian), "纯 Debian 判为可 apt");

        // ID_LIKE 里带 debian 但 ID 是别的派生发行版（如 linuxmint）
        const QString mint = "ID=linuxmint\nID_LIKE=\"ubuntu debian\"\n";
        check(Nas::aptInstallable(mint), "ID_LIKE 含 ubuntu 判为可 apt");

        // 关键负例：只在 PRETTY_NAME 里出现 Ubuntu，但 ID 不是 apt 系。
        // 整串 find 会误判，逐行解析不会。
        const QString trap = "PRETTY_NAME=\"Ubuntu-remix on Fedora\"\nID=fedora\nID_LIKE=fedora\n";
        check(!Nas::aptInstallable(trap), "PRETTY_NAME 提 Ubuntu 但 ID=fedora -> 否");

        const QString arch = "ID=arch\n";
        check(!Nas::aptInstallable(arch), "Arch 判为不可 apt");

        check(!Nas::aptInstallable(QString()), "空内容判为不可 apt");
        check(!Nas::aptInstallable("garbage with no keys\n"), "无 KEY 的行 -> 否");

        // 值带引号也要认
        check(Nas::aptInstallable("ID=\"ubuntu\"\n"), "ID 带引号仍认");
    }

    printf("\n   通过 %d 条，失败 %d 条\n", passes, fails);
    if (fails == 0) printf("OK 全部通过 (0 失败)\n");
    return fails ? 1 : 0;
}