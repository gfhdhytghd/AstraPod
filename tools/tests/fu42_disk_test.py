"""Production disk enter gate, ACK barrier and body-menu EP0 exit, no devices."""
from pathlib import Path
import runpy
import subprocess
import sys

R = Path(__file__).resolve().parents[2]
O = Path(sys.argv[1]).resolve(); O.mkdir(parents=True, exist_ok=True)
base = runpy.run_path(str(R/'tools/tests/usb_after_agents_test.py'))
usb = (R/'firmware/usb.c').read_text()
fn = base['fn']
source = (O/'after-agents.c').read_text().replace('int main(int argc,char**argv)', 'int original_main(int argc,char**argv)')
# Existing harness signature is deliberately checked, never rewrite silently.
if 'int original_main' not in source:
    source = source.replace('int main(void)', 'int original_main(void)')
assert 'int original_main' in source
end = source.rfind('}')
source = source[:end] + 'return 0;\n' + source[end:]
state = usb[usb.index('static long disk_menu_at'):usb.index('\n#endif\n#endif\n#ifdef USB_ENABLE_AUDIO',usb.index('static long disk_menu_at'))]
# State section ends after disk API functions, before serial's closing guards.
branch = usb[usb.index('        case USB_DISK_ENTER:'):usb.index('        case USB_PODLINK_OPEN:')]
extra = r'''
#define BUTTON_MENU 0x100
#define BUTTON_RC_MENU 0x800
#define TIME_BEFORE(a,b) ((long)(a)-(long)(b)<0)
static int raw_buttons;
static int button_status(void){return raw_buttons;}
static int disable_irq_save(void){return 0;}
static void restore_irq(int n){(void)n;}
static void usb_core_storage_ejected(void){}
@STATE@
@EJECT@
@POLL@
enum { USB_DISK_ENTER=900 };
@REQUEST@
static void enter_drain(void){while(head<tail){struct queue_event ev=pending[head++];switch(ev.id){
@BRANCH@
default:assert(0);
}}head=tail=0;}
static void storage(void){
    begin(USB_MODE_MASS_STORAGE,true);
    usb_podlink_disk();enter_drain();assert(!podlink_requested&&!usb_disk_ejected);
    host_configure();assert(exclusive_storage_requested&&!exclusive_storage_enabled);
    assert(usb_num_acks_to_expect==3);
    for(int n=0;n<3;n++){
        queue_post(&usb_queue,SYS_USB_CONNECTED_ACK,usb_broadcast_seqnum);drain();
        assert(exclusive_storage_enabled==(n==2));
    }
}
int main(void){
    /* Every screen uses the same resident request and complete ACK barrier. */
    for(int screen=0;screen<5;screen++){
        storage();assert(handovers==1);usb_storage_ejected();
        assert(usb_disk_ejected&&!exclusive_storage_requested);
        current_tick+=HZ;usb_disk_poll();assert(podlink_requested&&serial()=='2');
    }
    storage();raw_buttons=BUTTON_RC_MENU;usb_disk_poll();current_tick+=2*HZ;
    usb_disk_poll();assert(!disk_exit_pending&&exclusive_storage_enabled);
    raw_buttons=BUTTON_MENU;usb_disk_poll();current_tick+=HZ-1;usb_disk_poll();
    assert(!disk_exit_pending);current_tick++;usb_disk_poll();assert(disk_exit_pending);
    unsigned char status[8];usb_disk_status(status);
    assert(status[0]==1&&status[1]&&status[2]);
    assert(!usb_disk_flush_ack(disk_exit_request+1));
    assert(usb_disk_flush_ack(disk_exit_request));
    current_tick++;usb_disk_poll();assert(!exclusive_storage_enabled&&podlink_requested);
    assert(!usb_disk_take_warning());assert(!usb_disk_flush_ack(disk_exit_request));
    storage();raw_buttons=BUTTON_MENU;usb_disk_poll();current_tick+=HZ;usb_disk_poll();
    assert(disk_exit_pending);current_tick+=3*HZ;usb_disk_poll();
    assert(exclusive_storage_enabled);current_tick++;usb_disk_poll();
    assert(!exclusive_storage_enabled&&podlink_requested&&usb_disk_take_warning());
    assert(!usb_disk_take_warning());
    storage();raw_buttons=BUTTON_MENU;usb_disk_poll();current_tick+=HZ;usb_disk_poll();
    cable=false;usb_disk_poll();assert(!disk_exit_pending&&!disk_return_at);
    puts("FU42: five screen contexts/all 3 ACKs/FU29; remote bits ignored; body 1s; EP0 stale/valid ACK; 3s timeout warning; disconnect cancellation PASS");
    return 0;
}
'''
for key,value in {'STATE':state,'EJECT':fn(usb,'usb_storage_ejected'),'POLL':fn(usb,'usb_disk_poll'),'REQUEST':fn(usb,'usb_podlink_disk'),'BRANCH':branch}.items():
    extra=extra.replace('@'+key+'@',value)
cfile=O/'fu42.c';cfile.write_text(source+'\n'+extra)
cmd=['cc','-std=gnu11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(R/'firmware/export'),str(cfile),'-o',str(O/'fu42')]
subprocess.run(cmd,check=True);subprocess.run([str(O/'fu42')],check=True)
# Execute the production EP0 dispatch branch with both valid and malformed setups.
core = (R/'firmware/usbstack/usb_core.c').read_text()
start = core.index('#ifdef HAVE_USB_PODLINK', core.index('static void usb_core_control_request_handler('))
end = core.index('#ifdef USB_REMOTE_KEY_ENABLED', start)
ep0 = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define HAVE_USB_PODLINK
#define CONFIGURED 2
#define USB_CONTROL_ACK 0
#define USB_CONTROL_STALL 1
static int usb_state=CONFIGURED,response=-1;
static unsigned length;
static uint32_t acknowledged;
struct usb_ctrlrequest {uint8_t bRequestType,bRequest;uint16_t wValue,wIndex,wLength;};
static void usb_disk_status(unsigned char *p){p[0]=1;p[2]=1;}
static bool usb_disk_flush_ack(uint32_t r){acknowledged=r;return r==0x12345678;}
static void usb_core_control_response(int r,const void*p,size_t n){(void)p;response=r;length=n;}
static void route(struct usb_ctrlrequest* req,unsigned char* reqdata){
@ROUTE@
}
int main(void){
 unsigned char bytes[8]={0};
 struct usb_ctrlrequest q={0xc0,0x5c,0,0,8};route(&q,bytes);
 assert(response==USB_CONTROL_ACK&&length==8&&bytes[0]==1&&bytes[2]==1);
 q.wLength=7;route(&q,bytes);assert(response==USB_CONTROL_STALL);
 q=(struct usb_ctrlrequest){0x40,0x5c,0x5678,0x1234,0};route(&q,bytes);
 assert(response==USB_CONTROL_ACK&&acknowledged==0x12345678);
 q.wIndex=0;route(&q,bytes);assert(response==USB_CONTROL_STALL);
 q.bRequestType=0x41;route(&q,bytes);assert(response==USB_CONTROL_STALL);
 q=(struct usb_ctrlrequest){0xc0,0x5c,0,0,8};usb_state=0;route(&q,bytes);
 assert(response==USB_CONTROL_STALL);
 return 0;
}
'''.replace('@ROUTE@',core[start:end])
f=O/'ep0.c';f.write_text(ep0)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(f),'-o',str(O/'ep0')],check=True)
subprocess.run([str(O/'ep0')],check=True)
print('FU42 EP0: GET status, complete request ID, malformed/stale/wrong recipient/unconfigured stall PASS')
