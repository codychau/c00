//! 目录级镜像（软件 RAID1）的**规划**部分 —— 纯计算，不碰文件系统。
//!
//! 与 raid.rs 的关系：那一份算的是 RAID0 的条带落点（块级），这一份算的是
//! "两份目录副本之间还差哪些文件"（文件级）。两者都是纯函数，都只吃数字和
//! 字符串，所以都能在无特权、无磁盘权限的机器上跑单测。
//!
//! 为什么要有这一条路线：RAID0 + NBD + LIO 那条链走到今天仍然缺导出通路
//! （见 main.rs 末尾），而"两份都存一份完整的、访问者只读其中一份"这件事
//! **不需要 root、不需要内核模块、不需要 nbd**，纯用户态就能做完并且真的能跑。
//! 对家庭 NAS 来说它给的正是最常被要的那两样：一份坏了数据还在、能直接扫目录。
//!
//! 三条不变量（改动前请先读，它们决定了每个分支为什么长这样）：
//!
//!   1) **单向**：主网 → 副本。主网是唯一可写的一侧，副本永远不是"另一个入口"。
//!      因此两份只要有任何不一致，判据永远是"以主网为准"，不需要猜哪边新 ——
//!      猜新旧就得比较时钟，而两台机器的时钟、甚至同一台机器改时区之后的
//!      mtime，都不可信。
//!   2) **大小相同不代表内容相同**：所以 size 相同但 mtime 不同也照样重传。
//!      代价是"外部改过副本"会触发一次多余复制，这是有意的 —— 宁可多拷一次，
//!      也不要留下一份看起来同步、实际内容不同的副本，那种副本在真正需要它
//!      的那天会让人吃到最贵的教训。
//!   3) **删除是可选的、默认关**：主网删了个文件，默认**不**跟着删副本。
//!      误删同步过去就两份都没了；多留一份旧文件最多占点空间。要真同步删除
//!      得显式传 delete_extras。

use std::collections::BTreeMap;

/// 参与比对的一个普通文件（目录不单独成条目，它在落盘时按需创建）
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Entry {
    /// 相对根的相对路径，统一用 '/' 分隔，形如 `a/b.mkv`
    pub rel: String,
    pub size: u64,
    /// 修改时间（秒，UTC）。来自文件系统，不做任何换算。
    pub mtime: i64,
}

/// 一条待执行的同步动作
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Action {
    /// 把主网的这个文件完整拷到副本（覆盖同名旧文件）
    Copy { rel: String, size: u64 },
    /// 删掉副本上这个多余的文件（仅在 delete_extras 时产生）
    Delete { rel: String },
}

/// 一轮同步的完整计划。数字是给用户看的，动作是给执行器用的。
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Plan {
    /// 按 rel 升序，保证同一输入永远得到同一份计划（可复现、可比对）
    pub actions: Vec<Action>,
    /// 需要拷贝的文件数
    pub copy_files: u64,
    /// 需要拷贝的字节数
    pub copy_bytes: u64,
    /// 副本上有、主网没有的文件数。**无论 delete_extras 与否都统计**，
    /// 因为"多出来的东西"本身就是要告诉用户的信息。
    pub extra_files: u64,
    pub extra_bytes: u64,
}

impl Plan {
    /// 副本是否已与主网一致（按当前 delete_extras 语义）
    pub fn in_sync(&self) -> bool {
        self.actions.is_empty()
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PlanError {
    /// 相对路径不安全（绝对路径、`..`、空段、控制字符）
    UnsafePath(String),
    /// 同一侧出现重复的相对路径。正常走目录树遍历不可能出现，
    /// 出现说明调用方给的清单本身有问题，那就整批作废而不是"挑一个用"。
    DuplicatePath(String),
}

/// 相对路径是否安全。
///
/// 这个判断直接决定执行器会不会把文件写到副本目录**外面**去：
/// `../escape` 这种路径一旦进了一次 `join`，同步就能写到父目录，
/// 而 `--delete-extras` 又是删除操作 —— 两者叠加就是"往别人的目录里删东西"。
/// 所以宁可严：只允许普通字符，段不能空，不能是 `.` 或 `..`。
pub fn is_safe_rel(rel: &str) -> bool {
    if rel.is_empty() || rel.len() > 4096 {
        return false;
    }
    if rel.starts_with('/') || rel.contains('\\') {
        return false;
    }
    if rel.chars().any(|c| (c as u32) < 0x20 || c == '\x7f') {
        return false;
    }
    rel.split('/').all(|seg| {
        !seg.is_empty() && seg != "." && seg != ".."
    })
}

/// 把两侧的清单合成一份动作列表。
///
/// `delete_extras = false` 时副本上多出来的文件只在统计里出现、不产生动作。
pub fn plan(src: &[Entry], dst: &[Entry], delete_extras: bool) -> Result<Plan, PlanError> {
    let src_map = index(src)?;
    let dst_map = index(dst)?;

    let mut p = Plan::default();

    // BTreeMap 的迭代顺序就是 rel 的字典序，所以动作列表天然有序，
    // 不需要再 sort —— 但也不要依赖这个"免费"：契约是 Plan.actions 有序，
    // tests::actions_are_sorted 会守住它，将来换成 HashMap 也不会漏。
    for (rel, s) in &src_map {
        match dst_map.get(rel) {
            None => {
                p.actions.push(Action::Copy { rel: rel.to_string(), size: s.size });
                p.copy_files += 1;
                p.copy_bytes += s.size;
            }
            Some(d) => {
                // 见文件头不变量 2：size 相同但 mtime 不同也重传。
                if d.size != s.size || d.mtime != s.mtime {
                    p.actions.push(Action::Copy { rel: rel.to_string(), size: s.size });
                    p.copy_files += 1;
                    p.copy_bytes += s.size;
                }
            }
        }
    }

    for (rel, d) in &dst_map {
        if src_map.contains_key(rel) {
            continue;
        }
        p.extra_files += 1;
        p.extra_bytes += d.size;
        if delete_extras {
            p.actions.push(Action::Delete { rel: rel.to_string() });
        }
    }

    p.actions.sort_by(|a, b| rel_of(a).cmp(rel_of(b)));
    Ok(p)
}

fn rel_of(a: &Action) -> &str {
    match a {
        Action::Copy { rel, .. } => rel,
        Action::Delete { rel } => rel,
    }
}

fn index(items: &[Entry]) -> Result<BTreeMap<&str, &Entry>, PlanError> {
    let mut m = BTreeMap::new();
    for e in items {
        if !is_safe_rel(&e.rel) {
            return Err(PlanError::UnsafePath(e.rel.clone()));
        }
        if m.insert(e.rel.as_str(), e).is_some() {
            return Err(PlanError::DuplicatePath(e.rel.clone()));
        }
    }
    Ok(m)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn e(rel: &str, size: u64, mtime: i64) -> Entry {
        Entry { rel: rel.into(), size, mtime }
    }

    fn copies(p: &Plan) -> Vec<String> {
        p.actions.iter().filter_map(|a| match a {
            Action::Copy { rel, .. } => Some(rel.clone()),
            Action::Delete { .. } => None,
        }).collect()
    }

    fn deletes(p: &Plan) -> Vec<String> {
        p.actions.iter().filter_map(|a| match a {
            Action::Delete { rel } => Some(rel.clone()),
            Action::Copy { .. } => None,
        }).collect()
    }

    #[test]
    fn new_file_is_copied() {
        let p = plan(&[e("a.mkv", 10, 100)], &[], false).unwrap();
        assert_eq!(copies(&p), vec!["a.mkv"]);
        assert_eq!(p.copy_files, 1);
        assert_eq!(p.copy_bytes, 10);
        assert!(!p.in_sync());
    }

    #[test]
    fn identical_tree_needs_nothing() {
        let a = [e("x/y.mkv", 10, 100), e("z.mkv", 20, 200)];
        let b = [e("z.mkv", 20, 200), e("x/y.mkv", 10, 100)]; // 顺序不同也要认得出一致
        let p = plan(&a, &b, false).unwrap();
        assert!(p.in_sync(), "同一批文件换个顺序不该产生任何动作");
        assert_eq!(p.copy_bytes, 0);
    }

    #[test]
    fn size_change_is_recopied() {
        let p = plan(&[e("a.mkv", 11, 100)], &[e("a.mkv", 10, 100)], false).unwrap();
        assert_eq!(copies(&p), vec!["a.mkv"]);
    }

    /// 不变量 2：等长但 mtime 不同，照样重传。
    /// 若有人"优化"成只看 size，这条会红。
    #[test]
    fn same_size_different_mtime_is_recopied() {
        let p = plan(&[e("a.mkv", 10, 200)], &[e("a.mkv", 10, 100)], false).unwrap();
        assert_eq!(copies(&p), vec!["a.mkv"], "mtime 不同就不能当作已同步");
    }

    #[test]
    fn extras_are_reported_but_not_deleted_by_default() {
        let p = plan(&[], &[e("old.mkv", 5, 1)], false).unwrap();
        assert!(p.actions.is_empty(), "默认绝不删副本上的文件");
        assert_eq!(p.extra_files, 1);
        assert_eq!(p.extra_bytes, 5);
    }

    #[test]
    fn extras_deleted_only_when_asked() {
        let p = plan(&[], &[e("old.mkv", 5, 1)], true).unwrap();
        assert_eq!(deletes(&p), vec!["old.mkv"]);
        // 统计照样要给：界面要能说出"删了 1 个 / 5 字节"
        assert_eq!(p.extra_files, 1);
    }

    #[test]
    fn unsafe_paths_reject_the_whole_batch() {
        for bad in ["../escape", "/abs", "a/../b", "./a", "a//b", "a/./b", ""] {
            let one = [e("ok.mkv", 1, 1), e(bad, 1, 1)];
            assert_eq!(
                plan(&one, &[], false),
                Err(PlanError::UnsafePath(bad.into())),
                "不安全路径 {bad:?} 必须让整批作废"
            );
        }
        assert!(!is_safe_rel("a\\b"));   // 反斜杠：Windows 分享路径的常见形状
        assert!(is_safe_rel("a/b-c_1.0.mkv"));
    }

    #[test]
    fn duplicate_entry_is_an_error() {
        let dup = [e("a.mkv", 1, 1), e("a.mkv", 2, 2)];
        assert_eq!(plan(&dup, &[], false), Err(PlanError::DuplicatePath("a.mkv".into())));
    }

    #[test]
    fn actions_are_sorted_by_rel() {
        // 执行器可以依赖这个顺序（同目录的文件连续处理，创建父目录更省），
        // 所以它是契约，不是巧合。
        let src = [e("b.mkv", 1, 1), e("a.mkv", 1, 1), e("c.mkv", 1, 1)];
        let p = plan(&src, &[], true).unwrap();
        let mut rels: Vec<&str> = p.actions.iter().map(rel_of).collect();
        rels.sort();
        let got: Vec<&str> = p.actions.iter().map(rel_of).collect();
        assert_eq!(got, rels);
    }

    #[test]
    fn totals_match_the_actions() {
        let src = [e("a", 100, 1), e("b", 200, 1), e("c", 50, 1)];
        let p = plan(&src, &[e("b", 200, 1)], false).unwrap();
        assert_eq!(p.copy_files, 2);
        assert_eq!(p.copy_bytes, 150);
        let sum: u64 = p.actions.iter().map(|a| match a {
            Action::Copy { size, .. } => *size,
            Action::Delete { .. } => 0,
        }).sum();
        assert_eq!(sum, p.copy_bytes, "汇总数必须与动作列表自洽");
    }

    #[test]
    fn empty_on_both_sides_is_in_sync() {
        let p = plan(&[], &[], true).unwrap();
        assert!(p.in_sync());
        assert_eq!(p.extra_files, 0);
    }
}
