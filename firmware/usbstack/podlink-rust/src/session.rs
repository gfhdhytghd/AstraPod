// SPDX-License-Identifier: GPL-2.0-or-later
#![forbid(unsafe_code)]
use crate::wire::{self, Kind, Meta, Parser, FRAME, MAX};
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(i32)]
pub enum State {
    Closed,
    WaitUsb,
    Opening,
    Connected,
    Closing,
}
pub trait Transport {
    fn open(&mut self) -> bool;
    fn close(&mut self);
    fn usb_state(&mut self) -> Option<u32>;
    fn read(&mut self, dest: &mut [u8]) -> usize;
    fn write(&mut self, data: &[u8]) -> usize;
    fn tick(&self) -> i32;
    fn hz(&self) -> i32;
    fn wait(&mut self, ticks: i32);
}
struct Tx {
    data: [u8; FRAME],
    len: usize,
    sent: usize,
}
impl Tx {
    const NEW: Self = Self {
        data: [0; FRAME],
        len: 0,
        sent: 0,
    };
}
pub struct Session {
    tx: [Tx; 3],
    head: usize,
    count: usize,
    pub parser: Parser,
    input: [u8; 4096],
    input_pos: usize,
    input_len: usize,
    pub event: [u8; MAX],
    pub state: State,
    next_seq: u32,
    ping_seq: u32,
    pub peer_max: usize,
    pub revision: u32,
    pub transport_generation: u32,
    last_rx: i32,
    ping_at: i32,
    close_at: i32,
    pub leased: bool,
    pub had_usb: bool,
    ping_pending: bool,
    release: bool,
}
fn after(a: i32, b: i32) -> bool {
    b.wrapping_sub(a) < 0
}
impl Session {
    pub const fn new() -> Self {
        Self {
            tx: [Tx::NEW; 3],
            head: 0,
            count: 0,
            parser: Parser::new(),
            input: [0; 4096],
            input_pos: 0,
            input_len: 0,
            event: [0; MAX],
            state: State::Closed,
            next_seq: 0,
            ping_seq: 0,
            peer_max: 0,
            revision: 0,
            transport_generation: 0,
            last_rx: 0,
            ping_at: 0,
            close_at: 0,
            leased: false,
            had_usb: false,
            ping_pending: false,
            release: false,
        }
    }
    fn enqueue(&mut self, kind: Kind, seq: u32, data: &[u8], control: bool) -> i32 {
        if self.count >= if control { 3 } else { 2 } {
            return 0;
        }
        let f = &mut self.tx[(self.head + self.count) % 3];
        f.len = wire::encode(&mut f.data, kind, seq, data);
        if f.len == 0 {
            return -1;
        }
        f.sent = 0;
        self.count += 1;
        1
    }
    fn next(&mut self) -> u32 {
        let s = self.next_seq;
        self.next_seq = self.next_seq.wrapping_add(1);
        s
    }
    fn pump(&mut self, p: &mut impl Transport) {
        if self.count == 0 {
            return;
        }
        let f = &mut self.tx[self.head];
        f.sent += core::cmp::min(p.write(&f.data[f.sent..f.len]), f.len - f.sent);
        if f.sent == f.len {
            self.head = (self.head + 1) % 3;
            self.count -= 1;
        }
    }
    fn reset(&mut self) {
        self.head = 0;
        self.count = 0;
        self.parser.reset();
        self.input_pos = 0;
        self.input_len = 0;
        self.ping_pending = false;
        self.next_seq = 1;
        self.peer_max = MAX;
    }
    pub fn abort(&mut self, p: &mut impl Transport) {
        p.close();
        self.leased = false;
        self.state = State::Closed;
        self.reset();
    }
    fn close_event(&mut self, code: u16) -> Option<Meta> {
        self.event[..2].copy_from_slice(&code.to_le_bytes());
        Some(Meta {
            kind: Kind::Close,
            seq: 0,
            len: 2,
        })
    }
    fn send_close(&mut self, code: u16) {
        let seq = self.next();
        self.enqueue(Kind::Close, seq, &code.to_le_bytes(), true);
    }
    #[cfg(feature = "host-test")]
    pub fn debug(&self, id: u32) -> u32 {
        match id {
            0 => self.parser.used,
            1 => self.count as u32,
            2 => self.input_pos as u32,
            3 => self.input_len as u32,
            4 => self.ping_pending as u32,
            5 => self.peer_max as u32,
            _ => 0,
        }
    }
    pub fn open(&mut self, p: &mut impl Transport) -> bool {
        if self.leased || !p.open() {
            return false;
        }
        self.leased = true;
        self.had_usb = false;
        self.release = false;
        self.reset();
        self.state = State::WaitUsb;
        true
    }
    pub fn send(&mut self, kind: Kind, data: &[u8]) -> i32 {
        if !matches!(kind, Kind::Text | Kind::Binary | Kind::Ping | Kind::Pong)
            || data.len() > self.peer_max
        {
            return -1;
        }
        if self.state != State::Connected {
            return 0;
        }
        let rc = self.enqueue(kind, self.next_seq, data, false);
        if rc == 1 {
            self.next();
        }
        rc
    }
    pub(crate) fn send_resident_binary(&mut self, next: impl FnOnce(&mut [u8]) -> usize) {
        // Pull a chunk only when it can be queued. Control keeps the third slot.
        if self.count >= 2 || self.peer_max == 0 {
            return;
        }
        let frame = &mut self.tx[(self.head + self.count) % 3];
        let n = next(&mut frame.data[16..16 + self.peer_max]);
        if n != 0 && n <= self.peer_max {
            frame.len = wire::finish_encode(&mut frame.data, Kind::Binary, self.next_seq, n);
            frame.sent = 0;
            self.count += 1;
            self.next();
        }
    }
    pub fn close(&mut self, p: &mut impl Transport) {
        if !self.leased || self.release {
            return;
        }
        self.release = true;
        if !self.had_usb {
            self.abort(p);
            return;
        }
        self.send_close(1000);
        self.state = State::Closing;
        self.close_at = p.tick();
    }
    pub fn wait(&mut self, p: &mut impl Transport, ticks: i32) {
        if self.input_pos == self.input_len {
            p.wait(ticks);
        }
    }
    pub fn poll(&mut self, p: &mut impl Transport) -> Option<Meta> {
        if !self.leased {
            return None;
        }
        let usb = p.usb_state();
        if usb.is_none() || (self.had_usb && usb != Some(self.transport_generation)) {
            let notify = self.had_usb;
            self.had_usb = false;
            self.reset();
            self.state = State::WaitUsb;
            if self.release {
                self.abort(p);
            }
            return if notify { self.close_event(1006) } else { None };
        }
        if !self.had_usb {
            self.had_usb = true;
            self.transport_generation = usb.unwrap();
            self.reset();
            self.state = State::Opening;
            self.last_rx = p.tick();
            let seq = self.next();
            self.enqueue(Kind::Hello, seq, &[1, 0, 0, 64, 0, 0], true);
        }
        self.pump(p);
        if self.release && after(p.tick(), self.close_at.wrapping_add(p.hz())) {
            self.abort(p);
            return self.close_event(1000);
        }
        if self.state == State::Connected {
            if self.ping_pending && after(p.tick(), self.ping_at.wrapping_add(15 * p.hz())) {
                self.send_close(1001);
                self.state = State::Opening;
                self.ping_pending = false;
                return self.close_event(1001);
            }
            if !self.ping_pending && after(p.tick(), self.last_rx.wrapping_add(5 * p.hz())) {
                self.ping_seq = self.next();
                if self.enqueue(Kind::Ping, self.ping_seq, &[], true) == 1 {
                    self.ping_pending = true;
                    self.ping_at = p.tick();
                }
            }
        }
        if self.count != 0 {
            return None;
        }
        for _ in 0..64 {
            if self.input_pos == self.input_len {
                self.input_len = core::cmp::min(p.read(&mut self.input), self.input.len());
                self.input_pos = 0;
            }
            let (consumed, result) = self
                .parser
                .decode(&self.input[self.input_pos..self.input_len]);
            self.input_pos += consumed;
            let m = match result {
                Err(-2) => {
                    self.send_close(1009);
                    self.state = State::Opening;
                    return self.close_event(1009);
                }
                Ok(Some(m)) => m,
                _ => {
                    if matches!(result, Ok(None)) && self.input_len == 0 {
                        break;
                    }
                    continue;
                }
            };
            self.last_rx = p.tick();
            if self.state == State::Connected
                && !matches!(m.kind, Kind::Open | Kind::Hello)
                && m.len > self.peer_max
            {
                self.send_close(1009);
                self.state = State::Opening;
                return self.close_event(1009);
            }
            // The parser owns the payload. Copy only bounded, validated messages.
            self.event[..m.len].copy_from_slice(self.parser.payload(m));
            match m.kind {
                Kind::Open => {
                    if self.release {
                        continue;
                    }
                    if m.len != 6 || self.event[0] != 1 || self.event[1] != 0 {
                        self.send_close(1002);
                        self.state = State::Opening;
                        return self.close_event(1002);
                    }
                    let limit = wire::get32(&self.event[2..6]) as usize;
                    if limit == 0 {
                        self.send_close(1002);
                        continue;
                    }
                    self.peer_max = core::cmp::min(limit, MAX);
                    let n = (self.peer_max as u32).to_le_bytes();
                    self.enqueue(Kind::Open, m.seq, &[1, 0, n[0], n[1], n[2], n[3]], true);
                    self.state = State::Connected;
                    self.revision = self.revision.wrapping_add(1);
                    self.ping_pending = false;
                }
                Kind::Close => {
                    if m.len < 2 {
                        self.send_close(1002);
                        if !self.release {
                            self.state = State::Opening;
                            self.ping_pending = false;
                        }
                        return self.close_event(1002);
                    }
                    if self.release {
                        self.abort(p);
                    } else {
                        // Disjoint borrows avoid an extra 16 KiB stack copy.
                        let f = &mut self.tx[(self.head + self.count) % 3];
                        if self.count < 3 {
                            f.len =
                                wire::encode(&mut f.data, Kind::Close, m.seq, &self.event[..m.len]);
                            f.sent = 0;
                            self.count += 1;
                        }
                        self.state = State::Opening;
                        self.ping_pending = false;
                    }
                    return Some(m);
                }
                Kind::Ping if self.state == State::Connected && self.count < 3 => {
                    let f = &mut self.tx[(self.head + self.count) % 3];
                    f.len = wire::encode(&mut f.data, Kind::Pong, m.seq, &self.event[..m.len]);
                    f.sent = 0;
                    self.count += 1;
                }
                Kind::Pong if self.state == State::Connected => {
                    if m.seq == self.ping_seq {
                        self.ping_pending = false;
                    }
                    return Some(m);
                }
                Kind::Text | Kind::Binary if self.state == State::Connected => return Some(m),
                _ => {}
            }
            if self.count != 0 {
                break;
            }
        }
        self.pump(p);
        None
    }
}
impl Default for Session {
    fn default() -> Self {
        Self::new()
    }
}
