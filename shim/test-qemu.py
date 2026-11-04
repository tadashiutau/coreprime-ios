from pathlib import Path
import json,socket,subprocess,time
from decode import decode
root=Path(__file__).resolve().parent
qemu=root.parent/'qemu-runtime/root/usr/bin/qemu-system-aarch64'
out=root/'out';sock=out/'qmp.sock';log=out/'qemu-diag.log'
if sock.exists():sock.unlink()
with log.open('wb') as output:
 proc=subprocess.Popen([str(qemu),'-M','virt,gic-version=2','-cpu','cortex-a53','-m','1G','-nographic','-no-reboot','-kernel',str(out/'Image'),'-dtb',str(out/'qemu-test.dtb'),'-qmp',f'unix:{sock},server=on,wait=off'],stdout=output,stderr=subprocess.STDOUT)
 try:
  deadline=time.monotonic()+10
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
  while 'READY: CPU ENTRY CHECK COMPLETE' not in log.read_text(errors='replace'):
   if proc.poll() is not None or time.monotonic()>deadline:raise RuntimeError('Diagnostic did not reach READY')
   time.sleep(.05)
  for filename,address,size in [('qemu-ramoops.bin',0x47000000,0x80000),('qemu-framebuffer.bin',0x46000000,0x177000)]:
   cmd('pmemsave',{'val':address,'size':size,'filename':str(out/filename)})
  s.close()
  record=decode((out/'qemu-ramoops.bin').read_bytes())
  assert record['stage']==4 and record['el']==4
  assert record['gicd']==0x08000000 and record['gicc']==0x08010000
  assert record['vtimer_intid']==27 and record['reserved_count']>=1
  assert record['fb']==0x46000000 and record['mem_count']==1
  assert record['memory_0']==0x40000000 and record['memory_1']==0x40000000
  (out/'qemu-diagnostic.json').write_text(json.dumps(record,indent=2)+'\n')
  assert proc.wait(timeout=20)==0
  assert 'RESETTING TO BOOT LOADER' in log.read_text()
  print('PASS: ARM64 entry, FDT parsing, framebuffer, checksummed RAM log, and automatic reset')
 finally:
  if proc.poll() is None:proc.kill();proc.wait()
from PIL import Image
Image.frombytes('RGB',(480,800),(out/'qemu-framebuffer.bin').read_bytes(),'raw','BGRX').save(out/'qemu-display.png')
