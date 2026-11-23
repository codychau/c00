//! nasrs：用户态 NAS 引擎。
//!
//! 分层：
//!   raid.rs    纯条带映射（无 IO，可单测）—— RAID0 那条路线
//!   nbd.rs     纯协议编解码（无 IO，可单测）—— RAID0 的导出层，尚未接入
//!   mirror.rs  纯镜像规划（无 IO，可单测）—— RAID1 镜像那条路线
//!   sync.rs    镜像的执行层：真的读目录、写文件的地方
//!   main.rs    参数解析 + 起进程
//!
//! 两条路线的成熟度**不同**，说清楚比含糊过去重要：
//!   RAID0 → 需要 NBD/loop + root 才能导出成块设备，本机无法端到端验证，
//!            所以 main.rs 里那条仍然只算布局就退出（退出码 3）。
//!   镜像   → 普通用户权限就能跑，`nasrs mirror` 是**真的能同步文件**的，
//!            不依赖任何内核模块。这也是它先给到用户的原因。

pub mod mirror;
pub mod nbd;
pub mod raid;
pub mod sync;
