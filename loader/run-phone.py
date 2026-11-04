"""One serial-targeted RAM boot, bounded wait, and ramoops retrieval. Never flashes."""
from pathlib import Path
import subprocess,time,sys,os
root=Path(__file__).resolve().parent
name=sys.argv[1]
assert name.replace('-','').replace('_','').isalnum()
logs=root.parent.parent/'outputs/core-prime-native-bootstrap/hardware-logs'
image=Path(sys.argv[2]).resolve() if len(sys.argv)>2 else root/'out/xnu-phone-persistent.img'
serial='45693dc9'
assert serial in subprocess.check_output(['fastboot','devices'],text=True),'phone absent'
with (logs/f'fastboot-{name}.log').open('w') as log:
 subprocess.run(['fastboot','-s',serial,'boot',str(image)],stdout=log,stderr=log,check=True)
time.sleep(3)
end=time.monotonic()+int(os.environ.get("RETURN_WAIT","150"))
while time.monotonic()<end:
 if serial in subprocess.check_output(['fastboot','devices'],text=True):break
 time.sleep(2)
else:raise SystemExit('Phone did not return; do not send more transactions')
dest=logs/f'ramoops-{name}.bin'
with dest.with_suffix('.transfer.log').open('w') as log:
 for args in [['oem','ramoops','raw'],['get_staged',str(dest)]]:
  subprocess.run(['fastboot','-s',serial]+args,stdout=log,stderr=log,check=True)
subprocess.run([sys.executable,str(root/'extract-run.py'),str(dest)],check=True)
d=dest.read_bytes();log=d[0x1000:0x3000].split(b'\0')[0]
assert b'KERNEL RETURNED TO LOADER' in log and b'LOADER EXCEPTION' not in log, 'loader failure; kernel bytes may be invalid'
assert b'Core Prime native Darwin bring-up' in d[0x4000:0x28000]
assert b'loader_return_after expired' in d[0x4000:0x28000]
assert b'panic(cpu' not in d[0x4000:0x28000]
print('PASS: fresh native userspace run; timed return without panic.')
