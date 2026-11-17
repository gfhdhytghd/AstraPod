#!/usr/bin/env python3
"""Capture the actual SDL simulator, without a device or a desktop session.
Usage: python3 tools/tests/fu57_sim_evidence.py BUILD BASE_ZIP EVIDENCE_DIR
Requires an already built simulator/rockbox.zip, Xvfb, xdotool and Pillow.
"""
from pathlib import Path
import json,os,subprocess,sys,time,wave,zipfile,tempfile
from PIL import Image,ImageGrab,ImageChops
build=Path(sys.argv[1]).resolve();base=Path(sys.argv[2]).resolve();out=Path(sys.argv[3]).resolve();out.mkdir(parents=True,exist_ok=True)
root=Path(tempfile.mkdtemp(prefix='fu57-proof-',dir=build))
with zipfile.ZipFile(build/'rockbox.zip') as z:z.extractall(root)
with zipfile.ZipFile(base) as z:
 for n in z.namelist():
  if any(p in n for p in ['/fonts/','/wps/','/themes/','/backdrops/','/icons/']):z.extract(n,root)
(root/'.rockbox/config.cfg').write_text('font: /.rockbox/fonts/18-noto-sans.fnt\nfont bold: /.rockbox/fonts/18-noto-sans-bold.fnt\nwps: scrim\nsbs: scrim\nforeground color: ffffff\nbackground color: 080d17\nstatusbar: off\nstart in screen: files\nshow files: all\nbacklight timeout: off\nscrollbar: off\n')
# A fresh status/config pair makes the file selector deterministic.
for n in ['nvram.bin','config01.cfg']:
 p=root/'.rockbox'/n
 if p.exists():p.unlink()
with wave.open(str(root/'FU57.wav'),'wb') as w:
 w.setnchannels(1);w.setsampwidth(2);w.setframerate(22050);w.writeframes(bytes(22050*2*120))
(root/'FU57.lrc').write_text('[ti:Evening light]\n[ar:FU57 simulator]\n[00:00.00]The sky is turning blue\n[00:03.00]<00:03.00>Follow <00:04.00>the <00:05.00>evening <00:05.80>light\n[00:06.00]让音乐陪你走过今晚\n[00:09.00]Every word will find its time\n[00:12.00]A melody across the city\n[00:15.00]We keep moving through the night\n[00:18.00]The sky is turning blue\n[00:21.00]And all the stars are yours\n',encoding='utf-8')
# The display belongs only to these child processes. Terminate by their PIDs.
env={**os.environ,'DISPLAY':':158','SDL_VIDEODRIVER':'x11','SDL_AUDIODRIVER':'dummy'}
xlog=(out/'xvfb.log').open('w');slog=(out/'simulator.log').open('w')
xv=subprocess.Popen(['Xvfb',':158','-screen','0','800x600x24'],stdout=xlog,stderr=subprocess.STDOUT)
sim=None;timings={}
try:
 time.sleep(.25)
 sim=subprocess.Popen([str(build/'rockboxui'),'--root',str(root),'--nobackground','--zoom','1','--screenshot'],cwd=build,env=env,stdout=slog,stderr=subprocess.STDOUT)
 window=None
 for _ in range(100):
  q=subprocess.run(['xdotool','search','--onlyvisible','--name','iPod 6G'],env=env,capture_output=True,text=True)
  if q.returncode==0:window=q.stdout.splitlines()[-1];break
  if sim.poll() is not None:raise RuntimeError('simulator exited')
  time.sleep(.05)
 assert window,'no simulator window'
 time.sleep(.8)
 window=subprocess.check_output(['xdotool','search','--onlyvisible','--name','iPod 6G'],env=env,text=True).splitlines()[-1]
 def key(k):subprocess.run(['xdotool','key','--window',window,'--delay','70',k],env=env,check=True)
 def shot():return ImageGrab.grab(xdisplay=':158').crop((240,180,560,420)).convert('RGB')
 def sequence(name,seconds,button=None,trim=False):
  frames=[];stamps=[];start=time.monotonic();process=None
  if button:process=subprocess.Popen(['xdotool','key','--window',window,'--delay','70',button],env=env)
  for i in range(round(seconds*60)):
   target=start+i/60;time.sleep(max(0,target-time.monotonic()))
   frames.append(shot());stamps.append(time.monotonic()-start)
  if process:assert process.wait()==0
  if trim:
   motion=[]
   for i in range(1,len(frames)):
    hist=ImageChops.difference(frames[i-1],frames[i]).histogram()
    if sum(sum(hist[c*256+1:(c+1)*256]) for c in range(3))>600:motion.append(i)
   assert motion,(name,'no column motion')
   a=max(0,motion[0]-2);frames=frames[a:a+28];stamps=stamps[a:a+28]
  for i,im in enumerate(frames):im.save(out/f'{name}-{i:02}.png')
  frames[0].save(out/f'{name}.gif',save_all=True,append_images=frames[1:],duration=17,loop=0)
  unique=sum(a.tobytes()!=b.tobytes() for a,b in zip(frames,frames[1:]))
  timings[name]={'seconds':stamps[-1],'capture_times':stamps,'changed_frames':unique,'frames':len(frames)}
  assert unique>=4,(name,unique)
  return frames
 key('Down');time.sleep(.1);key('Down');time.sleep(.1);key('Return');playing=time.monotonic()
 time.sleep(.65)
 shot().save(out/'wps.png')
 entry=sequence('entry',.55,'Return')
 auto=sequence('auto',8,trim=True)
 manual_at=time.monotonic()
 manual=sequence('manual',.6,'Down')
 shot().save(out/'manual-settled.png')
 # The four-second idle return includes pill fade and retarget to playback.
 time.sleep(max(0,manual_at+3.5-time.monotonic()));returned=sequence('idle-return',1.6)
 exit_frames=sequence('exit',.55,'Return')
 # Show CJK and all layout states in a compact contact sheet.
 sheet=Image.new('RGB',(320*3,240*2))
 for i,im in enumerate([entry[0],entry[8],entry[-1],auto[-1],manual[-1],returned[-1]]):sheet.paste(im,((i%3)*320,(i//3)*240))
 sheet.save(out/'contact-sheet.png')
 (out/'capture.json').write_text(json.dumps({'source':'actual SDL simulator','size':[320,240],'command':[str(build/'rockboxui'),'--root',str(root),'--nobackground','--zoom','1','--screenshot'],'sequences':timings},indent=2)+'\n')
 print('PASS actual simulator entry/auto/manual/idle-return/exit frame sequences',out)
finally:
 for process in [sim,xv]:
  if process and process.poll() is None:
   process.terminate()
   try:process.wait(timeout=3)
   except subprocess.TimeoutExpired:process.kill();process.wait()
 xlog.close();slog.close()
