//! nasrs —— 用户态 RAID0 over 文件，导出为块设备。
//!
//! 设计要点：**v1 不做后台守护**。
//! NBD 卷的生命周期等于本进程的生命周期。如果 nasrs 被 daemonize 或被
//! systemd 之外的方式托管，进程一挂内核的 nbd 设备不会立刻报错，而是让
//! 所有在它上面的文件系统卡死在 D 状态 —— 这比干脆起不来更糟。
//! 所以这里坚持"前台进程 + 调用方负责监督"，由 Qt 侧持有其生命周期。

use nasrs::raid::{self, Layout};
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
