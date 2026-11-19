// SPDX-License-Identifier: GPL-2.0-or-later
#![forbid(unsafe_code)]
use crate::session::{Session, State, Transport};
use crate::wire::{Kind, Meta, MAX};
const INBOX: usize = 16;
const BYTES: usize = 32768;
// Only the audited FFI adapter implements the callback ports in firmware.
// All calls and registry replacements occur under the same service mutex.
pub trait Ports: Transport {
    fn active(&self) -> bool;
    fn connected(&self, s: &Session) -> bool;
    fn disk(&mut self);
    fn store_time(&mut self, data: &[u8], tick: u32);
    fn clear_time(&mut self);
    fn receive(&mut self, m: Meta, data: &[u8]) -> bool;
    fn next_record(&mut self, out: &mut [u8]) -> usize;
    fn reset_consumer(&mut self);
    fn background(&mut self, m: Meta, data: &[u8]);
    fn log_enabled(&self) -> bool;
    fn log_epoch(&self) -> u32;
    fn log_read(&mut self, out: &mut [u8]) -> usize;
}
struct Inbox {
    frames: [Meta; INBOX],
    offset: [usize; INBOX],
    bytes: [u8; BYTES],
    first: usize,
    used: usize,
    byte_first: usize,
    byte_used: usize,
}
impl Inbox {
    const fn new() -> Self {
        Self {
            frames: [Meta::EMPTY; INBOX],
            offset: [0; INBOX],
            bytes: [0; BYTES],
            first: 0,
            used: 0,
            byte_first: 0,
            byte_used: 0,
        }
    }
    fn clear(&mut self) {
        self.first = 0;
        self.used = 0;
        self.byte_first = 0;
        self.byte_used = 0;
    }
    fn full(&self, n: usize) -> bool {
        self.used == INBOX || n > BYTES - self.byte_used
    }
    fn store(&mut self, m: Meta, data: &[u8]) {
        if m.len > MAX || data.len() != m.len || self.full(m.len) {
            return;
        }
        let f = (self.first + self.used) % INBOX;
        self.frames[f] = m;
        let offset = (self.byte_first + self.byte_used) % BYTES;
        self.offset[f] = offset;
        let n = core::cmp::min(m.len, BYTES - offset);
        self.bytes[offset..offset + n].copy_from_slice(&data[..n]);
        self.bytes[..m.len - n].copy_from_slice(&data[n..]);
        self.used += 1;
        self.byte_used += m.len;
    }
    fn take(&mut self, delivered: &mut [u8; MAX]) -> Option<Meta> {
        if self.used == 0 {
            return None;
        }
        let m = self.frames[self.first];
        let offset = self.offset[self.first];
        let n = core::cmp::min(m.len, BYTES - offset);
        delivered[..n].copy_from_slice(&self.bytes[offset..offset + n]);
        delivered[n..m.len].copy_from_slice(&self.bytes[..m.len - n]);
        self.byte_first = (self.byte_first + m.len) % BYTES;
        self.byte_used -= m.len;
        self.first = (self.first + 1) % INBOX;
        self.used -= 1;
        Some(m)
    }
}
pub struct Service {
    pub session: Session,
    inbox: Inbox,
    pub client: u8,
    announced: u8,
    pub running: bool,
    pub started: bool,
    pub generation: u32,
    wire_revision: u32,
    welcome: [u8; 128],
    welcome_len: usize,
    log_line: [u8; 768],
    log_len: usize,
    log_epoch: u32,
    disk_hello: bool,
    token: Option<[u8; 32]>,
    record: [u8; 512],
    record_len: usize,
}
impl Service {
    pub const fn new() -> Self {
        Self {
            session: Session::new(),
            inbox: Inbox::new(),
            client: 0,
            announced: 255,
            running: false,
            started: false,
            generation: 0,
            wire_revision: 0,
            welcome: [0; 128],
            welcome_len: 0,
            log_line: [0; 768],
            log_len: 0,
            log_epoch: 0,
            disk_hello: false,
            token: None,
            record: [0; 512],
            record_len: 0,
        }
    }
    pub fn invalidate(&mut self, p: &mut impl Ports) {
        self.invalidate_inner(p, false);
    }
    fn invalidate_inner(&mut self, p: &mut impl Ports, preserve_time: bool) {
        self.inbox.clear();
        self.record_len = 0;
        p.reset_consumer();
        self.generation = self.generation.wrapping_add(1);
        self.welcome_len = 0;
        self.disk_hello = false;
        self.token = None;
        if !preserve_time {
            p.clear_time();
        }
        self.announced = 255;
        if self.client != 1 {
            p.background(Meta::EMPTY, &[]);
        }
    }
    pub fn step(&mut self, p: &mut impl Ports) {
        if !self.running || !p.active() {
            if self.started {
                self.session.abort(p);
                self.started = false;
                self.invalidate(p);
            }
            return;
        }
        if !self.started {
            self.started = self.session.open(p);
            self.announced = 255;
        }
        for _ in 0..8 {
            if !self.started {
                break;
            }
            let message = self.session.poll(p);
            if self.session.revision != self.wire_revision {
                self.wire_revision = self.session.revision;
                self.invalidate(p);
            }
            let m = match message {
                Some(m) => m,
                None => break,
            };
            match m.kind {
                Kind::Pong => continue,
                Kind::Text => {
                    let data = &self.session.event[..m.len];
                    if data.starts_with(b"DA\t") || data.starts_with(b"DK\t") {
                        if p.connected(&self.session)
                            && self.welcome[..self.welcome_len].starts_with(b"HR\t1\t")
                            && data.len() == 37
                            && &data[3..5] == b"1\t"
                            && data[5..]
                                .iter()
                                .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(b))
                        {
                            let mut token = [0; 32];
                            token.copy_from_slice(&data[5..37]);
                            if data[1] == b'A' && self.token.is_none() {
                                self.token = Some(token);
                            } else if data[1] == b'K' && self.token == Some(token) {
                                self.token = None;
                                p.disk();
                            }
                        }
                        continue;
                    }
                    if data.starts_with(b"HR\t") && data.len() <= 128 {
                        self.welcome[..m.len].copy_from_slice(data);
                        self.welcome_len = m.len;
                    }
                    if data.starts_with(b"TS\t") {
                        p.store_time(data, p.tick() as u32);
                        continue;
                    }
                }
                Kind::Close => self.invalidate(p),
                Kind::Open | Kind::Binary | Kind::Ping | Kind::Hello => {}
            }
            if p.receive(m, &self.session.event[..m.len]) {
                continue;
            }
            if self.client != 0 {
                if self.inbox.full(m.len) {
                    let welcome = self.welcome_len;
                    let disk = self.disk_hello;
                    let token = self.token;
                    self.invalidate_inner(p, true);
                    self.welcome_len = welcome;
                    self.disk_hello = disk;
                    self.token = token;
                    self.inbox.store(Meta::EMPTY, &[]);
                    if welcome != 0 {
                        self.inbox.store(
                            Meta {
                                kind: Kind::Text,
                                seq: 0,
                                len: welcome,
                            },
                            &self.welcome[..welcome],
                        );
                    }
                }
                self.inbox.store(m, &self.session.event[..m.len]);
            } else {
                p.background(m, &self.session.event[..m.len]);
            }
        }
        if self.started && self.session.state == State::Connected {
            if !self.disk_hello && self.session.send(Kind::Text, b"DH\t1\tdisk-mode-v1") == 1 {
                self.disk_hello = true;
            }
            if self.announced != self.client
                && self.session.peer_max >= 5
                && self
                    .session
                    .send(Kind::Ping, &[b'P', b'L', b'F', b'G', self.client])
                    == 1
            {
                self.announced = self.client;
            }
            if self.record_len == 0 {
                self.record_len =
                    core::cmp::min(p.next_record(&mut self.record), self.record.len());
            }
            if self.record_len != 0
                && self
                    .session
                    .send(Kind::Text, &self.record[..self.record_len])
                    == 1
            {
                self.record_len = 0;
            }
        }
        if !p.log_enabled() || self.log_epoch != p.log_epoch() {
            self.log_len = 0;
            self.log_epoch = p.log_epoch();
        }
        if self.started
            && p.log_enabled()
            && self.session.state == State::Connected
            && self.session.peer_max >= 10
        {
            if self.log_len == 0 {
                self.log_len = core::cmp::min(p.log_read(&mut self.log_line), self.log_line.len());
            }
            if self.log_len > self.session.peer_max {
                self.log_line[..10].copy_from_slice(b"LOG LOST 1");
                self.log_len = 10;
            }
            if self.log_len != 0
                && self
                    .session
                    .send(Kind::Text, &self.log_line[..self.log_len])
                    == 1
            {
                self.log_len = 0;
            }
        }
    }
    pub fn attach(&mut self, kind: u8, p: &mut impl Ports) -> bool {
        if !self.running || self.client != 0 || !p.open() {
            return false;
        }
        self.client = kind;
        self.inbox.clear();
        if kind == 1 && self.welcome_len != 0 {
            self.inbox.store(
                Meta {
                    kind: Kind::Text,
                    seq: 0,
                    len: self.welcome_len,
                },
                &self.welcome[..self.welcome_len],
            );
        }
        true
    }
    pub fn detach(&mut self, p: &mut impl Ports) {
        if self.client == 1 {
            while let Some(m) = self.inbox.take(&mut self.session.event) {
                p.background(m, &self.session.event[..m.len]);
            }
        }
        self.client = 0;
        self.inbox.clear();
    }
    pub fn state(&self) -> State {
        if self.client == 0 {
            State::Closed
        } else if self.started {
            self.session.state
        } else {
            State::WaitUsb
        }
    }
    pub fn send(&mut self, kind: Kind, data: &[u8]) -> i32 {
        if self.client != 0 && self.started {
            self.session.send(kind, data)
        } else {
            0
        }
    }
    pub fn poll(&mut self, delivered: &mut [u8; MAX]) -> Option<Meta> {
        if self.client != 0 {
            self.inbox.take(delivered)
        } else {
            None
        }
    }
    #[cfg(feature = "host-test")]
    pub fn debug(&self, id: u32) -> u32 {
        match id {
            10 => self.client as u32,
            11 => self.started as u32,
            12 => self.inbox.used as u32,
            13 => self.inbox.byte_used as u32,
            14 => self.token.map_or(0, |t| t[0] as u32),
            15 => self.disk_hello as u32,
            17 => self.log_len as u32,
            _ => self.session.debug(id),
        }
    }
    #[cfg(feature = "host-test")]
    pub fn debug_log(&self, out: &mut [u8]) {
        out.copy_from_slice(&self.log_line);
    }
    pub fn clear_record(&mut self) {
        self.record_len = 0;
    }
    pub fn shutdown(&mut self, p: &mut impl Ports) {
        if self.started {
            self.session.close(p);
            self.session.abort(p);
        }
        self.running = false;
        self.started = false;
        self.invalidate(p);
    }
    pub const fn inbox_ram() -> usize {
        core::mem::size_of::<Inbox>() + MAX + 128 + 512
    }
}
impl Default for Service {
    fn default() -> Self {
        Self::new()
    }
}

// Separate storage lets input/UI consume time without borrowing the worker's
// state or waiting for its mutex. The FFI module supplies the IRQ guard.
pub struct TimeMailbox {
    data: [u8; 64],
    tick: u32,
    pending: bool,
}
impl TimeMailbox {
    pub const fn new() -> Self {
        Self {
            data: [0; 64],
            tick: 0,
            pending: false,
        }
    }
    pub fn publish(&mut self, data: &[u8], tick: u32) -> bool {
        self.data[0] = 0;
        if data.len() < 64 && !data.contains(&0) {
            self.data[..data.len()].copy_from_slice(data);
            self.data[data.len()] = 0;
        }
        self.tick = tick;
        let wake = !self.pending;
        self.pending = true;
        wake
    }
    pub fn take(&mut self, out: &mut [u8; 64]) -> Option<u32> {
        if !self.pending {
            return None;
        }
        out.copy_from_slice(&self.data);
        self.pending = false;
        Some(self.tick)
    }
    pub fn clear(&mut self) {
        self.pending = false;
    }
    #[cfg(feature = "host-test")]
    pub fn pending(&self) -> bool {
        self.pending
    }
}
impl Default for TimeMailbox {
    fn default() -> Self {
        Self::new()
    }
}
