from pathlib import Path
import json,socket,subprocess,time,struct
root=Path(__file__).resolve().parent
out=root/'out'; sock=out/'persistent-qmp.sock'; log=out/'persistent-qemu.log'
if sock.exists():sock.unlink()
with log.open('wb') as output:
 proc=subprocess.Popen([str(root.parent/'qemu-runtime/root/usr/bin/qemu-system-aarch64'),'-M','virt,gic-version=2','-cpu','cortex-a53','-m','1G','-nographic','-no-reboot','-no-shutdown','-monitor','none','-kernel',str(out/'loader-Image'),'-dtb',str(out/'xnu-test.dtb'),'-device','loader,file='+str(out/'payload.bin')+',addr=0x49000000,force-raw=on','-qmp',f'unix:{sock},server=on,wait=off'],stdout=output,stderr=subprocess.STDOUT)
 try:
  deadline=time.monotonic()+65
  while not sock.exists():
   if proc.poll() is not None or time.monotonic()>deadline:raise RuntimeError('QEMU did not start')
   time.sleep(.05)
  s=socket.socket(socket.AF_UNIX);s.connect(str(sock)); f=s.makefile('rwb',buffering=0);f.readline()
  def cmd(name,args=None):
   f.write((json.dumps({'execute':name,'arguments':args or {}})+'\n').encode())
   while True:
    reply=json.loads(f.readline())
    if 'error' in reply:raise RuntimeError(reply)
    if 'return' in reply:return reply
  cmd('qmp_capabilities')
  while 'KERNEL RETURNED TO LOADER' not in log.read_text(errors='replace'):
   if proc.poll() is not None or time.monotonic()>deadline:raise RuntimeError('No orderly return; inspect '+str(log))
   time.sleep(.2)
  dump=out/'persistent-ramoops.bin'
  cmd('pmemsave',{'val':0x47000000,'size':0x80000,'filename':str(dump)})
  d=dump.read_bytes()
  for value in [b'Core Prime native Darwin bring-up: persistent PID 1',b'Core Prime PID 1 alive: 15 seconds',b'loader_return_after expired',b'hw.machine=iPhone10,3']:
   assert value in d, 'missing '+repr(value)
  assert b'panic(cpu' not in d,'unexpected panic'
  for off in [0x28000,0x34000]: assert d[off:off+8]==b'CPFBMON1','snapshot absent'
  assert d[0x28018:0x28020]!=d[0x34018:0x34020], 'framebuffer did not change'
  print('PASS: persistent PID1 heartbeat, timed return without panic, changed framebuffer with text mode.'); print(dump)
 finally:
  if proc.poll() is None:proc.kill();proc.wait()
