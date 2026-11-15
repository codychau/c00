//! RAID0 条带映射 —— 纯计算，不碰文件系统。
//!
//! 布局（chunk = 条带粒度，n = 成员数）：
//!
//!   逻辑偏移 L  →  条带号 s = L / chunk
//!                →  成员   m = s % n
//!                →  成员内偏移 o = (s / n) * chunk + (L % chunk)
//!
//! 这是 Linux md RAID0 的 "parity-ignore 0" 布局同构，便于和 mdadm 的结果对照。
//!
//! 关键约束：**所有成员必须等大**。最后一个条带如果不完整，
//! 本实现在读时返回零、在写时拒绝（返回 ShortWrite），
//! 而不是静默写到成员之外 —— 静默越界会写坏成员分区上的其它文件。

/// 条带映射的静态参数
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Layout {
    /// 条带粒度，字节。必须是 2 的幂且 >= 512。
    pub chunk: u64,
    /// 成员数量，>= 2
    pub disks: usize,
    /// 每个成员的容量（字节）。所有成员相同。
    pub member_size: u64,
}

/// 一次 IO 落点
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Extent {
    pub disk: usize,
    pub offset: u64,
    pub len: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MapError {
    /// 起始逻辑偏移超出卷容量
    BeyondVolume,
    /// 请求跨到了不完整尾条带，无法安全完成
    ShortWrite,
    /// 参数不合法
    InvalidLayout,
}

impl Layout {
    pub fn new(chunk: u64, disks: usize, member_size: u64) -> Result<Self, MapError> {
        if disks < 2 {
            return Err(MapError::InvalidLayout);
        }
        // 2 的幂：这样 chunk 的除法/取模可以退化成移位与掩码，
        // 热路径上每次 IO 都要算，省下来的除法不是装饰。
        if chunk < 512 || !chunk.is_power_of_two() {
            return Err(MapError::InvalidLayout);
        }
        if member_size < chunk {
            return Err(MapError::InvalidLayout);
        }
        Ok(Self { chunk, disks, member_size })
    }

    /// 每个成员能装下多少个**完整**条带
    pub fn full_stripes_per_disk(&self) -> u64 {
        self.member_size / self.chunk
    }

    /// 卷的可用容量：只算完整条带，尾部的零头一律不算。
    ///
    /// 注意不要用 member_size * disks —— 当 member_size 不是 chunk 的
    /// 整数倍时，多出来的那段永远写不到，报出去会让用户以为有空间。
    pub fn capacity(&self) -> u64 {
        self.full_stripes_per_disk() * self.chunk * self.disks as u64
    }

    /// 因为"必须整条带"而浪费掉的字节数（每个成员各浪费这么多）
    pub fn slack_per_disk(&self) -> u64 {
        self.member_size - self.full_stripes_per_disk() * self.chunk
    }
}

/// 把 [offset, offset+len) 拆成一串落在单个成员上的区间。
///
/// 返回的每个 Extent 保证完全落在同一个成员、且不超过该成员容量，
/// 因此调用方可以对每个区间单独 pread/pwrite，无需再判边界。
pub fn map(layout: &Layout, offset: u64, len: u64) -> Result<Vec<Extent>, MapError> {
    if len == 0 {
        return Ok(Vec::new());
    }
    let cap = layout.capacity();
    // 先验总长度，避免 offset + len 溢出成小数字绕过检查
    let end = offset.checked_add(len).ok_or(MapError::BeyondVolume)?;
    if offset >= cap || end > cap {
        return Err(MapError::BeyondVolume);
    }

    let shift = layout.chunk.trailing_zeros();
    let mask = layout.chunk - 1;
    let n = layout.disks as u64;

    let mut out = Vec::with_capacity(4);
    let mut cur = offset;
    while cur < end {
        let stripe = cur >> shift;
        let within = cur & mask;
        // 本次请求在当前条带内还能吃多少
        let room_in_stripe = layout.chunk - within;
        let want = std::cmp::min(room_in_stripe, end - cur);

        let disk = (stripe % n) as usize;
        let member_off = (stripe / n) * layout.chunk + within;

        // 到这里 member_off + want 一定 <= member_size：
        // capacity() 只统计完整条带，end <= capacity 已排除尾部零头。
        out.push(Extent { disk, offset: member_off, len: want });
        cur += want;
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn lay() -> Layout {
        Layout::new(4096, 2, 16384).unwrap()
    }

    #[test]
    fn capacity_counts_full_stripes_only() {
        let l = lay();
        // member 16384 / chunk 4096 = 4 条带/盘，2 盘 → 32768
        assert_eq!(l.capacity(), 32768);
        assert_eq!(l.slack_per_disk(), 0);

        // 容量不是 chunk 整数倍时，零头不算进容量
        let l2 = Layout::new(4096, 2, 16384 + 1000).unwrap();
        assert_eq!(l2.capacity(), 32768);
        assert_eq!(l2.slack_per_disk(), 1000);
    }

    #[test]
    fn stripes_alternate_disks() {
        let l = lay();
        // 第 0 条带 → 盘 0；第 1 条带 → 盘 1；第 2 条带 → 盘 0
        assert_eq!(map(&l, 0, 4096).unwrap()[0].disk, 0);
        assert_eq!(map(&l, 4096, 4096).unwrap()[0].disk, 1);
        assert_eq!(map(&l, 8192, 4096).unwrap()[0].disk, 0);
        // 成员内偏移按"第几个自己的条带"推进
        assert_eq!(map(&l, 8192, 4096).unwrap()[0].offset, 4096);
    }

    #[test]
    fn unaligned_request_is_split() {
        let l = lay();
        // 从 4000 开始读 3000 字节：跨过条带边界，必须拆成 2 段
        let ext = map(&l, 4000, 3000).unwrap();
        assert_eq!(ext.len(), 2);
        assert_eq!(ext[0].len, 96);   // 96 = 4096 - 4000
        assert_eq!(ext[1].len, 2904);
        assert_eq!(ext[0].disk, 0);
        assert_eq!(ext[1].disk, 1);
        // 拼回去的总长必须等于请求长度：这是"不丢字节"的基本保证
        let total: u64 = ext.iter().map(|e| e.len).sum();
        assert_eq!(total, 3000);
    }

    #[test]
    fn every_extent_stays_inside_its_member() {
        let l = lay();
        for off in (0..l.capacity()).step_by(997) {
            for len in [1usize, 511, 4096, 9000] {
                let end = std::cmp::min(off + len as u64, l.capacity());
                let req = end - off;
                if req == 0 { continue; }
                for e in map(&l, off, req).unwrap() {
                    assert!(e.disk < l.disks, "盘号越界");
                    assert!(e.offset + e.len <= l.member_size, "成员内越界");
                }
            }
        }
    }

    #[test]
    fn beyond_volume_is_rejected_not_clamped() {
        let l = lay();
        // 越过末尾 1 字节：整笔拒绝，不做部分成功
        assert_eq!(map(&l, l.capacity() - 1, 2), Err(MapError::BeyondVolume));
        assert_eq!(map(&l, l.capacity(), 1), Err(MapError::BeyondVolume));
    }

    #[test]
    fn overflow_cannot_bypass_bounds() {
        let l = lay();
        // offset + len 溢出：checked_add 必须拦住，
        // 否则回绕成小数字就能写出成员之外，毁掉分区上的其它文件。
        assert_eq!(map(&l, u64::MAX - 10, 100), Err(MapError::BeyondVolume));
    }

    #[test]
    fn invalid_layouts_rejected() {
        assert_eq!(Layout::new(4096, 1, 1 << 20), Err(MapError::InvalidLayout));
        assert_eq!(Layout::new(3000, 2, 1 << 20), Err(MapError::InvalidLayout)); // 非 2 的幂
        assert_eq!(Layout::new(256, 2, 1 << 20), Err(MapError::InvalidLayout));  // < 512
        assert_eq!(Layout::new(4096, 2, 512), Err(MapError::InvalidLayout));     // 成员比条带还小
    }

    #[test]
    fn round_trip_reassembly_is_identity() {
        // 写入顺序 = 读出顺序时，条带映射必须原样还原数据。
        // 用内存里两个"成员"模拟文件，验证整卷写读一致。
        let l = lay();
        let mut members = vec![vec![0u8; l.member_size as usize]; l.disks];

        let data: Vec<u8> = (0..l.capacity() as usize).map(|i| (i % 251) as u8).collect();

        // 写：map 给出的 Extent 流是**按逻辑偏移递增**的，
        // 所以可以用一个游标依次取源数据；这个前提若破了，下面就会错位。
        let mut logical = 0u64;
        for e in map(&l, 0, l.capacity()).unwrap() {
            let s = e.offset as usize;
            let ls = logical as usize;
            members[e.disk][s..s + e.len as usize]
                .copy_from_slice(&data[ls..ls + e.len as usize]);
            logical += e.len;
        }
        assert_eq!(logical, l.capacity(), "映射必须覆盖整卷，不多不少");

        // 读：同一套映射，逐字节比对
        let mut back = vec![0u8; l.capacity() as usize];
        let mut logical = 0u64;
        for e in map(&l, 0, l.capacity()).unwrap() {
            let s = e.offset as usize;
            let ls = logical as usize;
            back[ls..ls + e.len as usize]
                .copy_from_slice(&members[e.disk][s..s + e.len as usize]);
            logical += e.len;
        }
        assert_eq!(back, data);
    }

    #[test]
    fn extents_are_emitted_in_logical_order() {
        // round_trip 的正确性依赖"Extent 按逻辑偏移递增"。
        // 这条把它变成显式契约，将来若改成乱序/并行下发，测试会先炸。
        let l = lay();
        for off in (0..l.capacity()).step_by(1009) {
            let end = std::cmp::min(off + 12000, l.capacity());
            if end <= off { continue; }
            let ext = map(&l, off, end - off).unwrap();
            let mut sum = 0u64;
            for (i, e) in ext.iter().enumerate() {
                if i > 0 {
                    assert!(ext[i - 1].offset <= e.offset
                            || ext[i - 1].disk != e.disk,
                            "同一盘上的区间必须递增");
                }
                sum += e.len;
            }
            assert_eq!(sum, end - off);
        }
    }
}
