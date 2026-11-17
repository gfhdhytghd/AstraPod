"""FU43 Dock ownership through real EP0/button sampling/gesture/consent code."""
from pathlib import Path
import re,subprocess,sys
R=Path(__file__).resolve().parents[2];O=Path(sys.argv[1]).resolve();O.mkdir(parents=True,exist_ok=True)
p=R/'tools/tests/usb_remote_key_test.py';ns={'__file__':str(p),'__name__':'fu43_keys_adapters'}
exec(compile(p.read_text().split("tests = r'''")[0],str(p),'exec'),ns)
fn=ns['function'];pre=ns['preamble'];source=ns['source']
text=p.read_text();a=text.index("tests = r'''")+len("tests = r'''");b=text.index('int main(void)',a);helpers=text[a:b];helpers=helpers[:helpers.index('static void gesture(')]
confirm=fn((R/'apps-ipod/widgets/usb_confirm.c').read_text(),'confirm_keymap')
extra=sorted(set(re.findall(r'\b(?:ACTION_|BUTTON_MULTIMEDIA_)\w+',confirm)))
extra='enum {'+','.join(f'{n}={1000+i}' for i,n in enumerate(extra))+'};\n'
extra+='struct button_mapping{int action_code,button_code,pre_button_code;};\n#define LAST_ITEM_IN_LIST {-1,0,0},\n'
post=r'''
static int count(int key){int n=0;for(int i=0;i<mapped_count;i++)n+=mapped[i]==key;return n;}
static void reset_at(unsigned long base,int profile){
 physical=0;remote_button=0;lastbtn=last_read=0;remote=(struct agents_remote_state){0};used=cursor=mapped_count=0;
 for(unsigned long t=base;t<base+4;t++)tick(t);
 used=cursor=0;current_tick=base+4;request(0x40,5,profile,0,USB_CONTROL_ACK);
 assert(button_usb_remote_bridge_fresh(current_tick));
}
static void pulse(int context,int key,unsigned long start,unsigned ticks){
 physical=key;for(unsigned long t=start;t<start+ticks;t++)poll_tick(context,t);
 physical=0;for(unsigned long t=start+ticks;t<start+ticks+4;t++)poll_tick(context,t);
}
int main(void){
 unsigned long base=10000;int scopes=0;
 for(int profile=1;profile<=2;profile++)for(int context=REMOTE_OTHER;context<=REMOTE_QUICKSCREEN;context++){
  reset_at(base,profile);
  /* Computer IR blocked: only the receiver heartbeat, zero PC key reports. */
  pulse(context,BUTTON_RC_PLAY,base+10,13);
  for(unsigned long t=base+27;t<base+55;t++)poll_tick(context,t);
  int tap=context==REMOTE_QUICKSCREEN?BUTTON_RC_SELECT:context==REMOTE_AGENTS||context==REMOTE_COVERS||context==REMOTE_ALBUM_LIST?BUTTON_SELECT:BUTTON_RC_PLAY;
  assert(count(tap)==1&&!count(REMOTE_SWITCH_BUTTON)&&!count(REMOTE_COVERS_BUTTON));
  if(context==REMOTE_OTHER){
   const struct button_mapping*k=confirm_keymap(0);bool connect=false;
   for(int i=0;k[i].action_code!=-1;i++)if(k[i].button_code==(BUTTON_RC_PLAY|BUTTON_REL)&&k[i].pre_button_code==BUTTON_RC_PLAY&&k[i].action_code==ACTION_STD_OK)connect=true;
   assert(connect&&count(BUTTON_RC_PLAY|BUTTON_REL)==1);
  }
  reset_at(base+1000,profile);pulse(context,BUTTON_RC_PLAY,base+1010,100);
  if(context==REMOTE_QUICKSCREEN)assert(count(BUTTON_RC_SELECT)&&!count(BUTTON_PLAY|BUTTON_REPEAT));
  else if(context==REMOTE_AGENTS)assert(count(BUTTON_RC_PLAY)&&count(BUTTON_PLAY|BUTTON_REL));
  else if(context==REMOTE_WPS)assert(count(REMOTE_LYRICS_BUTTON)==1&&!count(BUTTON_RC_PLAY|BUTTON_REL)&&!count(BUTTON_PLAY|BUTTON_REL));
  else assert(count(BUTTON_PLAY|BUTTON_REPEAT)&&count(BUTTON_PLAY|BUTTON_REL));
  reset_at(base+2000,profile);
  for(int n=0;n<2;n++)pulse(context,BUTTON_RC_PLAY,base+2010+n*28,13);
  for(unsigned long t=base+2069;t<base+2100;t++)poll_tick(context,t);
  assert(!count(REMOTE_COVERS_BUTTON));
  if(context==REMOTE_QUICKSCREEN)assert(count(BUTTON_RC_SELECT)==2&&!count(REMOTE_SWITCH_BUTTON));else assert(count(REMOTE_SWITCH_BUTTON)==1);
  reset_at(base+3000,profile);
  for(int n=0;n<3;n++)pulse(context,BUTTON_RC_PLAY,base+3010+n*28,13);
  assert(count(REMOTE_COVERS_BUTTON)==1&&!count(REMOTE_SWITCH_BUTTON));
  for(int direction=0;direction<2;direction++){
   reset_at(base+4000+direction*1000,profile);int key=direction?BUTTON_RC_RIGHT:BUTTON_RC_LEFT;
   int id=direction?7:6;unsigned long at=base+4010+direction*1000;
   /* Old forwarder duplicates before, during and AFTER the Dock pulse. All
      are ACKed but cannot extend or replay the Dock-owned physical press. */
   request(0x40,id,1,0,USB_CONTROL_ACK);request(0x40,id,0,0,USB_CONTROL_ACK);
   pulse(context,key,at,13);
   int expected=context==REMOTE_AGENTS?(direction?BUTTON_RIGHT:BUTTON_LEFT):key;
   assert(count(expected)==1&&count(expected|BUTTON_REL)==1);
   int before=used;
   request(0x40,id,1,0,USB_CONTROL_ACK);request(0x40,id,0,0,USB_CONTROL_ACK);
   for(unsigned long t=at+17;t<at+30;t++)poll_tick(context,t);
   assert(used==before&&count(expected)==1);
  }
  reset_at(base+6000,profile);
  for(int n=0;n<3;n++){
   request(0x40,8,1,0,USB_CONTROL_ACK);request(0x40,8,0,0,USB_CONTROL_ACK);
   pulse(context,BUTTON_RC_PLAY,base+6010+n*28,13);
  }
  assert(count(REMOTE_COVERS_BUTTON)==1&&!count(REMOTE_SWITCH_BUTTON));
  reset_at(base+7000,profile);physical=BUTTON_RC_MENU|BUTTON_RC_SELECT;tick(base+7010);tick(base+7011);assert(!lastbtn);
  /* A computer MENU is still injected exactly once while the Dock's copy
     is masked. Releasing it does not reveal the Dock copy under the heartbeat. */
  used=0;current_tick=base+7012;request(0x40,1,1,0,USB_CONTROL_ACK);tick(base+7013);tick(base+7014);assert(lastbtn==BUTTON_RC_MENU);
  current_tick=base+7020;request(0x40,1,0,0,USB_CONTROL_ACK);tick(base+7021);tick(base+7022);assert(!lastbtn);
  tick(base+7400);tick(base+7401);assert(lastbtn==(BUTTON_RC_MENU|BUTTON_RC_SELECT));
  base+=10000;scopes++;
 }
 reset_at(base,1);pulse(REMOTE_QUICKSCREEN,BUTTON_RC_PLAY,base+10,13);
 pulse(REMOTE_WPS,BUTTON_RC_PLAY,base+38,13);pulse(REMOTE_WPS,BUTTON_RC_PLAY,base+66,13);
 assert(count(BUTTON_RC_SELECT)==1&&count(REMOTE_COVERS_BUTTON)==1&&!count(REMOTE_SWITCH_BUTTON));
 reset_at(base+500,1);lastbtn=BUTTON_RC_PLAY;collect(REMOTE_QUICKSCREEN,BUTTON_RC_PLAY,base+510);lastbtn=0;collect(REMOTE_WPS,0,base+523);
 pulse(REMOTE_WPS,BUTTON_RC_PLAY,base+538,13);pulse(REMOTE_WPS,BUTTON_RC_PLAY,base+566,13);
 assert(count(BUTTON_RC_SELECT)==1&&count(REMOTE_COVERS_BUTTON)==1&&!count(REMOTE_SWITCH_BUTTON));
 reset_at(base+1000,1);lastbtn=BUTTON_RC_PLAY;collect(REMOTE_QUICKSCREEN,BUTTON_RC_PLAY,base+1010);lastbtn=0;collect(REMOTE_WPS,0,base+1014);
 mapped_count=0;for(int n=0;n<3;n++)pulse(REMOTE_WPS,BUTTON_RC_PLAY,base+1100+n*28,13);
 assert(count(REMOTE_COVERS_BUTTON)==1&&!count(REMOTE_SWITCH_BUTTON));
 printf("FU43 %d heartbeat/context scopes: IR blocked Dock tap/hold/double/triple/LR, legacy temporal duplicates, PC MENU ownership, consent Connect and Quick->WPS triple PASS\n",scopes);
}
'''
(O/'config.h').write_text('');(O/'test.c').write_text(pre+extra+source+confirm+helpers+post)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(R),'-I'+str(O),str(O/'test.c'),'-o',str(O/'test')],check=True)
subprocess.run([str(O/'test')],check=True,timeout=30)
