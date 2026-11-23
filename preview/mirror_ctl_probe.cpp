// NasController 里镜像那三条命令的断言：生成什么、不给什么。
//
// 为什么单独测控制器而不是只看界面：界面显示的是"同步到副本"这句人话，
// 真正决定后果的是 argv 里那几个 token。两条最贵的规矩都在这一层：
//   · 同步**不带 pkexec** —— 用 root 跑会把副本里的文件全变成 root 所有，
//     回头 Samba 写不进去，还得再补一轮 chown。
//   · 副本目录必须叫 <卷名>.nas-mirror —— 写成挂载点本身时，一旦带上
//     --delete-extras，删的是那个分区上原有的东西。形状校验挡不住这种，
//     因为只有拿着卷名才判得出来。
#include "nascontroller.h"
#include "nasmodel.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <cstdio>
#include <QString>
#include <QStringList>

static int g_pass = 0, g_fail = 0;
static void check(bool c, const QString &w)
{
    if (c) { ++g_pass; fprintf(stderr, "  ✓ %s\n", qPrintable(w)); }
    else   { ++g_fail; fprintf(stderr, "  ✗ %s\n", qPrintable(w)); }
    fflush(stderr);
}

static Nas::Plan mirrorPlan(const QString &vol, const QString &d1, const QString &d2)
{
    Nas::Plan p;
    p.engine = Nas::Engine::MirrorRs;
    p.volumeName = vol;
    auto add = [&p](const QString &dev, const QString &dir) {
        Nas::Member m;
        m.kind = Nas::MemberKind::MirrorDir;
        m.device = dev;
        m.filePath = dir;
        m.capacity = 1LL << 40;
        p.members << m;
    };
    add("/dev/vdb1", d1);
    add("/dev/vdc1", d2);
    return p;
}

int main(int argc, char **argv)
{
    // 必须有个 QCoreApplication：nasrsBinaryPath() 走 applicationDirPath()，
    // 没有实例时它返回空串并打一句警告 —— 表现是"引擎找不到"，
    // 而真正的原因是探针没建应用对象（这个坑踩过，写在这免得再踩）。
    QCoreApplication app(argc, argv);
    // 这台机器上的探针由镜像里的 ld-linux 拉起，/proc/self/exe 于是指向加载器，
    // applicationDirPath() 回的是加载器目录 —— "引擎在程序旁边"那条找不到，
    // 表现成"nasrs 不在"。所以跑探针时把 nasrs 放到 PATH 上（见 build 脚本），
    // 让 nasrsBinaryPath() 走它本来就支持的第三条发现路径。
    // 这不是放宽判据：探针照样要求"引擎必须找得到"，只是换成 PATH 这种方式给它。
    NasController ctl;

    fprintf(stderr, "== planMirrorCreate ==\n");
    {
        const auto steps = ctl.planMirrorCreate(mirrorPlan("nas",
            "/mnt/d1/nas.nas-mirror", "/mnt/d2/nas.nas-mirror"));
        check(steps.size() == 2, "两个成员 → 两条建目录命令");
        bool allPkexec = true, allInstall = true, hasOwner = false, anyDestructive = false;
        for (const auto &s : steps) {
            if (s.program != "pkexec") allPkexec = false;
            if (!s.args.contains("install")) allInstall = false;
            if (s.args.contains("-o") && s.args.contains("-g")) hasOwner = true;
            if (s.destructive) anyDestructive = true;
        }
        check(allPkexec, "建目录走 pkexec：挂载点通常是 root 所有，这是唯一需要提权的一步");
        check(allInstall, "用 install -d（目录已存在不报错，也不碰里面已有的文件）");
        check(hasOwner, "带 -o/-g 把属主给当前用户，否则后面普通用户同步会撞 EACCES");
        check(!anyDestructive, "建目录不是破坏性操作，不该要求手输卷名那级确认");
    }
    {
        // 副本目录写成了挂载点本身：形状是合法的（两层、字符合法），
        // 只有"必须以 .<卷名>.nas-mirror 结尾"这条能挡住
        const auto steps = ctl.planMirrorCreate(mirrorPlan("nas", "/mnt/d1", "/mnt/d2"));
        check(steps.isEmpty(), "镜像目录 = 挂载点本身 → 整批拒绝生成");
        const auto s2 = ctl.planMirrorCreate(mirrorPlan("nas", "/mnt/d1/other", "/mnt/d2/other"));
        check(s2.isEmpty(), "目录名不是 <卷名>.nas-mirror → 拒绝");
        const auto s3 = ctl.planMirrorCreate(mirrorPlan("nas", "/mnt/d1/nas.nas-mirror",
                                                        "/mnt/d1/nas.nas-mirror"));
        check(s3.isEmpty(), "两个目录一模一样 → 拒绝（nasrs 自己也会拒，但别等确认之后）");
        const auto s4 = ctl.planMirrorCreate(mirrorPlan("nas/../x", "/mnt/d1/x.nas-mirror",
                                                        "/mnt/d2/x.nas-mirror"));
        check(s4.isEmpty(), "卷名不合规 → 拒绝");
    }

    fprintf(stderr, "== planMirrorSync ==\n");
    {
        const auto steps = ctl.planMirrorSync("/mnt/d1/nas.nas-mirror",
                                              "/mnt/d2/nas.nas-mirror", false);
        check(steps.size() == 1, "同步就一条命令");
        if (!steps.isEmpty()) {
            const NasController::Step &s = steps.first();
            check(s.program != "pkexec", "同步**不**走 pkexec（提权只会把副本变成 root 所有）");
            check(s.program.endsWith("/nasrs"), QString("程序是 nasrs 本体：%1").arg(s.program));
            check(s.args.contains("mirror") && s.args.contains("--once"), "参数含 mirror 与 --once");
            check(s.args.contains("--primary") && s.args.contains("--secondary"),
                  "方向显式给出：--primary 在前、--secondary 在后");
            check(!s.args.contains("--delete-extras"), "默认不带 --delete-extras");
            check(!s.destructive, "只增不删不算破坏性，不该弹手输确认");
            const int iP = s.args.indexOf("--primary"), iS = s.args.indexOf("--secondary");
            check(s.args.value(iP + 1) == "/mnt/d1/nas.nas-mirror"
                  && s.args.value(iS + 1) == "/mnt/d2/nas.nas-mirror",
                  "路径原样进 argv（不是拼进一个 shell 字符串）");
        }
        const auto del = ctl.planMirrorSync("/mnt/d1/nas.nas-mirror",
                                            "/mnt/d2/nas.nas-mirror", true);
        check(!del.isEmpty() && del.first().args.contains("--delete-extras"),
              "勾了删除才带上这个开关");
        check(!del.isEmpty() && del.first().destructive,
              "带删除的同步标成破坏性 → 会走逐条确认 + 手输卷名");
        // 注意这里**不**断言"/mnt/d1 会被拒"：形状上它是合法的（两层、字符干净）。
        // "恰好等于某个挂载点"那一拦要靠真实挂载表，是 mirrorDirConflict 的活，
        // 由页面在建/同步之前查（见 NasPage::mirrorDirs）。控制器拿不到挂载表，
        // 在这里假装能挡反而是在骗自己。
        check(ctl.planMirrorSync("/mnt", "/mnt/d2", false).isEmpty(),
              "只有一层的目录（/mnt）→ 不生成");
        check(ctl.planMirrorSync("/etc/x.nas-mirror", "/mnt/d2/x.nas-mirror", false).isEmpty(),
              "系统目录下的镜像目录 → 不生成");
        check(ctl.planMirrorSync("/mnt/d1/x", "/mnt/d1/x", false).isEmpty(),
              "两个目录相同 → 不生成");
    }

    fprintf(stderr, "== parseMirrorStatus ==\n");
    {
        const QByteArray j = R"({"ok":true,"primary":"/mnt/d1/nas.nas-mirror",
            "secondary":"/mnt/d2/nas.nas-mirror","src_files":128,"dst_files":125,
            "lag_files":3,"lag_bytes":13002342,"extra_files":1,"extra_bytes":10,
            "copied_files":0,"copied_bytes":0,"deleted":0,"failed":0,"torn":0,
            "remaining_files":0,"remaining_bytes":0,"errors":[]})";
        const auto st = NasController::parseMirrorStatus(j);
        check(st.parsed && st.ok, "正常 JSON 解析成功");
        check(st.lagFiles == 3 && st.lagBytes == 13002342, "落后 3 个 / 12.4 MiB 读对了");
        check(st.extraFiles == 1, "副本多余 1 个读对了");
        check(!st.inSync(), "有落后就不算一致");
        check(st.summaryLine().contains("3 个文件") && st.summaryLine().contains("MiB"),
              QString("一句话摘要给人看：%1").arg(st.summaryLine()));

        const auto bad = NasController::parseMirrorStatus("not json at all");
        check(!bad.parsed, "非 JSON（引擎没输出）→ parsed=false");
        check(!bad.inSync(), "解析失败时绝不该报「已一致」");
        check(bad.summaryLine().contains("读不到"),
              QString("解析失败也要有一句人话：%1").arg(bad.summaryLine()));

        const auto failed = NasController::parseMirrorStatus(
            R"({"ok":false,"failed":2,"lag_files":0,"extra_files":0,"errors":["打开源文件失败"]})");
        check(failed.parsed && !failed.ok, "ok=false 要能读出来");
        check(failed.messages.size() == 1, "引擎带回来的逐条说明进了 messages");
        check(failed.summaryLine().contains("失败"), "失败数出现在摘要里");
    }

    fprintf(stderr, "\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
