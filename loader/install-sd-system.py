"""Install the checked system image to the explicitly verified SD partition only."""
from pathlib import Path
import subprocess,time,json,hashlib
root=Path(__file__).resolve().parent.parent.parent
image=root/'work/hakios/userland/system.apfs'
logs=root/'outputs/core-prime-native-bootstrap/hardware-logs'
adb=['adb','-s','45693dc9']
def shell(command):return subprocess.check_output(adb+['shell',command],text=True).strip().replace('\r','')
state=subprocess.check_output(adb+['get-state'],text=True).strip()
assert state=='recovery',repr(state)
values=shell('cat /sys/block/mmcblk1/device/type /sys/block/mmcblk1/device/name /sys/block/mmcblk1/size /sys/block/mmcblk1/mmcblk1p1/start /sys/block/mmcblk1/mmcblk1p1/size').splitlines()
assert values==['SD','SK128','249737216','2048','249733120'],values
assert image.stat().st_size==2476720128
expected=json.loads((root/'outputs/core-prime-native-bootstrap/ios-filesystem-verification.json').read_text())['system.apfs']['sha256']
h=hashlib.sha256()
with image.open('rb') as f:
 for b in iter(lambda:f.read(4*1024*1024),b''):h.update(b)
assert h.hexdigest()==expected,'local checked image changed'
if 'mmcblk1' in shell('cat /proc/mounts'):
 subprocess.run(adb+['shell','umount /external_sd'],check=True)
assert 'mmcblk1' not in shell('cat /proc/mounts'),'SD remains mounted'
with (logs/'microsd-layout-before.bin').open('wb') as f:
 subprocess.run(adb+['exec-out','dd if=/dev/block/mmcblk1 bs=512 count=2048'],stdout=f,stderr=subprocess.PIPE,check=True)
layout=logs/'microsd-layout-before.bin'
raw=layout.read_bytes();assert len(raw)>=1048576 and raw[510:512]==b'\x55\xaa'
layout.write_bytes(raw[:1048576])
# All target paths are fixed literals, following the model/type/geometry guard.
with (logs/'microsd-install-transfer.log').open('wb') as log:
 p=subprocess.Popen(adb+['exec-in','dd of=/dev/block/mmcblk1p1 bs=1048576 conv=fsync'],stdin=subprocess.PIPE,stdout=log,stderr=log)
 sent=0;next_report=256*1024*1024;started=time.monotonic()
 with image.open('rb') as f:
  while b:=f.read(1024*1024):
   p.stdin.write(b);sent+=len(b)
   if sent>=next_report:
    print(f'SD write: {sent//(1024*1024)} MiB / {image.stat().st_size//(1024*1024)} MiB, {time.monotonic()-started:.0f}s',flush=True);next_report+=256*1024*1024
 p.stdin.close();assert p.wait()==0,'SD dd failed; inspect transfer log'
print('SD write finished; verifying full image on card',flush=True)
subprocess.run(adb+['shell','sync'],check=True)
actual=shell('head -c 2476720128 /dev/block/mmcblk1p1 | sha256sum').split()[0]
assert actual==expected,('CARD HASH MISMATCH',actual,expected)
result={'device':'45693dc9','target':'/dev/block/mmcblk1p1','model':'SK128','type':'SD','card_sectors':249737216,'partition_start':2048,'partition_sectors':249733120,'image_bytes':image.stat().st_size,'sha256':actual,'full_card_readback_matches':True,'partition_table_modified':False,'internal_partitions_flashed':False,'ios_booted':False}
(root/'outputs/core-prime-native-bootstrap/microsd-system-install.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2),flush=True)
