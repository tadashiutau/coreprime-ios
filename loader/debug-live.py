from pathlib import Path
import json,socket,subprocess,time,struct
root=Path(__file__).resolve().parent
qemu=root.parent/'qemu-runtime/root/usr/bin/qemu-system-aarch64'
out=root/'out';sock=out/'dbg2-qmp.sock';log=out/'dbg2-qemu.log'
if sock.exists():sock.unlink()
with log.open('wb') as output:
 proc=subprocess.Popen([str(qemu),'-M','virt,gic-version=2','-cpu','cortex-a53','-m','1G',
  '-nographic','-no-reboot','-monitor','none',
  '-kernel',str(out/'loader-Image'),
  '-dtb',str(out/'xnu-test.dtb'),
  '-device','loader,file='+str(out/'payload.bin')+',addr=0x49000000,force-raw=on',
  '-qmp',f'unix:{sock},server=on,wait=off'],stdout=output,stderr=subprocess.STDOUT)
 try:
  deadline=time.monotonic()+60
  while not sock.exists():
   if proc.poll() is not None or time.monotonic()>deadline:raise RuntimeError('QEMU did not start')
   time.sleep(.05)
  s=socket.socket(socket.AF_UNIX);s.connect(str(sock));f=s.makefile('rwb',buffering=0);f.readline()
  def cmd(name,args=None):
   f.write((json.dumps({'execute':name,'arguments':args or {}})+'\n').encode())
   while True:
    reply=json.loads(f.readline())
    if 'error' in reply:raise RuntimeError(reply)
    if 'return' in reply:return reply
  cmd('qmp_capabilities')
  while 'JUMPING TO KERNEL' not in log.read_text(errors='replace'):
   if time.monotonic()>deadline:raise RuntimeError('loader never jumped')
   time.sleep(.1)
  time.sleep(15)
  cmd('pmemsave',{'val':0x4C649A0,'size':8,'filename':str(out/'dbg2-len.bin')})
  n=struct.unpack_from('<I',open(out/'dbg2-len.bin','rb').read(),0)[0]
  print('loader_log_len =',n)
  n=min(n,0x2000)
  cmd('pmemsave',{'val':0x4AE49A0,'size':n,'filename':str(out/'dbg2-log.bin')})
  data=open(out/'dbg2-log.bin','rb').read()
  print(data.decode(errors='replace')[:3500])
  s.close()
 finally:
  if proc.poll() is None:proc.kill();proc.wait()
