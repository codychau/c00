//! nasrs：用户态 RAID0 over 文件，通过 NBD 暴露为块设备，再由 LIO 导出 iSCSI。
//!
//! 分层：
//!   raid.rs  纯条带映射（无 IO，可单测）
//!   nbd.rs   纯协议编解码（无 IO，可单测）
//!   main.rs  唯一碰文件与 socket 的地方

pub mod nbd;
pub mod raid;
