from pathlib import Path
import json,socket,subprocess,time,sys
root=Path(__file__).resolve().parent
qemu=root.parent/'qemu-runtime/root/usr/bin/qemu-system-aarch64'
out=root/'out';sock=out/'xnu-qmp.sock';log=out/'xnu-qemu.log'
timeout=int(sys.argv[1]) if len(sys.argv)>1 else 75
if sock.exists():sock.unlink()
with log.open('wb') as output:
 proc=subprocess.Popen([str(qemu),'-M','virt,gic-version=2','-cpu','cortex-a53','-m','1G',
  '-nographic','-no-reboot','-monitor','none',
  '-kernel',str(out/'loader-Image'),
  '-dtb',str(out/'xnu-test.dtb'),
  '-device','loader,file='+str(out/'payload.bin')+',addr=0x49000000,force-raw=on',
  '-qmp',f'unix:{sock},server=on,wait=off'],stdout=output,stderr=subprocess.STDOUT)
 try:
  deadline=time.monotonic()+timeout
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
  # wait for the milestone lines on serial (loader text + kernel console via PL011)
  while ('Hello from user space on Darwin!' not in log.read_text(errors='replace')):
   if proc.poll() is not None or time.monotonic()>deadline:break
   time.sleep(.2)
  # dump the ramoops region (record + loader log + harvested kernel log)
  cmd('pmemsave',{'val':0x47000000,'size':0x80000,'filename':str(out/'xnu-ramoops.bin')})
  s.close()
  text=log.read_text(errors='replace')
  for expected in ['CORE PRIME XNU LOADER','JUMPING TO KERNEL','Hello from user space on Darwin!',
                   'I am process 1, uid 0']:
   if expected not in text:raise SystemExit('FAIL: missing '+expected)
  (out/'xnu-ramoops.bin')
  print('PASS: native lk2nd-style loader booted the real XNU kernel; PID 1 ran')
  print(log)
 finally:
  if proc.poll() is None:proc.kill();proc.wait()
