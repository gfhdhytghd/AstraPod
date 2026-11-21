// SPDX-License-Identifier: GPL-2.0-or-later
#![forbid(unsafe_code)]
pub const MAX: usize = 16384;
pub const FRAME: usize = MAX + 20;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Kind {
    Open = 1,
    Text,
    Binary,
    Ping,
    Pong,
    Close,
    Hello,
}
impl Kind {
    pub fn parse(n: u32) -> Option<Self> {
        Some(match n {
            1 => Self::Open,
            2 => Self::Text,
            3 => Self::Binary,
            4 => Self::Ping,
            5 => Self::Pong,
            6 => Self::Close,
            7 => Self::Hello,
            _ => return None,
        })
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Meta {
    pub kind: Kind,
    pub seq: u32,
    pub len: usize,
}
impl Meta {
    pub const EMPTY: Self = Self {
        kind: Kind::Close,
        seq: 0,
        len: 0,
    };
}
const fn table() -> [u32; 256] {
    let mut t = [0; 256];
    let mut i = 0;
    while i < 256 {
        let mut c = i as u32;
        let mut j = 0;
        while j < 8 {
            c = if c & 1 != 0 {
                0xedb88320 ^ (c >> 1)
            } else {
                c >> 1
            };
            j += 1;
        }
        t[i] = c;
        i += 1;
    }
    t
}
const CRC: [u32; 256] = table();
pub fn crc32(data: &[u8]) -> u32 {
    let mut c = !0u32;
    for b in data {
        c = CRC[((c ^ *b as u32) & 255) as usize] ^ (c >> 8);
    }
    !c
}
pub fn get32(data: &[u8]) -> u32 {
    u32::from_le_bytes([data[0], data[1], data[2], data[3]])
}
pub fn encode(dest: &mut [u8], kind: Kind, seq: u32, data: &[u8]) -> usize {
    if data.len() > MAX || dest.len() < data.len() + 20 {
        return 0;
    }
    let n = data.len();
    dest[16..16 + n].copy_from_slice(data);
    finish_encode(dest, kind, seq, n)
}

/* The caller has filled the payload starting at byte 16. */
pub(crate) fn finish_encode(dest: &mut [u8], kind: Kind, seq: u32, n: usize) -> usize {
    dest[..4].copy_from_slice(b"PLNK");
    dest[4..8].copy_from_slice(&[kind as u8, 0, 0, 0]);
    dest[8..12].copy_from_slice(&seq.to_le_bytes());
    dest[12..16].copy_from_slice(&(n as u32).to_le_bytes());
    let c = crc32(&dest[4..16 + n]);
    dest[16 + n..20 + n].copy_from_slice(&c.to_le_bytes());
    n + 20
}
// Layout also serves the public C parser ABI. Bounds are revalidated on entry.
#[repr(C)]
pub struct Parser {
    pub bytes: [u8; FRAME],
    pub used: u32,
    pub consumed: u32,
}
impl Parser {
    pub const fn new() -> Self {
        Self {
            bytes: [0; FRAME],
            used: 0,
            consumed: 0,
        }
    }
    pub fn reset(&mut self) {
        self.used = 0;
        self.consumed = 0;
    }
    pub(crate) fn compact(&mut self) {
        if self.used as usize > FRAME || self.consumed > self.used {
            self.reset();
        }
        if self.consumed != 0 {
            self.bytes
                .copy_within(self.consumed as usize..self.used as usize, 0);
            self.used -= self.consumed;
            self.consumed = 0;
        }
    }
    fn discard(&mut self) {
        self.bytes.copy_within(1..self.used as usize, 0);
        self.used -= 1;
    }
    pub(crate) fn buffered(&mut self) -> Result<Option<Meta>, i32> {
        while self.used >= 4 && &self.bytes[..4] != b"PLNK" {
            self.discard();
        }
        if self.used < 16 {
            return Ok(None);
        }
        let kind = Kind::parse(self.bytes[4] as u32);
        if kind.is_none() || self.bytes[5..8] != [0, 0, 0] {
            self.discard();
            return Err(-1);
        }
        let len = get32(&self.bytes[12..16]) as usize;
        if len > MAX {
            self.discard();
            return Err(-2);
        }
        if (self.used as usize) < len + 20 {
            return Ok(None);
        }
        if get32(&self.bytes[16 + len..20 + len]) != crc32(&self.bytes[4..16 + len]) {
            self.discard();
            return Err(-1);
        }
        self.consumed = (len + 20) as u32;
        Ok(Some(Meta {
            kind: kind.unwrap(),
            seq: get32(&self.bytes[8..12]),
            len,
        }))
    }
    pub fn decode(&mut self, data: &[u8]) -> (usize, Result<Option<Meta>, i32>) {
        self.compact();
        let mut used = 0;
        match self.buffered() {
            Ok(None) => {}
            r => return (0, r),
        }
        while used < data.len() {
            let p = self.used as usize;
            let mut target = 0;
            if p >= 4 && &self.bytes[..4] == b"PLNK" {
                if p < 16 {
                    target = 16;
                } else if Kind::parse(self.bytes[4] as u32).is_some()
                    && self.bytes[5..8] == [0, 0, 0]
                {
                    let n = get32(&self.bytes[12..16]) as usize;
                    if n <= MAX {
                        target = n + 20;
                    }
                }
            }
            if target > 0 && p < target - 1 {
                let n = core::cmp::min(target - 1 - p, data.len() - used);
                self.bytes[p..p + n].copy_from_slice(&data[used..used + n]);
                self.used += n as u32;
                used += n;
                continue;
            }
            // A full valid-sized frame has already yielded a result above.
            if p >= FRAME {
                self.reset();
                return (used, Err(-1));
            }
            self.bytes[p] = data[used];
            self.used += 1;
            used += 1;
            match self.buffered() {
                Ok(None) => {}
                r => return (used, r),
            }
        }
        (used, Ok(None))
    }
    pub fn payload(&self, m: Meta) -> &[u8] {
        &self.bytes[16..16 + m.len]
    }
}
impl Default for Parser {
    fn default() -> Self {
        Self::new()
    }
}
