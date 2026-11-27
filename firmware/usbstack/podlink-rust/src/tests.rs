// SPDX-License-Identifier: GPL-2.0-or-later
#![forbid(unsafe_code)]
use crate::{
    session::{Session, State, Transport},
    wire::{self, Kind, Parser, FRAME, MAX},
};
use std::{vec, vec::Vec};
#[test]
fn full_frame_and_all_fragment_sizes() {
    let data: Vec<u8> = (0..MAX).map(|i| (i % 251) as u8).collect();
    let mut frame = vec![0; FRAME];
    assert_eq!(
        wire::encode(&mut frame, Kind::Binary, u32::MAX, &data),
        FRAME
    );
    for chunk in [1, 7, 64, 512, 4096, FRAME] {
        let mut parser = Parser::new();
        let mut messages = 0;
        for part in frame.chunks(chunk) {
            let (n, r) = parser.decode(part);
            assert_eq!(n, part.len());
            if let Some(m) = r.unwrap() {
                assert_eq!(m.seq, u32::MAX);
                assert_eq!(parser.payload(m), data);
                messages += 1;
            }
        }
        assert_eq!(messages, 1);
    }
}
#[test]
fn parser_rejects_untrusted_lengths_and_corrupt_c_state() {
    let mut p = Parser::new();
    p.used = u32::MAX;
    p.consumed = u32::MAX;
    assert_eq!(p.decode(&[]), (0, Ok(None)));
    let mut b = [0; 20];
    wire::encode(&mut b, Kind::Text, 1, &[]);
    b[12..16].copy_from_slice(&u32::MAX.to_le_bytes());
    assert_eq!(p.decode(&b).1, Err(-2));
    assert_eq!(wire::encode(&mut b, Kind::Binary, 1, &[0; 21]), 0);
}
#[test]
fn crc_standard_vector() {
    assert_eq!(wire::crc32(b"123456789"), 0xcbf43926);
}
struct Port {
    now: i32,
    input: Vec<u8>,
    output: Vec<u8>,
    usb: Option<u32>,
}
impl Transport for Port {
    fn open(&mut self) -> bool {
        true
    }
    fn close(&mut self) {}
    fn usb_state(&mut self) -> Option<u32> {
        self.usb
    }
    fn read(&mut self, b: &mut [u8]) -> usize {
        let n = b.len().min(self.input.len());
        b[..n].copy_from_slice(&self.input[..n]);
        self.input.drain(..n);
        n
    }
    fn write(&mut self, b: &[u8]) -> usize {
        self.output.extend_from_slice(b);
        b.len()
    }
    fn tick(&self) -> i32 {
        self.now
    }
    fn hz(&self) -> i32 {
        100
    }
    fn wait(&mut self, _: i32) {}
}
fn input(p: &mut Port, k: Kind, data: &[u8]) {
    let mut b = vec![0; data.len() + 20];
    wire::encode(&mut b, k, 42, data);
    p.input.extend_from_slice(&b);
}
#[test]
fn keepalive_and_close_across_tick_wrap() {
    let mut s = Session::new();
    let mut p = Port {
        now: i32::MAX - 100,
        input: vec![],
        output: vec![],
        usb: Some(1),
    };
    assert!(s.open(&mut p));
    s.poll(&mut p);
    input(&mut p, Kind::Open, &[1, 0, 0, 64, 0, 0]);
    s.poll(&mut p);
    assert_eq!(s.state, State::Connected);
    p.now = p.now.wrapping_add(501);
    s.poll(&mut p);
    p.now = p.now.wrapping_add(1501);
    let m = s.poll(&mut p).unwrap();
    assert_eq!(m.kind, Kind::Close);
    assert_eq!(&s.event[..2], &1001u16.to_le_bytes());
    s.close(&mut p);
    p.now = p.now.wrapping_add(101);
    s.poll(&mut p);
    assert_eq!(s.state, State::Closed);
}
#[test]
fn deterministic_wire_fuzz_never_exceeds_fixed_capacity() {
    let mut seed = 0x48u32;
    let mut p = Parser::new();
    for _ in 0..100_000 {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        let b = [seed as u8];
        let _ = p.decode(&b);
        assert!(p.used as usize <= FRAME);
    }
}

#[test]
fn clock_mailbox_coalesces_and_rejects_untrusted_records() {
    use crate::service::{TimeMailbox, TIME_RECORD};
    let mut mailbox = TimeMailbox::new();
    let mut out = [0; TIME_RECORD];
    assert_eq!(mailbox.take(&mut out), None);
    assert!(mailbox.publish(b"TS\tfirst", 10));
    assert!(!mailbox.publish(b"TS\tnewest", 20));
    assert_eq!(mailbox.take(&mut out), Some(20));
    assert_eq!(&out[..10], b"TS\tnewest\0");
    for invalid in [&[b'x'; TIME_RECORD][..], &b"TS\tbad\0value"[..]] {
        assert!(mailbox.publish(invalid, 30));
        assert_eq!(mailbox.take(&mut out), Some(30));
        assert_eq!(out[0], 0);
    }
    mailbox.publish(b"TS\tvalid", 40);
    mailbox.clear();
    assert_eq!(mailbox.take(&mut out), None);
}

#[test]
fn resident_binary_waits_for_tx_space_and_uses_the_negotiated_limit() {
    let mut s = Session::new();
    let mut p = Port {
        now: 0,
        input: vec![],
        output: vec![],
        usb: Some(1),
    };
    assert!(s.open(&mut p));
    s.poll(&mut p);
    let limit = 4096u32;
    let mut offer = vec![1, 0];
    offer.extend_from_slice(&limit.to_le_bytes());
    input(&mut p, Kind::Open, &offer);
    for _ in 0..3 {
        s.poll(&mut p);
    }
    assert_eq!(s.state, State::Connected);
    p.output.clear();
    assert_eq!(s.send(Kind::Text, b"one"), 1);
    assert_eq!(s.send(Kind::Text, b"two"), 1);
    assert_eq!(s.send(Kind::Binary, b"three"), 0);
    s.send_resident_binary(|_| panic!("must retain the chunk while busy"));
    s.poll(&mut p);
    let chunk = vec![0x9a; limit as usize];
    s.send_resident_binary(|out| {
        assert_eq!(out.len(), chunk.len());
        out.copy_from_slice(&chunk);
        (out.len(), None)
    });
    for _ in 0..3 {
        s.poll(&mut p);
    }
    let mut parser = Parser::new();
    let mut offset = 0;
    for (kind, payload) in [
        (Kind::Text, &b"one"[..]),
        (Kind::Text, &b"two"[..]),
        (Kind::Binary, &chunk),
    ] {
        let (used, meta) = parser.decode(&p.output[offset..]);
        offset += used;
        let meta = meta.unwrap().unwrap();
        assert_eq!(meta.kind, kind);
        assert_eq!(parser.payload(meta), payload);
    }
    assert_eq!(offset, p.output.len());
}

#[test]
fn folded_msf1_crc_matches_both_original_checksums() {
    use crate::wire::{self, Kind};

    let history = b"bytes from earlier chunks";
    let previous = !wire::crc32(history);
    let data: Vec<u8> = (0..16384).map(|i| ((i * 137) ^ (i >> 3)) as u8).collect();
    for size in [1, 3, 4, 7, 8, 55, 4096, 16356, 16384] {
        for prefix_len in 0..=3.min(size) {
            let body = &data[..size];
            let mut chunks = body[prefix_len..].chunks_exact(4);
            let words: Vec<u32> = chunks
                .by_ref()
                .map(|part| u32::from_ne_bytes(part.try_into().unwrap()))
                .collect();
            let raw = wire::bulk_raw_crc(&body[..prefix_len], &words, chunks.remainder());
            let mut file = history.to_vec();
            file.extend_from_slice(body);
            assert_eq!(!wire::append_raw(previous, raw, size), wire::crc32(&file));

            if size <= wire::MAX - 8 {
                let mut payload = b"MSF1\x10\x23\x45\x67".to_vec();
                payload.extend_from_slice(body);
                let mut original = vec![0; wire::FRAME];
                let mut folded = original.clone();
                let used = wire::encode(&mut original, Kind::Binary, 17, &payload);
                folded[16..16 + payload.len()].copy_from_slice(&payload);
                wire::finish_encode_with_crc(
                    &mut folded,
                    Kind::Binary,
                    17,
                    payload.len(),
                    Some(raw),
                );
                assert_eq!(&folded[..used], &original[..used]);
            }
        }
    }
}
