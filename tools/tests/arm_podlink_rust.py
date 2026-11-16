#!/usr/bin/env python3
"""Execute the linked ARM Rust worker, codec, FFI and snapshots in Unicorn.

Only scheduler/USB/log/consumer ports are deterministic adapters. Samples SP
at every instruction; no boot, real scheduler, MMIO or device access. Consumer
adapter stack is excluded; this is a bounded fixture, not a universal bound.
"""
import argparse,json,struct,zlib
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_CPSR,UC_ARM_REG_SP,UC_ARM_REG_LR,UC_ARM_REG_PC,UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--elf',type=Path,required=True)
p.add_argument('--budget',type=int,default=2048)
a=p.parse_args()
u=Uc(UC_ARCH_ARM,UC_MODE_ARM);u.reg_write(UC_ARM_REG_CPSR,0xdf)
u.mem_map(0,0x10000);u.mem_map(0x08000000,0x4000000);u.mem_map(0x10000000,0x200000)
symbols={}
with a.elf.open('rb') as f:
    e=ELFFile(f)
    for seg in e.iter_segments():
        if seg['p_type']=='PT_LOAD':u.mem_write(seg['p_vaddr'],seg.data())
    for s in e.get_section_by_name('.symtab').iter_symbols():
        if s.name:symbols[s.name]=s['st_value']
stop=0x10050000;stacktop=0x10110000;minimum=stacktop;locked=False
rx=bytearray();tx=bytearray();callbacks=[];disk=0;redraw=0;now=100;usb=True;generation=1
receive=0x10051000;background=0x10051010;reset=0x10051020;next_record=0x10051030
calls={};pending_record=True
ports=['pfu_init','pfu_start','pfu_lock','pfu_unlock','usb_podlink_active','usb_podlink_open','usb_podlink_close','usb_podlink_disk','usb_serial_get_state','usb_serial_generation_current','usb_serial_read','usb_serial_write','usb_serial_wait','pfu_tick','pfu_hz','pfu_redraw','remote_log_enabled','remote_log_epoch','remote_log_read','panicf']
byaddr={symbols[n]:n for n in ports}
byaddr|={receive:'receive',background:'background',reset:'reset',next_record:'next'}

def word(at,n):u.mem_write(at,struct.pack('<I',n&0xffffffff))
def ret(n=0):u.reg_write(UC_ARM_REG_R0,n&0xffffffff);u.reg_write(UC_ARM_REG_PC,u.reg_read(UC_ARM_REG_LR))
def hook(uc,address,size,_):
    global minimum,locked,disk,redraw,pending_record
    minimum=min(minimum,uc.reg_read(UC_ARM_REG_SP))
    assert address!=0,'NULL instruction pointer'
    if address==stop:uc.emu_stop();return
    n=byaddr.get(address)
    if not n:return
    calls[n]=calls.get(n,0)+1
    r0,r1,r2=[uc.reg_read(r) for r in [UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2]]
    if n=='panicf':raise AssertionError(('unexpected panic',bytes(u.mem_read(r0,80))))
    if n in ('pfu_init','pfu_start','usb_podlink_open'):ret(1)
    elif n=='pfu_lock':assert not locked;locked=True;ret(1)
    elif n=='pfu_unlock':assert locked;locked=False;ret()
    elif n=='usb_podlink_active':ret(1)
    elif n=='usb_podlink_close':ret()
    elif n=='usb_serial_get_state':word(r0,generation);ret(usb)
    elif n=='usb_serial_generation_current':ret(usb and r0==generation)
    elif n=='usb_serial_read':
        count=min(r1,len(rx));u.mem_write(r0,bytes(rx[:count]));del rx[:count];ret(count)
    elif n=='usb_serial_write':tx.extend(u.mem_read(r0,r1));ret(r1)
    elif n in ('usb_serial_wait','remote_log_enabled','remote_log_epoch','remote_log_read'):ret()
    elif n=='usb_podlink_disk':disk+=1;ret()
    elif n=='pfu_redraw':redraw+=1;ret()
    elif n=='pfu_tick':ret(now)
    elif n=='pfu_hz':ret(100)
    elif n in ('receive','background'):
        assert locked
        kind,flags,seq,length,data=struct.unpack('<BB2xIII',u.mem_read(r0,16))
        assert length<=16384
        payload=bytes(u.mem_read(data,length)) if length else b''
        callbacks.append((n,kind,seq,payload));ret(n=='receive' and (payload.startswith(b'CAR1') or payload.startswith(b'CXS1')))
    elif n=='reset':assert locked;ret()
    elif n=='next':
        assert locked
        record=b'cf\t1\tplay' if pending_record else b'';pending_record=False
        assert r1>=len(record);u.mem_write(r0,record);ret(len(record))
    else:raise AssertionError(n)
u.hook_add(UC_HOOK_CODE,hook)

def call(name,*args):
    u.reg_write(UC_ARM_REG_SP,stacktop);u.reg_write(UC_ARM_REG_LR,stop)
    for reg,value in zip([UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3],args):u.reg_write(reg,value)
    u.emu_start(symbols[name],stop+4,count=5_000_000)
    assert u.reg_read(UC_ARM_REG_PC)==stop,(name,hex(u.reg_read(UC_ARM_REG_PC)))
    assert u.reg_read(UC_ARM_REG_SP)==stacktop and not locked,name
    return u.reg_read(UC_ARM_REG_R0)
def frame(kind,seq,payload):
    body=struct.pack('<BBHII',kind,0,0,seq,len(payload))+payload
    return b'PLNK'+body+struct.pack('<I',zlib.crc32(body))
def step(n=3):
    for _ in range(n):call('podlink_rust_step')
def send(kind,payload,seq=5):rx.extend(frame(kind,seq,payload));step()
def messages():
    data=bytes(tx);pos=0;out=[]
    while pos<len(data):
        assert data[pos:pos+4]==b'PLNK';kind,flags,reserved,seq,length=struct.unpack_from('<BBHII',data,pos+4)
        assert length<=16384 and flags==0 and reserved==0
        end=pos+16+length;assert zlib.crc32(data[pos+4:end])==struct.unpack_from('<I',data,end)[0]
        out.append((kind,seq,data[pos+16:end]));pos=end+4
    return out
call('podlink_init');step();send(1,b'\1\0\0\x40\0\0',1)
assert call('podlink_host_connected')==1
registry=0x10020000
u.mem_write(registry,struct.pack('<III',receive,next_record,reset));call('podlink_set_resident',registry)
u.mem_write(registry,b'\0'*12) # caller table dies; Rust keeps a typed copy
assert call('podlink_open_agents',background)==1
send(2,b'HR\t1\thost');send(2,b'W\t1\nw1\t-\t1\t0\t0\t0\tCached')
# Full-size art, binary credit data, TS mailbox and foreground overflow.
send(3,b'CAR1'+bytes((i%251 for i in range(16380))))
send(3,b'CXS1'+bytes(4092))
send(2,b'TS\t1\t2026-11-16T10:00:00\t-18000');assert redraw==1
out=0x10021000
for _ in range(32):send(2,b'delta')
assert call('podlink_poll',out)==1
kind,flags,seq,length,data=struct.unpack('<BB2xIII',u.mem_read(out,16));saved=bytes(u.mem_read(data,length))
send(2,b'later');assert bytes(u.mem_read(data,length))==saved
# One-shot disk consent remains after inbox pressure, rejects replay.
token=b'0123456789abcdef0123456789abcdef'
send(2,b'DA\t1\t'+token);send(2,b'DK\t1\t'+token);send(2,b'DK\t1\t'+token);assert disk==1
call('podlink_close');send(2,b'W\t0');assert any(c[0]=='background' for c in callbacks)
# NULL registry slots, unregister, CRC error recovery, oversized header.
call('podlink_set_resident',0);before=len([c for c in callbacks if c[0]=='receive'])
send(3,b'CAR1xxxx');assert len([c for c in callbacks if c[0]=='receive'])==before
bad=bytearray(frame(2,9,b'bad'));bad[-1]^=1;rx.extend(bad+frame(2,10,b'recovery'));step()
assert any(c[3]==b'recovery' for c in callbacks)
rx.extend(b'PLNK'+struct.pack('<BBHII',3,0,0,99,0xffffffff));step()
assert any(m[0]==6 and m[2][:2]==struct.pack('<H',1009) for m in messages())
# OPEN and keepalive expiry plus USB generation invalidation.
send(1,b'\1\0\0\x10\0\0',20);now+=601;step();now+=1501;step()
assert any(m[0]==6 and m[2][:2]==struct.pack('<H',1001) for m in messages())
usb=False;generation+=1;assert call('podlink_host_connected')==0;step()
used=stacktop-minimum
print(json.dumps(dict(elf=str(a.elf),stack_bytes=used,budget_bytes=a.budget,within_budget=used<=a.budget,callbacks=len(callbacks),disk_requests=disk,tx_frames=len(messages()),ports=calls,scope='real linked ARM Rust worker/codec/FFI/snapshot instructions; deterministic scheduler/USB/log/C-consumer adapters; consumer stack excluded; no physical device')))
assert used<=a.budget
