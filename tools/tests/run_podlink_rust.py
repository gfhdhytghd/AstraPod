#!/usr/bin/env python3
"""Build C/Rust copies of existing RAM-only assertion streams; compare traces.

All generated code, binaries and traces stay in --build. Never opens a device.
Internal C test observations use host-test getters in Rust; product APIs and
all input/output vectors are unchanged. C sanitizers remain enabled.
"""
import argparse, os, re, subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,default=Path('build-fu48'))
a=p.parse_args();root=Path(__file__).resolve().parents[2];os.chdir(root)
b=a.build.resolve();d=b/'host';d.mkdir(parents=True,exist_ok=True)
manifest=root/'firmware/usbstack/podlink-rust/Cargo.toml'
subprocess.run(['cargo','+nightly','build','--manifest-path',str(manifest),'--release','--features','host-test','--target-dir',str(b/'host-target')],check=True)
lib=b/'host-target/release/libpodlink_rust.a'
trace_header='''#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
static FILE *trace_file;
static void trace(unsigned tag,const void *data,unsigned n) {
 if(!trace_file) {trace_file=fopen(getenv("PODLINK_TRACE"),"wb");if(!trace_file)abort();}
 fwrite(&tag,4,1,trace_file);fwrite(&n,4,1,trace_file);if(n)fwrite(data,1,n,trace_file);
}
'''
# Generate two variants; this preserves a reviewable source copy in each run.
firm=(root/'tools/tests/podlink_firmware_test.c').read_text()
firm=trace_header+firm
firm=firm.replace('    memcpy(output + output_size, data, size);','    trace(1,data,size);\n    memcpy(output + output_size, data, size);')
firm=firm.replace('    return n;\n}', '''    unsigned snapshot[]={n,podlink_state(),podlink_max_message()};
    trace(2,snapshot,sizeof snapshot);
    if(n){unsigned meta[]={m->type,m->flags,m->seq,m->length};trace(3,meta,sizeof meta);trace(4,m->data,m->length);}
    return n;
}''',1)
# Randomized session sequences use the same serial/controller fixture too.
fuzz_session=r"""
static unsigned fuzz_seed=0xf048;
static unsigned fuzz_random(void) {fuzz_seed^=fuzz_seed<<13;fuzz_seed^=fuzz_seed>>17;fuzz_seed^=fuzz_seed<<5;return fuzz_seed;}
static void fuzz_sessions(void) {
 for(unsigned round=0;round<256;round++) {
  clear_output();test_high_speed=round%2;assert(podlink_open());insert();
  struct podlink_message m;poll_message(&m);handshake(round%2?4096:16384,round+1);
  for(unsigned turn=0;turn<8;turn++) {
   unsigned char frame[128],data[64];unsigned n=fuzz_random()%65;
   for(unsigned j=0;j<n;j++)data[j]=(unsigned char)fuzz_random();
   unsigned size=podlink_encode(frame,1+fuzz_random()%7,fuzz_random(),data,n);
   if(turn%4==0)frame[12]=255,frame[13]=255;
   if(turn%4==1)frame[size-1]^=128;
   if(turn%4==2)frame[5]=1;
   input_bytes(frame,size);
   for(int j=0;j<12;j++)poll_message(&m);
   if(turn%3==0)current_tick+=601;
  }
  podlink_close();current_tick+=101;poll_message(&m);remove_usb();
 }
 puts("PASS: 256 seeded randomized session streams at HS/FS");
}
"""
firm=firm.replace('int main(void)',fuzz_session+'\nint main(void)',1)
firm='    fuzz_sessions();\n    return 0;\n}'.join(firm.rsplit('    return 0;\n}',1))
resident=(root/'tools/tests/podlink_resident_test.c').read_text()
resident=resident.replace('    else if (m->type == PODLINK_TEXT)','    else if (m->type == PODLINK_TEXT)',1)
resident=resident.replace('    if (m->type == PODLINK_CLOSE) losses++;','    unsigned meta[]={m->type,m->flags,m->seq,m->length};trace(5,meta,sizeof meta);trace(6,m->data,m->length);\n    if (m->type == PODLINK_CLOSE) losses++;')
resident=resident.replace('        service_step();controller();current_tick++;','''        service_step();controller();current_tick++;
        unsigned snap[]={podlink_state(),podlink_host_connected(),podlink_generation(),podlink_session_state(),podlink_session_revision()};trace(7,snap,sizeof snap);''')
# Synthetic recorded-shape Cider sequences exercise resident consumption and
# its outgoing command port alongside the original art/Agents/disk vectors.
resident=resident.replace('static bool dedicated_art(const struct podlink_message *m)\n{',r'''static unsigned cider_records;
static unsigned int cider_next(char *out,unsigned int capacity) {
 static bool sent;const char *record="cf\t1\tplay";unsigned n=strlen(record);
 if(sent || capacity<n)return 0;
 sent=true;memcpy(out,record,n);return n;
}
static bool dedicated_art(const struct podlink_message *m)
{
 if((m->type==PODLINK_TEXT && m->length>=3 && m->data[0]=='C') ||
    (m->type==PODLINK_BINARY && m->length>=4 && !memcmp(m->data,"CXS1",4))) {
   cider_records++;trace(8,m->data,m->length);return true;
 }
''')
resident=resident.replace('dedicated_art, NULL, resident_reset','dedicated_art, cider_next, resident_reset')
resident=resident.replace('    unsigned char art_frame[4096]',r'''    const char *cider[]={"CA\t1\tplaying","CN\t1\tSong","CL\t1\t0\t1\tSong","CK\t1\tok","CXS1xxxx"};
    for(unsigned i=0;i<5;i++){input_frame(i==4?PODLINK_BINARY:PODLINK_TEXT,700+i,cider[i],strlen(cider[i]));work();}
    assert(cider_records==5);
    unsigned char art_frame[4096]''')
resident=resident.replace('podlink_poll(', 'traced_client_poll(')
resident=resident.replace('#include "herdremote_model.h"',r'''#include "herdremote_model.h"
static int traced_client_poll(struct podlink_message *m) {
 int have=podlink_poll(m);trace(9,&have,sizeof have);
 if(have){unsigned meta[]={m->type,m->flags,m->seq,m->length};trace(10,meta,sizeof meta);trace(11,m->data,m->length);}
 return have;
}''')
# The original service fixture inspects private fields. Replace only those
# observations with getters; keep the original C source for the reference.
for mode in ['c','rust']:
    out=d/mode;out.mkdir(exist_ok=True)
    f=firm;r=resident
    if mode=='rust':
        # Adapter must follow usb_serial.c and precede the session-name macros.
        f=f.replace('#define podlink_open podlink_session_open','#include "'+str(root/'tools/tests/podlink_rust_host.h')+'"\n#define podlink_open podlink_session_open')
        f=f.replace('#include "../../firmware/usbstack/podlink.c"','')
        f=f.replace('parser.used','podlink_rust_debug(0)')
        for name,idx in [('count',1),('input_pos',2),('input_length',3),('ping_pending',4)]:
            # Those are session internals; serial names have different tokens.
            f=re.sub(r'(?<![.\w])'+name+r'\b',f'podlink_rust_debug({idx})',f)
        r=r.replace('#include "../../firmware/usbstack/podlink_service.c"','''#include "remote_log.h"
static char log_line[768];
static void service_step(void) {podlink_rust_step();podlink_rust_log((unsigned char *)log_line);}
static void invalidate(void) {podlink_rust_invalidate();}''')
        r=r.replace('uint32_t saved_peer_max=peer_max;peer_max=10;','uint32_t saved_peer_max=podlink_rust_debug(5);podlink_rust_set_peer(10);')
        r=r.replace('peer_max=9;','podlink_rust_set_peer(9);').replace('peer_max=saved_peer_max;','podlink_rust_set_peer(saved_peer_max);')
        r=r.replace('==foreground','==(unsigned)foreground')
        r=r.replace('disk_token[0]','podlink_rust_debug(14)')
        for name,idx in [('client',10),('started',11),('used',12),('byte_used',13),('disk_hello_sent',15),('time_pending',16),('log_length',17)]:
            r=re.sub(r'(?<![.\w])'+name+r'\b',f'podlink_rust_debug({idx})',r)
        # The Rust adapter covers waits. The old C fixture's sleep is unused.
        r=r.replace('static void sleep(int ticks) { current_tick += ticks; }','')
    # Generated sources sit one directory deeper than tools/tests.
    f=f.replace('"../../firmware/', '"'+str(root)+'/firmware/')
    r=r.replace('"../../firmware/', '"'+str(root)+'/firmware/')
    (out/'podlink_firmware_test.c').write_text(f)
    (out/'podlink_resident_test.c').write_text(r)
    bench=(root/'tools/tests/podlink_bench_test.c').read_text().replace('"../../apps-ipod/', '"'+str(root)+'/apps-ipod/')
    if mode=='rust':bench=re.sub(r'(?<![.\w])count\b','podlink_rust_debug(1)',bench)
    (out/'podlink_bench_test.c').write_text(bench)
    for name in ['podlink_firmware','podlink_wire','podlink_bench','podlink_resident']:
        src=out/(name+'_test.c')
        if not src.exists():src=root/'tools/tests'/(name+'_test.c')
        binary=out/name
        cmd=['cc','-std=gnu11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Itools/tests/podlink_stubs','-Ifirmware/export','-Ifirmware/usbstack','-Iapps-ipod/screens/system',str(src),'firmware/usbstack/remote_log.c','apps-ipod/screens/system/herdremote_model.c']
        if mode=='rust':
            if name=='podlink_wire':
                # Wire calls never initialize the service. Unused FFI functions
                # are discarded; only the panic handler needs a tiny adapter.
                panic=out/'panic.c';panic.write_text('#include <stdlib.h>\nvoid pfu_panic(void){abort();}\n')
                cmd.append(str(panic))
            cmd += [str(lib),'-ldl','-lpthread','-lm']
        else:cmd+=['firmware/usbstack/podlink_wire.c']
        cmd+=['-o',str(binary)]
        subprocess.run(cmd,check=True)
        env=os.environ|{'PODLINK_TRACE':str(out/(name+'.trace'))}
        subprocess.run([str(binary)],check=True,env=env)
for name in ['podlink_firmware','podlink_bench','podlink_resident']:
    c=(d/'c'/(name+'.trace')).read_bytes();r=(d/'rust'/(name+'.trace')).read_bytes()
    if c!=r:
        offset=next((i for i,(x,y) in enumerate(zip(c,r)) if x!=y),min(len(c),len(r)))
        raise AssertionError((name,'trace mismatch',offset,len(c),len(r)))
    print(f'PASS differential {name}: {len(c)} byte-identical trace bytes',flush=True)

# Fuzz C is renamed so both codecs execute in the same address space.
codec=d/'c-wire.o'
flags=['-std=gnu11','-g','-fsanitize=address,undefined','-fno-pie','-ffunction-sections','-fdata-sections','-Ifirmware/export']
subprocess.run(['cc',*flags,'-Dpodlink_encode=c_encode','-Dpodlink_decode=c_decode','-Dpodlink_decode_bytes=c_decode_bytes','-Dpodlink_crc32=c_crc32','-c','firmware/usbstack/podlink_wire.c','-o',str(codec)],check=True)
for name in ['podlink_rust_fuzz','podlink_registry_race']:
    cmd=['cc',*flags,'-Wall','-Wextra','-Werror','-no-pie','-Wl,--gc-sections',str(root/'tools/tests'/(name+'.c'))]
    if name=='podlink_rust_fuzz':cmd.append(str(codec))
    binary=d/name
    subprocess.run([*cmd,str(lib),'-lpthread','-ldl','-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=60)
