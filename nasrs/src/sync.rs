//! 镜像计划的**执行**：这一层才碰目录树。
//!
//! 分层与 raid/nbd 两个模块同一套路：mirror.rs 只算"差哪些文件"（纯函数、
//! 可单测），这里负责真的读目录、真的写文件。两件事放一起的坏处是"想验证
//! 判据就得准备一堆目录"，拆开之后判据在 CI 里零权限就能测。
//!
//! 落盘的可信度靠三件事，缺一个都会在最坏的那天露馅：
//!
//!   1) **临时文件 + rename**：先写 `.名字.nastmp`，成功后再 rename 成正式名。
//!      直接写正式名的话，断电或进程被杀会在副本里留下一个**半截文件**，
//!      而它的 size 与主网不同，下一轮同步会重新拷 —— 看起来能自愈，但在那之前
//!      访问者打开它拿到的是个坏文件。镜像的唯一意义就是"主盘坏了还能读副本"，
//!      副本里有半截文件等于没有副本。
//!   2) **拷完 fsync 再 rename**：顺序反了就只是把数据留在页缓存里换了个名字。
//!   3) **保留 mtime**：否则下一轮扫描"size 相同、mtime 不同"会把整棵树重拷一遍。
//!
//! 没做的一件事：**rename 之后不 fsync 父目录**。真断电时那次 rename 可能丢，
//! 表现为副本少了这个文件 —— 而不是内容错了一半。因为同步是幂等的，下一轮
//! 扫描发现副本没有这个文件就会再拷一次。用 FFI 打开目录再 fsync 换来的只是
//! "少重跑一轮"，不值得把多一处 unsafe 的代价换进来。
//!
//! 另一个边界：**空目录不镜像**。清单里只有文件，目录在落盘时按需创建。
//! 删掉多余文件后也**不删空目录** —— 空目录不占空间、不误导人，
//! 而"顺手删个看起来空的目录"万一判断错了就是别人的数据。

use crate::mirror::{Action, Plan};
use std::ffi::CString;
use std::fs::{File, Metadata, OpenOptions};
use std::io::{self, Read, Write};
use std::os::raw::{c_char, c_int};
use std::path::{Component, Path, PathBuf};
use std::time::UNIX_EPOCH;

/// 单侧最多枚举多少个文件。没有这道闸，一个误填成 `/` 或家目录的路径
/// 会把整台机器的文件都列进内存，然后开始往"副本"里拷。
pub const MAX_ENTRIES: usize = 200_000;
/// 最深递归层数，防符号链接成环（我们不解引用符号链接，但深度本身也要有界）
const MAX_DEPTH: usize = 64;
/// 临时文件后缀。以 `.` 开头是有意为之：枚举时整类跳过，
/// 于是"上次崩在半路留下的临时文件"不会被当成正经文件拷进副本。
const TMP_SUFFIX: &str = ".nastmp";

#[derive(Debug)]
pub enum SyncError {
    /// 两侧根目录的组合不可用（同一个目录、互相嵌套、不是目录）
    BadRoots(String),
    /// 枚举超限
    TooMany(String),
    /// 底层 IO 错误，带上下文
    Io(String, io::Error),
}

impl SyncError {
    pub fn describe(&self) -> String {
        match self {
            SyncError::BadRoots(s) => format!("目录组合不可用：{s}"),
            SyncError::TooMany(s) => s.clone(),
            SyncError::Io(ctx, e) => format!("{ctx}：{e}"),
        }
    }
}

impl From<(String, io::Error)> for SyncError {
    fn from(v: (String, io::Error)) -> Self {
        SyncError::Io(v.0, v.1)
    }
}

/// 校验并规范化两侧根目录。返回 (主网, 副本) 的绝对路径。
///
/// 必须挡掉三种组合：
///   · 同一个目录 —— "把主网镜像到主网"会把每个文件都当成副本，白写一倍，
///     而且 --delete-extras 时代码路径里两边同名，删哪份都可能。
///   · 互为嵌套 —— 副本在主网里面时，扫描会扫到自己刚写进去的副本，
///     下一轮又多一层，直到把盘写满。这是唯一一种会**自我放大**的错误配置。
///   · 不是目录 —— 让用户看到"不存在"，而不是拿到一串 ENOTDIR。
pub fn check_roots(primary: &Path, secondary: &Path) -> Result<(PathBuf, PathBuf), SyncError> {
    let p = canonical_dir(primary)?;
    let s = canonical_dir(secondary)?;
    if p == s {
        return Err(SyncError::BadRoots(format!(
            "两个副本是同一个目录 {}",
            p.display()
        )));
    }
    if starts_with(&s, &p) {
        return Err(SyncError::BadRoots(format!(
            "副本 {s} 在主网 {p} 里面，镜像会把副本再镜像进副本，一路撑满磁盘",
            s = s.display(),
            p = p.display()
        )));
    }
    if starts_with(&p, &s) {
        return Err(SyncError::BadRoots(format!(
            "主网 {p} 在副本 {s} 里面，同上",
            p = p.display(),
            s = s.display()
        )));
    }
    Ok((p, s))
}

/// `Path::starts_with` 的等价封装（对组件而非字符串前缀，避免 /mnt/ab 算 /mnt/a 的子目录）
fn starts_with(child: &Path, parent: &Path) -> bool {
    child.starts_with(parent)
}

fn canonical_dir(p: &Path) -> Result<PathBuf, SyncError> {
    let c = p
        .canonicalize()
        .map_err(|e| (format!("目录 {p:?} 打不开", p = p), e))?;
    if !c.is_dir() {
        return Err(SyncError::BadRoots(format!("{c:?} 不是目录")));
    }
    Ok(c)
}

/// 一次拷贝的统计
#[derive(Debug, Default, Clone, Copy)]
pub struct Tally {
    pub copied_files: u64,
    pub copied_bytes: u64,
    pub deleted: u64,
    /// 拷的过程中源文件又变了（正在被写入），下一轮会重拷
    pub changed_while_copying: u64,
    pub failed: u64,
}

/// 枚举一棵目录树，返回排序好的清单（mirror::plan 要的那种）。
///
/// 跳过：符号链接（不解引用，避免被指到 / 外面去）、FIFO/socket/设备文件、
/// 以及我们自己留下的 `.xxx.nastmp` 临时文件。
pub fn listing(root: &Path) -> Result<Vec<crate::mirror::Entry>, SyncError> {
    let mut out = Vec::new();
    let mut stack: Vec<(PathBuf, usize)> = vec![(root.to_path_buf(), 0)];

    while let Some((dir, depth)) = stack.pop() {
        let rd = std::fs::read_dir(&dir)
            .map_err(|e| (format!("读取目录 {} 失败", dir.display()), e))?;
        for de in rd {
            let de = de.map_err(|e| (format!("读取 {} 的目录项失败", dir.display()), e))?;
            let name = de.file_name();
            let name_str = name.to_string_lossy().to_string();
            if name_str.starts_with('.') && name_str.ends_with(TMP_SUFFIX) {
                continue; // 我们自己的半成品，不参与比对
            }
            let path = de.path();
            let ft = de.file_type().map_err(|e| {
                (format!("取 {} 的类型失败", path.display()), e)
            })?;
            // 目录先单独走一支再判"是不是普通文件"：合成一句
            // `if !ft.is_file() { continue }` 看着更短，但目录也被它 continue 掉了，
            // 于是递归在第一层就断掉 —— 实测只枚举到根目录那层的文件，
            // movies/ 底下的一个都不拷，而报告照样显示"已一致"。
            // 那种错法最难发现：目录浅的用法全都对，用户 movies/ 里的东西没副本。
            if ft.is_symlink() {
                continue;   // 不解引用：链接指向副本外面时，镜像就变成了往外面拷
            }
            if ft.is_dir() {
                if depth + 1 < MAX_DEPTH {
                    stack.push((path, depth + 1));
                }
                continue;
            }
            if !ft.is_file() {
                continue;   // FIFO / socket / 设备文件：读它们要么阻塞要么无意义
            }
            let md = de.metadata().map_err(|e| {
                (format!("取 {} 的属性失败", path.display()), e)
            })?;

            // 相对路径必须只由 Normal 组件构成；出现 .. / 绝对路径说明
            // 遍历逻辑错了，宁可报错也不要继续（见 mirror::is_safe_rel 的理由）
            let rel = match path.strip_prefix(root) {
                Ok(r) => r,
                Err(_) => {
                    return Err(SyncError::BadRoots(format!(
                        "{} 不在 {} 之下，遍历走偏了",
                        path.display(),
                        root.display()
                    )))
                }
            };
            let mut parts: Vec<&str> = Vec::new();
            for c in rel.components() {
                match c {
                    Component::Normal(s) => parts.push(s.to_str().unwrap_or_default()),
                    Component::CurDir => {}
                    _ => {
                        return Err(SyncError::BadRoots(format!(
                            "{} 含异常路径组件，拒绝继续",
                            path.display()
                        )))
                    }
                }
            }
            if parts.is_empty() {
                continue;
            }
            let rel_str = parts.join("/");
            if !crate::mirror::is_safe_rel(&rel_str) {
                return Err(SyncError::BadRoots(format!(
                    "{} 的相对路径不安全，拒绝继续",
                    path.display()
                )));
            }

            out.push(crate::mirror::Entry {
                rel: rel_str,
                size: md.len(),
                mtime: mtime_of(&md),
            });
            if out.len() > MAX_ENTRIES {
                return Err(SyncError::TooMany(format!(
                    "超过 {MAX_ENTRIES} 个文件，中止。请确认这两个目录填对了 —— \
                     填成家目录或 / 会走到这一步。"
                )));
            }
        }
    }

    out.sort_by(|a, b| a.rel.cmp(&b.rel));
    Ok(out)
}

/// 执行计划。`notify` 每处理一个文件回调一次（用于把进度写出去）。
pub fn apply(
    primary: &Path,
    secondary: &Path,
    plan: &Plan,
    mut notify: impl FnMut(&str),
) -> (Tally, Vec<String>) {
    let mut tally = Tally::default();
    let mut errors = Vec::new();

    for act in &plan.actions {
        match act {
            Action::Copy { rel, size } => {
                notify(&format!("拷贝 {rel}"));
                match copy_one(&primary.join(rel), &secondary.join(rel)) {
                    Ok(()) => {
                        tally.copied_files += 1;
                        tally.copied_bytes += size;
                    }
                    Err(CopyFail::Torn(msg)) => {
                        // 源文件正在被写：不算失败，副本这轮拿到的是接近最新的版本，
                        // 下一轮扫描会因为 size/mtime 变了而再拷一次。
                        tally.copied_files += 1;
                        tally.copied_bytes += size;
                        tally.changed_while_copying += 1;
                        errors.push(msg);
                    }
                    Err(CopyFail::Io(msg)) => {
                        tally.failed += 1;
                        errors.push(msg);
                    }
                }
            }
            Action::Delete { rel } => {
                notify(&format!("删除副本多余文件 {rel}"));
                let target = secondary.join(rel);
                match std::fs::remove_file(&target) {
                    Ok(()) => tally.deleted += 1,
                    Err(e) => {
                        tally.failed += 1;
                        errors.push(format!("删除 {} 失败：{e}", target.display()));
                    }
                }
            }
        }
    }

    (tally, errors)
}

enum CopyFail {
    /// 源文件在拷贝期间被改动（内容可能不完整），下一轮会重拷
    Torn(String),
    /// 真失败：权限、空间、路径问题
    Io(String),
}

fn copy_one(src: &Path, dst: &Path) -> Result<(), CopyFail> {
    let parent = dst
        .parent()
        .ok_or_else(|| CopyFail::Io(format!("{} 没有父目录", dst.display())))?;

    std::fs::create_dir_all(parent)
        .map_err(|e| CopyFail::Io(format!("创建目录 {} 失败：{e}", parent.display())))?;

    let file_name = dst
        .file_name()
        .ok_or_else(|| CopyFail::Io(format!("{} 没有文件名", dst.display())))?
        .to_string_lossy()
        .to_string();
    let tmp = parent.join(format!(".{file_name}{TMP_SUFFIX}"));
    // 上次崩在半路留下的同名临时文件：先清掉。我们要重写它，删掉自己
    // 上一次的临时产物不属于"删用户的文件"。
    if tmp.exists() {
        let _ = std::fs::remove_file(&tmp);
    }

    let mut inp =
        File::open(src).map_err(|e| CopyFail::Io(format!("打开源文件 {} 失败：{e}", src.display())))?;
    let src_mtime = mtime_of(
        &inp.metadata()
            .map_err(|e| CopyFail::Io(format!("读 {} 属性失败：{e}", src.display())))?,
    );

    let mut out = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&tmp)
        .map_err(|e| CopyFail::Io(format!("创建临时文件 {} 失败：{e}", tmp.display())))?;

    let mut buf = vec![0u8; 1 << 20];
    let mut written: u64 = 0;
    loop {
        let n = inp
            .read(&mut buf)
            .map_err(|e| CopyFail::Io(format!("读 {} 失败：{e}", src.display())))?;
        if n == 0 {
            break;
        }
        out.write_all(&buf[..n])
            .map_err(|e| CopyFail::Io(format!("写 {} 失败：{e}", tmp.display())))?;
        written += n as u64;
    }

    out.sync_all()
        .map_err(|e| CopyFail::Io(format!("落盘 {} 失败：{e}", tmp.display())))?;
    set_mtime(&tmp, src_mtime)
        .map_err(|e| CopyFail::Io(format!("设置 {} 的时间失败：{e}（不保留 mtime 会让下轮全量重拷，所以这里算失败）", tmp.display())))?;
    drop(out);

    std::fs::rename(&tmp, dst)
        .map_err(|e| CopyFail::Io(format!("改名 {} -> {} 失败：{e}", tmp.display(), dst.display())))?;

    // 事后核对：源文件若在拷贝期间被改，size 会对不上。
    // 报成 Torn 而不是静默：让界面能说"有 N 个文件当时正在被写，下轮重拷"。
    let now_src = std::fs::metadata(src).map(|m| m.len()).unwrap_or(u64::MAX);
    if written != now_src {
        return Err(CopyFail::Torn(format!(
            "{} 在拷贝期间被改动（拷了 {} 字节，源现在是 {} 字节），下一轮会重拷",
            src.display(),
            written,
            now_src
        )));
    }
    Ok(())
}

// ── 设置 mtime ──────────────────────────────────────────────
//
// 为什么手写 FFI 而不是加 filetime 这个 crate：本 crate 的依赖表是空的，
// 为了一次 utimensat 引入第三方，等于让"一次 cargo update"有机会改变同步
// 行为 —— 而这里改错的后果是副本内容不对。二十行 extern 声明是可以逐行
// 读过的，crates.io 的传递依赖树不是。
const UTIME_OMIT: i64 = (1 << 30) - 2; // 只改 mtime，atime 保持原样

#[repr(C)]
#[derive(Clone, Copy)]
struct TimeSpec {
    sec: i64,
    nsec: i64,
}

extern "C" {
    fn utimensat(dirfd: c_int, path: *const c_char, times: *const TimeSpec, flags: c_int) -> c_int;
}

fn set_mtime(path: &Path, mtime: i64) -> Result<(), io::Error> {
    let c = CString::new(path.as_os_str().as_encoded_bytes())
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "路径含 NUL"))?;
    let times = [
        TimeSpec { sec: 0, nsec: UTIME_OMIT },
        TimeSpec { sec: mtime, nsec: 0 },
    ];
    // AT_FDCWD = -100（Linux/x86_64 恒定）；传 -100 而不是从 libc crate 取，
    // 是因为本函数只在 Linux 上编译运行，且这个值从 2.6.22 起没变过。
    let rc = unsafe { utimensat(-100, c.as_ptr(), times.as_ptr(), 0) };
    if rc == 0 {
        Ok(())
    } else {
        Err(io::Error::last_os_error())
    }
}

/// 取 mtime（秒，UTC）。1970 年之前的时间 std 给的是负偏移，这里照负数处理，
/// 只要两侧口径一致就不影响比对（比对用的是"相等"，不是"谁新"）。
pub fn mtime_of(md: &Metadata) -> i64 {
    match md.modified() {
        Ok(t) => match t.duration_since(UNIX_EPOCH) {
            Ok(d) => d.as_secs() as i64,
            Err(e) => -(e.duration().as_secs() as i64),
        },
        Err(_) => 0,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU32, Ordering};

    static SEQ: AtomicU32 = AtomicU32::new(0);

    /// 真的在临时目录里搭一棵树。执行层不碰真盘就只能这样测；
    /// 纯函数那半边（mirror.rs）不需要这些。
    fn sandbox(tag: &str) -> PathBuf {
        let n = SEQ.fetch_add(1, Ordering::Relaxed);
        let p = std::env::temp_dir().join(format!("nasrs-sync-{}-{}-{}", std::process::id(), tag, n));
        let _ = std::fs::remove_dir_all(&p);
        std::fs::create_dir_all(&p).expect("建临时目录");
        p
    }

    fn put(root: &Path, rel: &str, content: &str) {
        let f = root.join(rel);
        std::fs::create_dir_all(f.parent().unwrap()).unwrap();
        std::fs::write(&f, content).unwrap();
    }

    /// 回归：枚举必须递归到子目录。
    /// 曾经写成 `if !ft.is_file() { continue }` 一句带走目录，结果只枚举到
    /// 根那一层，movies/ 里的文件一个都不拷，报告还说"已一致"。
    #[test]
    fn listing_recurses_into_subdirectories() {
        let root = sandbox("recur");
        put(&root, "top.txt", "a");
        put(&root, "movies/b.mkv", "bb");
        put(&root, "movies/sub/deep/c.srt", "ccc");

        let mut got: Vec<String> = listing(&root).unwrap().into_iter().map(|e| e.rel).collect();
        got.sort();
        assert_eq!(got, vec!["movies/b.mkv", "movies/sub/deep/c.srt", "top.txt"]);
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn listing_skips_symlinks_and_our_temps() {
        let root = sandbox("skip");
        put(&root, "real.txt", "x");
        #[cfg(unix)]
        std::os::unix::fs::symlink("/etc/hosts", root.join("link.txt")).unwrap();
        put(&root, ".real.txt.nastmp", "half");

        let got: Vec<String> = listing(&root).unwrap().into_iter().map(|e| e.rel).collect();
        assert_eq!(got, vec!["real.txt"], "符号链接和我们自己的临时文件都不该进清单");
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn roots_must_be_disjoint_and_not_nested() {
        let root = sandbox("roots");
        let a = root.join("a");
        let b = root.join("b");
        std::fs::create_dir_all(&a).unwrap();
        std::fs::create_dir_all(&b).unwrap();

        assert!(matches!(check_roots(&a, &a), Err(SyncError::BadRoots(_))));
        // 嵌套判断用的是 canonicalize 之后的**真实路径**，所以两个目录得先存在。
        // 不存在的那一侧在 canonicalize 那一步就报错（最后一条断言），轮不到
        // 嵌套判断 —— 界面这条链是先建目录再同步，不会走到"副本还不存在"。
        let nested = a.join("in");
        std::fs::create_dir_all(&nested).unwrap();
        assert!(matches!(check_roots(&a, &nested), Err(SyncError::BadRoots(_))), "副本在主网里面必须拒");
        assert!(matches!(check_roots(&a, &root), Err(SyncError::BadRoots(_))), "主网在副本里面同样要拒");
        assert!(check_roots(&a, &b).is_ok());
        assert!(matches!(check_roots(&root.join("nope"), &b), Err(SyncError::Io(..))));
        std::fs::remove_dir_all(&root).unwrap();
    }

    /// 执行层真正的产出：拷完之后两侧逐字节一致，且 mtime 保住了。
    /// mtime 不保的话下一轮会全量重拷 —— 这条和 smoke 脚本第 3 节是同一个断言，
    /// 放在这里是为了让 `cargo test` 也能挡住，不依赖有没有跑 shell 脚本。
    #[test]
    fn apply_makes_secondary_match_primary() {
        let root = sandbox("apply");
        let p = root.join("primary");
        let s = root.join("secondary");
        std::fs::create_dir_all(&p).unwrap();
        std::fs::create_dir_all(&s).unwrap();
        put(&p, "a.txt", "AAA");
        put(&p, "movies/sub/c.srt", "CCC");

        let (pr, sr) = check_roots(&p, &s).unwrap();
        let src = listing(&pr).unwrap();
        let dst = listing(&sr).unwrap();
        let plan = crate::mirror::plan(&src, &dst, false).unwrap();
        assert_eq!(plan.copy_files, 2);

        let (tally, errors) = apply(&pr, &sr, &plan, |_| {});
        assert_eq!(tally.failed, 0, "不该有失败：{errors:?}");
        assert!(errors.is_empty(), "{errors:?}");
        assert_eq!(tally.copied_bytes, 6);

        // 再来一轮：必须什么都不会做（这条就是 mtime 保留的证明）
        let src2 = listing(&pr).unwrap();
        let dst2 = listing(&sr).unwrap();
        let plan2 = crate::mirror::plan(&src2, &dst2, false).unwrap();
        assert!(plan2.in_sync(), "第二轮不该再有动作：{plan2:?}");

        assert_eq!(std::fs::read(sr.join("movies/sub/c.srt")).unwrap(), b"CCC");
        std::fs::remove_dir_all(&root).unwrap();
    }
}
