//! nasrs —— 用户态 RAID0 over 文件，导出为块设备。
//!
//! 设计要点：**v1 不做后台守护**。
//! NBD 卷的生命周期等于本进程的生命周期。如果 nasrs 被 daemonize 或被
//! systemd 之外的方式托管，进程一挂内核的 nbd 设备不会立刻报错，而是让
//! 所有在它上面的文件系统卡死在 D 状态 —— 这比干脆起不来更糟。
//! 所以这里坚持"前台进程 + 调用方负责监督"，由 Qt 侧持有其生命周期。

use nasrs::mirror::{self, Plan};
use nasrs::raid::{self, Layout};
use nasrs::sync;
use std::fs::{File, OpenOptions};
use std::io;
use std::path::PathBuf;

/// 一个成员：打开的 backing 文件 + 容量
/// v1 只用 file 取 metadata 并持有 fd（保证文件不被别人换掉），
/// 真正的 pread/pwrite 要等 NBD 服务端接入才用 —— 这个字段不是死的，
/// 是还没接上。接上之前不要删。
#[allow(dead_code)]
struct Member {
    file: File,
    size: u64,
    path: PathBuf,
}

/// 打开并校验所有 backing 文件。
///
/// 严格要求**所有文件同尺寸**：RAID0 的映射依赖等长成员，
/// 若允许不等长，最后一个条带会写到短成员之外 —— 那是别人的文件，
/// 会静默毁掉分区上的其它数据。宁可拒绝启动。
fn open_members(paths: &[PathBuf], create_with_size: Option<u64>) -> io::Result<Vec<Member>> {
    if paths.len() < 2 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "RAID0 至少需要 2 个成员",
        ));
    }
    let mut out = Vec::with_capacity(paths.len());
    for p in paths {
        // 已存在就不 truncate：覆盖已有卷文件是危险操作，
        // 需要显式的 --force，默认只读写打开。
        let mut opts = OpenOptions::new();
        opts.read(true).write(true);
        if create_with_size.is_some() {
            opts.create_new(true);
        }
        let file = opts.open(p).map_err(|e| {
            io::Error::new(
                e.kind(),
                format!("打开 backing 文件失败 {}: {e}", p.display()),
            )
        })?;
        let size = file.metadata()?.len();
        if size == 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("backing 文件 {} 大小为 0，先预分配", p.display()),
            ));
        }
        out.push(Member { file, size, path: p.clone() });
    }

    let first = out[0].size;
    for m in &out[1..] {
        if m.size != first {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!(
                    "成员尺寸不一致：{} 是 {}，{} 是 {}。RAID0 要求等长成员，                     否则尾部条带会写到短成员之外。",
                    out[0].path.display(), first,
                    m.path.display(), m.size
                ),
            ));
        }
    }
    Ok(out)
}

/// 从成员算出卷布局
fn build_layout(member_size: u64, disks: usize, chunk_kib: u64) -> Result<Layout, raid::MapError> {
    Layout::new(chunk_kib * 1024, disks, member_size)
}

fn main() {
    // 解析参数：serve --name X --chunk 512K --disk f1 --disk f2 [--report]
    let argv: Vec<String> = std::env::args().collect();

    // 镜像路线（RAID1）与 RAID0 是两套参数，早分流而不是在同一个 while 里
    // 塞两组 match 分支：混在一起时"忘了给 --disk"这类误用会被另一套参数的
    // 默认值悄悄兜住，跑出一条谁也没打算执行的命令。
    if argv.get(1).map(|s| s.as_str()) == Some("mirror") {
        std::process::exit(cmd_mirror(&argv[2..]));
    }

    let mut chunk_kib: u64 = 512;
    let mut disks: Vec<PathBuf> = Vec::new();
    let mut name = String::from("nasrs0");
    let mut report_only = false;

    let mut i = 1;
    while i < argv.len() {
        match argv[i].as_str() {
            "serve" => {}
            "--name" => { i += 1; if let Some(v) = argv.get(i) { name = v.clone(); } }
            "--chunk" => {
                i += 1;
                if let Some(v) = argv.get(i) {
                    let t = v.trim_end_matches(|c: char| !c.is_ascii_digit());
                    let mult = if v.ends_with('K') || v.ends_with('k') { 1 }
                               else if v.ends_with('M') || v.ends_with('m') { 1024 }
                               else { 1 };
                    chunk_kib = t.parse::<u64>().unwrap_or(512) * mult;
                }
            }
            "--disk" => { i += 1; if let Some(v) = argv.get(i) { disks.push(PathBuf::from(v)); } }
            "--report" => report_only = true,
            other => {
                eprintln!("未知参数：{other}");
                std::process::exit(2);
            }
        }
        i += 1;
    }

    if disks.len() < 2 {
        eprintln!("需要至少两个 --disk，例如 --disk /data1/nas.img --disk /data2/nas.img");
        std::process::exit(2);
    }

    let members = match open_members(&disks, None) {
        Ok(m) => m,
        Err(e) => { eprintln!("错误：{e}"); std::process::exit(1); }
    };
    let layout = match build_layout(members[0].size, members.len(), chunk_kib) {
        Ok(l) => l,
        Err(e) => { eprintln!("布局不合法：{e:?}"); std::process::exit(1); }
    };

    println!("卷名      : {name}");
    println!("成员      : {}", members.len());
    for m in &members {
        println!("  {}  {}", m.path.display(), human(m.size));
    }
    println!("条带粒度  : {} KiB", layout.chunk / 1024);
    println!("卷容量    : {}", human(layout.capacity()));
    let slack = layout.slack_per_disk();
    if slack > 0 {
        // 说清楚零头为什么不算进容量：不是 bug，是"必须整条带"的代价
        println!("每盘零头  : {}（不足一个条带，不计入容量）", human(slack));
    }

    if report_only {
        return;
    }

    // v1 到这里就停：容量与映射都已算清并打印。
    // NBD 服务端需要 nbd 内核模块与 CAP_SYS_ADMIN，本机非 root 无法验证，
    // 所以不写"看起来能用但从没跑过"的绑定代码 —— 那会让人误以为已可用。
    eprintln!("\n尚未接入 NBD 服务端（需要 nbd 内核模块与 root，且必须实机验证）。");
    eprintln!("当前 --report 已给出布局与容量；映射逻辑在 raid.rs 有单元测试覆盖。");
    std::process::exit(3);
}

fn human(b: u64) -> String {
    const U: [&str; 5] = ["B", "KiB", "MiB", "GiB", "TiB"];
    let mut v = b as f64;
    let mut i = 0;
    while v >= 1024.0 && i < 4 { v /= 1024.0; i += 1; }
    format!("{v:.2} {}", U[i])
}

// ── 镜像路线（软件 RAID1）───────────────────────────────────
//
// 与 RAID0 那条的区别不是"又一个功能"，而是**今天就能用**：
// 两个目录、普通用户权限、不需要 nbd/loop/targetcli，也不需要 root。
// RAID0 + NBD + LIO 那条链还缺导出通路（见下面 serve 分支末尾的说明），
// 而"两份都存完整的、访问者只读其中一份"这件事已经完整可跑了 ——
// 所以界面把它单独做成一条路线，而不是画成 RAID0 的一个开关。

const MIRROR_USAGE: &str = "\
nasrs mirror —— 把主网目录完整镜像到副本目录（软件 RAID1，单向）

用法：
  nasrs mirror --primary 主目录 --secondary 副本目录 [选项]

选项：
  --report          只算不拷（默认）：打印还差多少文件、多少字节
  --once            真的执行一轮同步
  --json            结果压成一行 JSON 打到 stdout（给程序解析）
  --delete-extras   副本上「主网没有」的文件也删掉（默认不删）
  --help            本说明

方向永远是 主 → 副本，副本不是第二个写入口。两侧只要有任何不一致都以主网
为准，所以不比较谁新谁旧 —— 比较时间戳要信时钟，而时钟会被人改。

默认不删多余文件：主网误删的东西一同步就两份都没了，多留一份旧文件最多占空间。

不做什么：不跟符号链接、不镜像空目录、不解引用设备文件；单侧最多 200000 个文件。
定时由调用方负责（本程序保持前台单轮执行，不做后台守护）。
";

/// 一行 JSON 里的字符串转义。只用得上这两个：错误信息里可能出现 `"` 和 `\`，
/// 少了转义 Qt 那边 QJsonDocument 直接解析失败，界面就变成"同步状态读不出来"。
fn json_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 8);
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out
}

fn cmd_mirror(argv: &[String]) -> i32 {
    let mut primary: Option<PathBuf> = None;
    let mut secondary: Option<PathBuf> = None;
    let mut once = false;
    let mut json = false;
    let mut delete_extras = false;

    let mut i = 0;
    while i < argv.len() {
        let flag = argv[i].as_str();
        // 带值的两个选项单独一支：漏了值就直接退 2，而不是"当作没给"继续往下跑。
        // 之前写成返回 Option 的闭包再在 i32 函数里用 `?`，编译不过 ——
        // 真正的教训是这里不该有 Option：缺失就是要退出，不是要传播。
        match flag {
            "--primary" | "--secondary" => {
                let Some(v) = argv.get(i + 1) else {
                    eprintln!("缺少 {flag} 的参数");
                    return 2;
                };
                if flag == "--primary" { primary = Some(PathBuf::from(v)); }
                else { secondary = Some(PathBuf::from(v)); }
                i += 2;
                continue;
            }
            "--report" => once = false,
            "--once" => once = true,
            "--json" => json = true,
            "--delete-extras" => delete_extras = true,
            "--help" | "-h" => { print!("{MIRROR_USAGE}"); return 0; }
            other => {
                eprintln!("未知参数：{other}");
                eprint!("{MIRROR_USAGE}");
                return 2;
            }
        }
        i += 1;
    }

    let (Some(p), Some(s)) = (primary, secondary) else {
        eprintln!("必须同时给 --primary 和 --secondary。");
        eprint!("{MIRROR_USAGE}");
        return 2;
    };

    // 根目录组合先验，再枚举。顺序有讲究：嵌套/同目录这两种填法一旦开始
    // 枚举就是几十万次 stat，而且 --once 会真的往里写。
    let (proot, sroot) = match sync::check_roots(&p, &s) {
        Ok(v) => v,
        Err(e) => { eprintln!("错误：{}", e.describe()); return 1; }
    };
    let src = match sync::listing(&proot) {
        Ok(v) => v,
        Err(e) => { eprintln!("错误：{}", e.describe()); return 1; }
    };
    let dst = match sync::listing(&sroot) {
        Ok(v) => v,
        Err(e) => { eprintln!("错误：{}", e.describe()); return 1; }
    };
    let plan: Plan = match mirror::plan(&src, &dst, delete_extras) {
        Ok(v) => v,
        Err(e) => { eprintln!("错误：清单里有不安全或重复的路径：{e:?}"); return 1; }
    };

    let mut tally = sync::Tally::default();
    let mut errors: Vec<String> = Vec::new();

    if once {
        let quiet = json;
        let (t, errs) = sync::apply(&proot, &sroot, &plan, |msg| {
            if !quiet { println!("{msg}"); }
        });
        tally = t;
        errors = errs;
    }

    // 同步完再算一次"还差多少"没有意义（plan 是同一批判据的结论），
    // 所以这里照实说明：本轮之后副本与主网的差距就是 tally 里补上的那部分。
    let remaining_files = plan.copy_files.saturating_sub(tally.copied_files);
    let remaining_bytes = plan.copy_bytes.saturating_sub(tally.copied_bytes);

    if json {
        let errs: Vec<String> = errors.iter().take(20).map(|e| format!("\"{}\"", json_escape(e))).collect();
        println!(
            "{{\"ok\":{},\"primary\":\"{}\",\"secondary\":\"{}\",\
             \"src_files\":{},\"dst_files\":{},\
             \"lag_files\":{},\"lag_bytes\":{},\
             \"extra_files\":{},\"extra_bytes\":{},\
             \"copied_files\":{},\"copied_bytes\":{},\
             \"deleted\":{},\"failed\":{},\"torn\":{},\
             \"remaining_files\":{},\"remaining_bytes\":{},\
             \"errors\":[{}]}}",
            tally.failed == 0,
            json_escape(&proot.display().to_string()),
            json_escape(&sroot.display().to_string()),
            src.len(), dst.len(),
            plan.copy_files, plan.copy_bytes,
            plan.extra_files, plan.extra_bytes,
            tally.copied_files, tally.copied_bytes,
            tally.deleted, tally.failed, tally.changed_while_copying,
            remaining_files, remaining_bytes,
            errs.join(","),
        );
    } else {
        println!("主网      : {}（{} 个文件，{}）", proot.display(), src.len(), human(sum_sizes(&src)));
        println!("副本      : {}（{} 个文件，{}）", sroot.display(), dst.len(), human(sum_sizes(&dst)));
        if plan.copy_files == 0 {
            println!("落后      : 无 —— 副本与主网一致");
        } else {
            println!("落后      : {} 个文件，{}", plan.copy_files, human(plan.copy_bytes));
        }
        if plan.extra_files > 0 {
            // 说清"没删"是默认行为而不是 bug，否则用户会以为命令漏了功能
            println!(
                "副本多余  : {} 个文件，{}（{}）",
                plan.extra_files,
                human(plan.extra_bytes),
                if delete_extras { "本轮已删除" } else { "默认不删；要删用 --delete-extras" }
            );
        }
        if once {
            println!(
                "本轮      : 拷贝 {} 个 / {}，删除 {} 个，失败 {}",
                tally.copied_files, human(tally.copied_bytes), tally.deleted, tally.failed
            );
            if tally.changed_while_copying > 0 {
                println!(
                    "注意      : 其中 {} 个文件在拷贝期间还在被写，下一轮会再拷一次",
                    tally.changed_while_copying
                );
            }
            if remaining_files > 0 {
                println!("仍落后    : {} 个文件，{}", remaining_files, human(remaining_bytes));
            }
        } else {
            println!("动作      : 只报告，没有写入（要执行加 --once）");
        }
        for e in errors.iter().take(20) {
            eprintln!("  ! {e}");
        }
        if errors.len() > 20 {
            eprintln!("  ! 其余 {} 条错误未显示", errors.len() - 20);
        }
    }

    if tally.failed > 0 { 1 } else { 0 }
}

fn sum_sizes(v: &[mirror::Entry]) -> u64 {
    v.iter().map(|e| e.size).sum()
}
