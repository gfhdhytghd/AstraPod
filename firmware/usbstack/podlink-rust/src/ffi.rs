// SPDX-License-Identifier: GPL-2.0-or-later
//! The only unsafe Rust module. See AUDIT.md for pointer, locking and lifetime contracts.
use crate::{
    service::{Ports, Service, TimeMailbox},
    session::{Session, State, Transport},
    wire::{self, Kind, Meta, Parser, FRAME, MAX},
};
use core::{
    cell::UnsafeCell,
    ffi::{c_int, c_uint, c_ulong, c_void},
    slice,
};
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Message {
    kind: u8,
    flags: u8,
    seq: u32,
    len: u32,
    data: *const u8,
}
type Background = unsafe extern "C" fn(*const Message);
type Receive = unsafe extern "C" fn(*const Message) -> bool;
type Next = unsafe extern "C" fn(*mut u8, c_uint) -> c_uint;
type NextBinary = unsafe extern "C" fn(*mut u8, c_uint, *mut u32) -> c_uint;
type Reset = unsafe extern "C" fn();
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Registry {
    receive: Option<Receive>,
    next: Option<Next>,
    reset: Option<Reset>,
    next_binary: Option<NextBinary>,
}
struct Adapter {
    registry: Registry,
    background: Option<Background>,
}
struct Context {
    service: Service,
    ports: Adapter,
}
struct Shared(UnsafeCell<Context>);
// Rockbox service mutex protects every Context access, including registration.
unsafe impl Sync for Shared {}
static CTX: Shared = Shared(UnsafeCell::new(Context {
    service: Service::new(),
    ports: Adapter {
        registry: Registry {
            receive: None,
            next: None,
            reset: None,
            next_binary: None,
        },
        background: None,
    },
}));
// Kept outside Context: worker borrows never invalidate the C payload pointer.
struct Delivery(UnsafeCell<[u8; MAX]>);
unsafe impl Sync for Delivery {}
static DELIVERY: Delivery = Delivery(UnsafeCell::new([0; MAX]));
struct Clock(UnsafeCell<TimeMailbox>);
// All mailbox accesses mask IRQs on this single-core firmware target.
unsafe impl Sync for Clock {}
static CLOCK: Clock = Clock(UnsafeCell::new(TimeMailbox::new()));
struct IrqGuard(c_int);
impl Drop for IrqGuard {
    fn drop(&mut self) {
        unsafe { pfu_irq_restore(self.0) }
    }
}
fn clock<T>(f: impl FnOnce(&mut TimeMailbox) -> T) -> T {
    let _guard = IrqGuard(unsafe { pfu_irq_save() });
    f(unsafe { &mut *CLOCK.0.get() })
}
extern "C" {
    fn pfu_irq_save() -> c_int;
    fn pfu_irq_restore(level: c_int);
    fn pfu_init() -> bool;
    fn pfu_start() -> bool;
    fn pfu_lock() -> bool;
    fn pfu_unlock();
    fn pfu_snapshot(state: c_int, session: c_int, connected: bool, host: bool, generation: u32);
    fn pfu_state() -> c_int;
    fn pfu_session_state() -> c_int;
    fn pfu_host_connected() -> bool;
    fn pfu_session_connected() -> bool;
    fn pfu_tick() -> i32;
    fn pfu_hz() -> i32;
    fn pfu_wait(ticks: c_int);
    fn podlink_wake();
    fn pfu_panic() -> !;
    fn usb_podlink_open() -> bool;
    fn usb_podlink_close();
    fn usb_podlink_active() -> bool;
    fn usb_podlink_disk();
    fn usb_serial_get_state(generation: *mut u32) -> bool;
    fn usb_serial_generation_current(generation: u32) -> bool;
    fn usb_serial_read(data: *mut u8, len: c_int) -> c_int;
    fn usb_serial_write(data: *const u8, len: c_int) -> c_int;
    fn usb_serial_wait(ticks: c_int);
    fn pfu_redraw();
    fn remote_log_enabled() -> bool;
    fn remote_log_epoch() -> c_uint;
    fn remote_log_read(data: *mut u8, len: c_uint) -> c_uint;
}
#[cfg(not(test))]
#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    unsafe { pfu_panic() }
}
fn message(m: Meta, data: &[u8]) -> Message {
    Message {
        kind: m.kind as u8,
        flags: 0,
        seq: m.seq,
        len: m.len as u32,
        data: data.as_ptr(),
    }
}
impl Transport for Adapter {
    fn open(&mut self) -> bool {
        unsafe { usb_podlink_open() }
    }
    fn close(&mut self) {
        unsafe { usb_podlink_close() }
    }
    fn usb_state(&mut self) -> Option<u32> {
        let mut g = 0;
        unsafe {
            if usb_serial_get_state(&mut g) {
                Some(g)
            } else {
                None
            }
        }
    }
    fn read(&mut self, out: &mut [u8]) -> usize {
        unsafe { usb_serial_read(out.as_mut_ptr(), out.len() as c_int).max(0) as usize }
            .min(out.len())
    }
    fn write(&mut self, data: &[u8]) -> usize {
        unsafe { usb_serial_write(data.as_ptr(), data.len() as c_int).max(0) as usize }
            .min(data.len())
    }
    fn tick(&self) -> i32 {
        unsafe { pfu_tick() }
    }
    fn hz(&self) -> i32 {
        unsafe { pfu_hz() }
    }
    fn wait(&mut self, ticks: i32) {
        unsafe { usb_serial_wait(ticks) }
    }
}
impl Ports for Adapter {
    fn active(&self) -> bool {
        unsafe { usb_podlink_active() }
    }
    fn connected(&self, s: &Session) -> bool {
        s.leased
            && s.had_usb
            && s.state == State::Connected
            && unsafe { usb_serial_generation_current(s.transport_generation) }
    }
    fn disk(&mut self) {
        unsafe { usb_podlink_disk() }
    }
    fn store_time(&mut self, data: &[u8], tick: u32) {
        if clock(|m| m.publish(data, tick)) {
            unsafe { pfu_redraw() }
        }
    }
    fn clear_time(&mut self) {
        clock(|m| m.clear());
    }
    fn receive(&mut self, m: Meta, data: &[u8]) -> bool {
        match self.registry.receive {
            Some(f) => unsafe { f(&message(m, data)) },
            None => false,
        }
    }
    fn next_record(&mut self, out: &mut [u8]) -> usize {
        match self.registry.next {
            Some(f) => unsafe { f(out.as_mut_ptr(), out.len() as c_uint) as usize }.min(out.len()),
            None => 0,
        }
    }
    fn next_binary(&mut self, out: &mut [u8]) -> (usize, Option<u32>) {
        match self.registry.next_binary {
            Some(f) => {
                let mut raw = 0;
                let n = unsafe { f(out.as_mut_ptr(), out.len() as c_uint, &mut raw) };
                (n as usize, Some(raw))
            }
            None => (0, None),
        }
    }
    fn reset_consumer(&mut self) {
        if let Some(f) = self.registry.reset {
            unsafe { f() }
        }
    }
    fn background(&mut self, m: Meta, data: &[u8]) {
        if let Some(f) = self.background {
            unsafe { f(&message(m, data)) }
        }
    }
    fn log_enabled(&self) -> bool {
        unsafe { remote_log_enabled() }
    }
    fn log_epoch(&self) -> u32 {
        unsafe { remote_log_epoch() }
    }
    fn log_read(&mut self, out: &mut [u8]) -> usize {
        unsafe { remote_log_read(out.as_mut_ptr(), out.len() as c_uint) as usize }.min(out.len())
    }
}
// Guard releases after the last exclusive borrow. Foreign callbacks must not
// reenter state APIs (the same rule as C). Pure CRC/codec calls are independent.
struct Guard;
impl Drop for Guard {
    fn drop(&mut self) {
        unsafe { pfu_unlock() }
    }
}
fn with<T>(default: T, f: impl FnOnce(&mut Service, &mut Adapter) -> T) -> T {
    if !unsafe { pfu_lock() } {
        return default;
    }
    let _guard = Guard;
    let c = unsafe { &mut *CTX.0.get() };
    let result = f(&mut c.service, &mut c.ports);
    let s = &c.service.session;
    unsafe {
        pfu_snapshot(
            c.service.state() as c_int,
            s.state as c_int,
            c.ports.connected(s),
            c.service.running && c.service.started,
            s.transport_generation,
        )
    };
    result
}
// Raw inputs are trusted C allocations, but lengths from the wire never reach
// this boundary unchecked. NULL plus zero denotes an empty slice.
unsafe fn input<'a>(data: *const c_void, len: c_uint) -> Option<&'a [u8]> {
    if len as usize > MAX || (len != 0 && data.is_null()) {
        return None;
    }
    Some(if len == 0 {
        &[]
    } else {
        slice::from_raw_parts(data.cast(), len as usize)
    })
}
unsafe fn deliver(out: *mut Message, m: Option<Meta>, data: *const u8) -> c_int {
    match m {
        Some(m) => {
            out.write(Message {
                kind: m.kind as u8,
                flags: 0,
                seq: m.seq,
                len: m.len as u32,
                data,
            });
            1
        }
        None => 0,
    }
}
#[no_mangle]
pub extern "C" fn podlink_init() {
    if !unsafe { pfu_init() } {
        return;
    }
    with((), |s, _| s.running = true);
    if !unsafe { pfu_start() } {
        with((), |s, _| s.running = false);
    }
}
#[no_mangle]
pub extern "C" fn podlink_rust_step() {
    with((), |s, p| s.step(p));
}
#[no_mangle]
pub extern "C" fn podlink_shutdown() {
    with((), |s, p| {
        s.shutdown(p);
        p.registry = Registry::default();
        p.background = None;
    });
}
#[no_mangle]
/// # Safety
/// The C caller must satisfy the matching pointer and lifetime contracts in AUDIT.md.
pub unsafe extern "C" fn podlink_set_resident(reg: *const Registry) {
    with((), |s, p| {
        p.registry = if reg.is_null() {
            Registry::default()
        } else {
            reg.read()
        };
        s.clear_record();
    });
}
#[no_mangle]
pub extern "C" fn podlink_open() -> bool {
    with(false, |s, p| s.attach(2, p))
}
#[no_mangle]
pub extern "C" fn podlink_open_agents(background: Option<Background>) -> bool {
    with(false, |s, p| {
        let ok = s.attach(1, p);
        if ok && background.is_some() {
            p.background = background;
        }
        ok
    })
}
#[no_mangle]
pub extern "C" fn podlink_close() {
    with((), |s, p| s.detach(p));
}
#[no_mangle]
pub extern "C" fn podlink_state() -> c_int {
    unsafe { pfu_state() }
}
#[no_mangle]
pub extern "C" fn podlink_host_connected() -> bool {
    unsafe { pfu_host_connected() }
}
#[no_mangle]
pub extern "C" fn podlink_generation() -> u32 {
    with(0, |s, _| s.generation)
}
#[no_mangle]
pub extern "C" fn podlink_max_message() -> c_uint {
    with(MAX as u32, |s, _| s.session.peer_max as c_uint)
}
#[no_mangle]
/// # Safety
/// The C caller must satisfy the matching pointer and lifetime contracts in AUDIT.md.
pub unsafe extern "C" fn podlink_send(kind: c_uint, data: *const c_void, len: c_uint) -> c_int {
    let result = with(0, |s, _| match (Kind::parse(kind), input(data, len)) {
        (Some(k), Some(d)) => s.send(k, d),
        _ => -1,
    });
    if result == 1 {
        podlink_wake();
    }
    result
}
#[no_mangle]
/// # Safety
/// The C caller must satisfy the matching pointer and lifetime contracts in AUDIT.md.
pub unsafe extern "C" fn podlink_poll(out: *mut Message) -> c_int {
    if out.is_null() {
        return 0;
    }
    with(0, |s, _| {
        let delivered = &mut *DELIVERY.0.get();
        let m = s.poll(delivered);
        deliver(out, m, delivered.as_ptr())
    })
}
#[no_mangle]
/// # Safety
/// The C caller must satisfy the matching pointer and lifetime contracts in AUDIT.md.
pub unsafe extern "C" fn podlink_take_time(out: *mut u8, tick: *mut c_ulong) -> bool {
    if out.is_null() || tick.is_null() {
        return false;
    }
    clock(|m| match m.take(&mut *out.cast::<[u8; 64]>()) {
        Some(t) => {
            tick.write(t as c_ulong);
            true
        }
        None => false,
    })
}
#[no_mangle]
pub extern "C" fn podlink_wait(ticks: c_int) {
    unsafe { pfu_wait(ticks) }
}
#[no_mangle]
pub extern "C" fn podlink_session_open() -> bool {
    with(false, |s, p| s.session.open(p))
}
#[no_mangle]
pub extern "C" fn podlink_session_state() -> c_int {
    unsafe { pfu_session_state() }
}
#[no_mangle]
pub extern "C" fn podlink_session_host_connected() -> bool {
    unsafe { pfu_session_connected() }
}
#[no_mangle]
pub extern "C" fn podlink_session_max_message() -> c_uint {
    with(0, |s, _| s.session.peer_max as c_uint)
}
#[no_mangle]
/// # Safety
/// The C caller must satisfy the matching pointer and lifetime contracts in AUDIT.md.
pub unsafe extern "C" fn podlink_session_send(
    kind: c_uint,
    data: *const c_void,
    len: c_uint,
) -> c_int {
    with(0, |s, _| match (Kind::parse(kind), input(data, len)) {
        (Some(k), Some(d)) => s.session.send(k, d),
        _ => -1,
    })
}
#[no_mangle]
/// # Safety
/// The C caller must satisfy the matching pointer and lifetime contracts in AUDIT.md.
pub unsafe extern "C" fn podlink_session_poll(out: *mut Message) -> c_int {
    if out.is_null() {
        return 0;
    }
    with(0, |s, p| {
        let m = s.session.poll(p);
        if let Some(m) = m {
            (&mut *DELIVERY.0.get())[..m.len].copy_from_slice(&s.session.event[..m.len]);
        }
        deliver(out, m, (*DELIVERY.0.get()).as_ptr())
    })
}
#[no_mangle]
pub extern "C" fn podlink_session_close() {
    with((), |s, p| s.session.close(p));
}
#[no_mangle]
pub extern "C" fn podlink_session_abort() {
    with((), |s, p| s.session.abort(p));
}
#[no_mangle]
pub extern "C" fn podlink_session_wait(ticks: c_int) {
    with((), |s, p| s.session.wait(p, ticks));
}
#[no_mangle]
pub extern "C" fn podlink_session_revision() -> u32 {
    with(0, |s, _| s.session.revision)
}
#[no_mangle]
/// # Safety
/// The C caller must satisfy the matching pointer and lifetime contracts in AUDIT.md.
pub unsafe extern "C" fn podlink_crc32(data: *const u8, len: c_uint) -> u32 {
    if len == 0 {
        return wire::crc32(&[]);
    }
    if data.is_null() || len as usize > isize::MAX as usize {
        return 0;
    }
    wire::crc32(slice::from_raw_parts(data, len as usize))
}
#[no_mangle]
/// # Safety
/// The C caller must satisfy the matching pointer and lifetime contracts in AUDIT.md.
pub unsafe extern "C" fn podlink_encode(
    dest: *mut u8,
    kind: c_uint,
    seq: u32,
    data: *const c_void,
    len: c_uint,
) -> c_uint {
    if dest.is_null() {
        return 0;
    }
    match (Kind::parse(kind), input(data, len)) {
        (Some(k), Some(d)) => wire::encode(
            slice::from_raw_parts_mut(dest, len as usize + 20),
            k,
            seq,
            d,
        ) as c_uint,
        _ => 0,
    }
}
#[no_mangle]
/// # Safety
/// The C caller must satisfy the matching pointer and lifetime contracts in AUDIT.md.
pub unsafe extern "C" fn podlink_decode_bytes(
    parser: *mut Parser,
    data: *const u8,
    len: c_uint,
    consumed: *mut c_uint,
    out: *mut Message,
) -> c_int {
    if consumed.is_null() {
        return -1;
    }
    consumed.write(0);
    if parser.is_null()
        || out.is_null()
        || (len != 0 && data.is_null())
        || len as usize > isize::MAX as usize
    {
        return -1;
    }
    let p = &mut *parser;
    let d = if len == 0 {
        &[]
    } else {
        slice::from_raw_parts(data, len as usize)
    };
    let (used, result) = p.decode(d);
    consumed.write(used as c_uint);
    match result {
        Err(e) => e,
        Ok(m) => deliver(out, m, p.bytes.as_ptr().add(16)),
    }
}
#[no_mangle]
/// # Safety
/// The C caller must satisfy the matching pointer and lifetime contracts in AUDIT.md.
pub unsafe extern "C" fn podlink_decode(parser: *mut Parser, byte: u8, out: *mut Message) -> c_int {
    if parser.is_null() || out.is_null() {
        return -1;
    }
    // The single-byte C API does not drain recovery bytes before appending.
    let p = &mut *parser;
    p.compact();
    if p.used as usize >= FRAME {
        p.reset();
        return -1;
    }
    p.bytes[p.used as usize] = byte;
    p.used += 1;
    match p.buffered() {
        Err(e) => e,
        Ok(m) => deliver(out, m, p.bytes.as_ptr().add(16)),
    }
}
const _: () = assert!(Service::inbox_ram() <= 51000);

// Host fixtures only; absent from firmware archives.
#[cfg(feature = "host-test")]
#[no_mangle]
pub extern "C" fn podlink_rust_debug(id: u32) -> u32 {
    if id == 16 {
        return clock(|m| m.pending() as u32);
    }
    with(0, |s, _| s.debug(id))
}
#[cfg(feature = "host-test")]
#[no_mangle]
pub extern "C" fn podlink_rust_set_peer(n: u32) {
    with((), |s, _| s.session.peer_max = (n as usize).min(MAX));
}
#[cfg(feature = "host-test")]
#[no_mangle]
/// # Safety
/// The C caller must satisfy the matching pointer and lifetime contracts in AUDIT.md.
pub unsafe extern "C" fn podlink_rust_log(out: *mut u8) {
    with((), |s, _| s.debug_log(slice::from_raw_parts_mut(out, 768)));
}
#[cfg(feature = "host-test")]
#[no_mangle]
pub extern "C" fn podlink_rust_invalidate() {
    with((), |s, p| s.invalidate(p));
}

#[cfg(target_pointer_width = "32")]
const _: () = {
    assert!(core::mem::size_of::<Message>() == 16);
    assert!(core::mem::size_of::<Registry>() == 16);
    assert!(core::mem::size_of::<Parser>() == FRAME + 8);
};

#[cfg(not(feature = "host-test"))]
pub(crate) fn profile_tx_crc(data: &[u8], raw: Option<u32>) -> u32 {
    unsafe extern "C" {
        fn podlink_profile_now() -> u32;
        fn podlink_profile_frame_crc(start: u32, bytes: u32);
    }
    let start = unsafe { podlink_profile_now() };
    let crc = wire::frame_crc(data, raw);
    if start != 0 {
        unsafe { podlink_profile_frame_crc(start, data.len() as u32) };
    }
    crc
}

#[no_mangle]
/// # Safety
/// For 1..=MAX bytes, data must be readable and raw must point to a writable u32.
pub unsafe extern "C" fn podlink_bulk_crc(
    data: *const u8,
    len: c_uint,
    previous: u32,
    raw: *mut u32,
) -> u32 {
    if data.is_null() || raw.is_null() || len == 0 || len as usize > MAX {
        return previous;
    }
    let bytes = slice::from_raw_parts(data, len as usize);
    // All bit patterns are valid in u32; align_to leaves unaligned ends as bytes.
    let (prefix, words, suffix) = bytes.align_to::<u32>();
    let chunk = wire::bulk_raw_crc(prefix, words, suffix);
    raw.write(chunk);
    wire::append_raw(previous, chunk, bytes.len())
}
