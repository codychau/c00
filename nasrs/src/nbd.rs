//! NBD（Network Block Device）固定头编解码。
//!
//! 只用 NBD 的老式固定头协议（不协商 newstyle 扩展），因为对端是
//! Linux 内核的 nbd 驱动，能协商是加分项、不是必需项。
//!
//! 这里**只做编解码**， socket 收发在 main.rs，为的是把协议数学隔离出来单测。

/// 请求 magic：客户端 → 服务端
pub const REQUEST_MAGIC: u32 = 0x2560_9513;
/// 应答 magic：服务端 → 客户端
pub const REPLY_MAGIC: u32 = 0x2560_9514;

/// 客户端握手完成后服务端要发的简单应答大小（旧式协议：16 字节）
pub const SIMPLE_REPLY_SIZE: usize = 16;
/// 固定请求头长度
pub const REQUEST_HEADER_SIZE: usize = 28;
/// 固定应答头长度
pub const REPLY_HEADER_SIZE: usize = 20;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Command {
    Read,
    Write,
    Discard,
    Flush,
    Trim,
    Other(u32),
}

impl Command {
    pub fn from_u32(v: u32) -> Self {
        match v {
            0 => Command::Read,
            1 => Command::Write,
            2 => Command::Discard,
            3 => Command::Flush,
            5 => Command::Trim,
            other => Command::Other(other),
        }
    }
    pub fn to_u32(self) -> u32 {
        match self {
            Command::Read => 0,
            Command::Write => 1,
            Command::Discard => 2,
            Command::Flush => 3,
            Command::Trim => 5,
            Command::Other(v) => v,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NbdError {
    BadMagic,
    TooShort,
    /// 客户端声明的长度异常（0 或超过合理上限）
    BadLength,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Request {
    pub handle: u64,
    pub command: Command,
    pub offset: u64,
    pub len: u32,
}

/// 解析 28 字节固定请求头
pub fn parse_request(buf: &[u8]) -> Result<Request, NbdError> {
    if buf.len() < REQUEST_HEADER_SIZE {
        return Err(NbdError::TooShort);
    }
    if u32::from_be_bytes([buf[0], buf[1], buf[2], buf[3]]) != REQUEST_MAGIC {
        return Err(NbdError::BadMagic);
    }
    let command = Command::from_u32(u32::from_be_bytes([buf[4], buf[5], buf[6], buf[7]]));
    let handle = u64::from_be_bytes([
        buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15],
    ]);
    let offset = u64::from_be_bytes([
        buf[16], buf[17], buf[18], buf[19], buf[20], buf[21], buf[22], buf[23],
    ]);
    let len = u32::from_be_bytes([buf[24], buf[25], buf[26], buf[27]]);

    // 长度必须非零且 <= 32 MiB：内核一般不会超，但畸形/恶意对端会发
    // 巨大的 len，直接按它分配就是远程 DoS。
    if len == 0 || len > 32 * 1024 * 1024 {
        return Err(NbdError::BadLength);
    }
    Ok(Request { handle, command, offset, len })
}

/// 构造 20 字节应答头（error=0 表示成功）
pub fn build_reply(handle: u64, error: u32) -> [u8; REPLY_HEADER_SIZE] {
    let mut out = [0u8; REPLY_HEADER_SIZE];
    out[0..4].copy_from_slice(&REPLY_MAGIC.to_be_bytes());
    out[4..8].copy_from_slice(&error.to_be_bytes());
    out[8..16].copy_from_slice(&handle.to_be_bytes());
    out
}

/// 旧式握手应答：16 字节大小 + 4 字节 flags + 12 字节保留
pub fn build_simple_reply(size: u64, flags: u16) -> [u8; SIMPLE_REPLY_SIZE] {
    let mut out = [0u8; SIMPLE_REPLY_SIZE];
    out[0..8].copy_from_slice(&size.to_be_bytes());
    out[8..10].copy_from_slice(&flags.to_be_bytes());
    out
}

/// 旧式握手的初始 8 字节 magic + flags
pub fn build_newstyle_handshake() -> [u8; 16] {
    let mut out = [0u8; 16];
    // INIT_MAGIC
    out[0..8].copy_from_slice(&0x4e42444d41474949u64.to_be_bytes());
    // 客户端标志位：固定 0 表示只支持旧式
    out[8..16].copy_from_slice(&0x0003u64.to_be_bytes());
    out
}

/// 判断命令是否需要读取客户端随头传来的数据
pub fn command_carries_data(c: Command) -> bool {
    matches!(c, Command::Write)
}

/// 判断命令是否需要回传数据
pub fn command_returns_data(c: Command) -> bool {
    matches!(c, Command::Read)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn req_bytes(cmd: u32, handle: u64, offset: u64, len: u32) -> Vec<u8> {
        let mut v = Vec::new();
        v.extend_from_slice(&REQUEST_MAGIC.to_be_bytes());
        v.extend_from_slice(&cmd.to_be_bytes());
        v.extend_from_slice(&handle.to_be_bytes());
        v.extend_from_slice(&offset.to_be_bytes());
        v.extend_from_slice(&len.to_be_bytes());
        v
    }

    #[test]
    fn parses_read_request() {
        let b = req_bytes(0, 0x1122, 4096, 512);
        let r = parse_request(&b).unwrap();
        assert_eq!(r.command, Command::Read);
        assert_eq!(r.handle, 0x1122);
        assert_eq!(r.offset, 4096);
        assert_eq!(r.len, 512);
    }

    #[test]
    fn rejects_bad_magic() {
        let mut b = req_bytes(0, 1, 0, 512);
        b[0] ^= 0xff;
        assert_eq!(parse_request(&b), Err(NbdError::BadMagic));
    }

    #[test]
    fn rejects_zero_and_huge_length() {
        // len=0：会让后面的缓冲区逻辑算出 0 长度切片，看似无害，
        // 但对端可以拿它做忙循环
        assert_eq!(parse_request(&req_bytes(0, 1, 0, 0)), Err(NbdError::BadLength));
        // 1 GiB：必须拒，否则一个畸形包就吃掉 1 GiB 内存
        assert_eq!(parse_request(&req_bytes(1, 1, 0, 1 << 30)), Err(NbdError::BadLength));
    }

    #[test]
    fn round_trips_reply() {
        let h = build_reply(0xDEAD_BEEF, 0);
        assert_eq!(u32::from_be_bytes([h[0], h[1], h[2], h[3]]), REPLY_MAGIC);
        assert_eq!(u64::from_be_bytes([h[8], h[9], h[10], h[11], h[12], h[13], h[14], h[15]]),
                   0xDEAD_BEEF);
    }

    #[test]
    fn short_buffer_is_not_panic() {
        // 截断的包头必须报错，不能 panic —— 这是对端可控的输入
        assert_eq!(parse_request(&[]), Err(NbdError::TooShort));
        assert_eq!(parse_request(&[0u8; 27]), Err(NbdError::TooShort));
    }

    #[test]
    fn only_write_carries_payload() {
        assert!(command_carries_data(Command::Write));
        assert!(!command_carries_data(Command::Read));
        assert!(command_returns_data(Command::Read));
        assert!(!command_returns_data(Command::Write));
    }
}
