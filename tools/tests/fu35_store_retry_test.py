"""FU35 unanswered fill, negative caching and resident-cover lookup budget."""
from pathlib import Path
import runpy,subprocess,sys
R=Path(__file__).resolve().parents[2];O=Path(sys.argv[1]).resolve();O.mkdir(parents=True,exist_ok=True)
ns=runpy.run_path(str(R/'tools/tests/cider_pin_test.py'),run_name='fu35_pin')
s=(O/'pin.c').read_text()
body=''.join(ns['fn'](name) for name in ['cider_warm_art','cider_art_local','cider_read_ahead'])
s=s.replace('static char out[2048]',body+'\nstatic char out[2048]')
extra=r'''
 /* No host answers: poll the real next() at 100 Hz for 12 seconds.
  * Work per poll is bounded; exactly three sends, then cancel and cooldown. */
 memset(&cache,0,sizeof cache);memset(&transfer,0,sizeof transfer);
 state.generation=7;host_live=false;host_diag=false;host_store=false;
 pin_on=false;noffers=ncommands=nacks=xs_count=0;
 cancel_request=0;cancel_all=warm_ready=list_ready=ls_due=want_ready=false;
 memset(store.bloom,0,sizeof store.bloom);granted=CIDER_CREDIT;grant=0;
 current_tick=10000;begin_transfer(A,"XS",true);
 int sends=0,cancels=0;long began=current_tick;
 for(int tick=0;tick<1200;tick++){
  current_tick=began+tick;
  unsigned n=next(out,sizeof out);
  if(n&&!strncmp(out,"cr\t",3))sends++;
  if(n&&!strncmp(out,"cx\t",3))cancels++;
 }
 assert(sends==3&&cancels==1&&!transfer.active);
 assert(cider_cache_backoff(&cache,A,"XS",current_tick));
 begin_transfer(A,"XS",true);assert(!transfer.active);
 current_tick+=5*HZ;begin_transfer(A,"XS",true);assert(transfer.active);
 /* Ready covers never need the Bloom filter or disk wakeups per frame. */
 transfer.active=false;memset(&cache,0,sizeof cache);memset(store.bloom,0xff,sizeof store.bloom);
 fill_art(A,"M");fill_art(B,"M");fill_art(C,"M");
 const char*window[]={A,B,C};unsigned before=diag.bloom_hits;int before_posts=posts;
 for(int frame=0;frame<10000;frame++){
  cider_warm_art(window,3,"M");cider_read_ahead(window,3,"M");
  for(int i=0;i<3;i++)assert(cider_art_local(window[i],"M"));
 }
 assert(diag.bloom_hits==before&&posts==before_posts&&nfills==0);
 puts("FU35 unanswered fill: 1200 polls/12s, 3 sends + 1 cancel; 5s backoff; 10000 resident frames: 0 bloom lookups / 0 disk wakeups PASS");
'''
s=s.replace('  puts("Production album pin',extra+'\n  puts("Production album pin')
(O/'retry.c').write_text(s)
cmd=['cc','-std=gnu11','-Wall','-Wextra','-Wno-unused-variable','-Wno-unused-function','-Wno-unused-parameter','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(R/'apps-ipod'),'-I'+str(R/'firmware/export'),str(O/'retry.c'),str(R/'apps-ipod/screens/playback/cider_model.c'),str(R/'apps-ipod/screens/playback/cider_store.c'),str(R/'firmware/usbstack/podlink_wire.c'),'-o',str(O/'retry')]
subprocess.run(cmd,check=True);subprocess.run([str(O/'retry')],check=True,timeout=10)
# Counterfactual: remove only the resident-cover shortcuts, retaining all
# other fixes. Repeated frames must then fail the Bloom budget assertion.
old=s.replace('if (cider_cache_find(&cache, keys[i], tier) >= 0) continue;', '')
old=old.replace('cider_cache_find(&cache, key, tier) >= 0 || store_local(key, tier)', 'store_local(key, tier)')
old=old.replace('cider_cache_find(&cache, keys[i], tier) < 0 &&\n            ', '')
assert old!=s
(O/'lookup-old.c').write_text(old)
oldcmd=cmd.copy();oldcmd[oldcmd.index(str(O/'retry.c'))]=str(O/'lookup-old.c');oldcmd[-1]=str(O/'lookup-old')
subprocess.run(oldcmd,check=True)
oldrun=subprocess.run([str(O/'lookup-old')],capture_output=True,text=True,timeout=10)
(O/'lookup-baseline.log').write_text(oldrun.stdout+oldrun.stderr)
assert oldrun.returncode!=0 and 'diag.bloom_hits' in oldrun.stderr

# The real resident demux must terminate the matching failed art transfer.
runpy.run_path(str(R/'tools/tests/cider_receive_test.py'),run_name='fu35_receive')
s=(O/'receive.c').read_text() if (O/'receive.c').exists() else (O/'test.c').read_text()
extra=r'''
transfer.active=true;transfer.request=987;strcpy(transfer.key,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");strcpy(transfer.tier,"XS");disk_lookup=request_ready=true;
ack="CK\t7\t986\t0\tArtwork unavailable";message.data=(const unsigned char*)ack;message.length=strlen(ack);assert(receive(&message)&&transfer.active);
ack="CK\t7\t987\t0\tArtwork unavailable";message.data=(const unsigned char*)ack;message.length=strlen(ack);assert(receive(&message)&&!transfer.active&&!disk_lookup&&!request_ready);
assert(cider_cache_backoff(&cache,transfer.key,transfer.tier,current_tick));
transfer.active=true;transfer.request=988;xs_count=2;
strcpy(xs_keys[0],transfer.key);memset(xs_keys[1],'b',64);xs_keys[1][64]=0;
ack="CK\t7\t988\t0\tArtwork unavailable";message.data=(const unsigned char*)ack;message.length=strlen(ack);assert(receive(&message)&&!transfer.active&&!xs_count);
assert(cider_cache_backoff(&cache,xs_keys[1],"XS",current_tick));
/* Successful packed XS must also release the background-fill gate. */
transfer.active=true;transfer.request=989;xs_count=1;
memset(art,0,2060);put32(art,8u<<16|0x5441);put32(art+4,32u<<16|32);
memcpy(packet,"CXS1",4);put32(packet+4,7);put32(packet+8,989);put32(packet+12,1);
memset(packet+16,0xcc,32);put32(packet+48,2060);put32(packet+52,podlink_crc32(art,2060));memcpy(packet+56,art,2060);
message=(struct podlink_message){.type=PODLINK_BINARY,.data=packet,.length=2116};assert(receive(&message)&&!transfer.active&&!xs_count);
assert(cider_cache_find(&cache,"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","XS")>=0);

'''
pos=s.rfind('puts(');s=s[:pos]+extra+s[pos:]
(O/'failed.c').write_text(s)
cmd[cmd.index(str(O/'retry.c'))]=str(O/'failed.c');cmd[-1]=str(O/'failed')
subprocess.run(cmd,check=True);subprocess.run([str(O/'failed')],check=True,timeout=10)
