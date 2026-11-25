//! nasrs —— 用户态软件镜像（RAID1）：把主网目录完整拷到副本目录。
//!
//! 设计要点：**不做后台守护**。一轮同步 = 一个进程，跑完就退出；
//! 定时（每 5 分钟）由调用方负责（Qt 页的 QTimer）。把本程序 daemonize
//! 会留下没人监督的写进程 —— 那比跑完退出危险得多。
//!
//! 原来的 RAID0 路线（raid.rs + nbd.rs + `serve` 子命令）已整条删除：导出层
//! 从来没有实现（算完布局就退出码 3），Qt 页那一格改用第三方 mergerfs
//! （FUSE 合并目录），不需要自研的块设备导出。

use nasrs::mirror::{self, Plan};
use nasrs::sync;
use std::path::PathBuf;

fn main() {
    let argv: Vec<String> = std::env::args().collect();
    // 第一个参数必须是 mirror。其它写法（含旧的 `serve`）一律退 2：
    // 这里不"兼容"旧参数，兼容会让一句已删除的用法悄悄继续存在。
    if argv.get(1).map(|s| s.as_str()) != Some("mirror") {
        eprint!("{MIRROR_USAGE}");
        eprintln!("\n本程序只做镜像：第一个参数必须是 mirror。");
        std::process::exit(2);
    }
    std::process::exit(cmd_mirror(&argv[2..]));
}

fn human(b: u64) -> String {
    const U: [&str; 5] = ["B", "KiB", "MiB", "GiB", "TiB"];
    let mut v = b as f64;
    let mut i = 0;
    while v >= 1024.0 && i < 4 { v /= 1024.0; i += 1; }
    format!("{v:.2} {}", U[i])
}

// ── 镜像（软件 RAID1）───────────────────────────────────────
//
// 这一条是普通用户权限就能跑通的：两个目录、不需要 nbd/loop/targetcli，
// 也不需要 root。"两份都存完整的、访问者只读其中一份"这件事已经完整可用，
// 所以界面把它单独做成一条路线。

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
