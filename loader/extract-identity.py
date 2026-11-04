"""Extract the unencrypted DeviceTree IM4P's OCTET STRING and decompress it."""
from pathlib import Path
import hashlib,json,sys,subprocess
sys.path.insert(0,str(Path(__file__).resolve().parent.parent/'identity-runtime'))
import lzfse
root=Path(__file__).resolve().parent.parent
src=root/'ipsw/DeviceTree.d22ap.im4p'
d=src.read_bytes()
def item(pos):
 tag=d[pos];n=d[pos+1];pos+=2
 if n&128:
  count=n&127;assert 0<count<=4
  n=int.from_bytes(d[pos:pos+count],'big');pos+=count
 assert pos+n<=len(d)
 return tag,d[pos:pos+n],pos+n
assert d[0]==0x30
pos=2+(d[1]&127) if d[1]&128 else 2
items=[]
while pos<len(d):
 tag,v,pos=item(pos);items.append((tag,v))
assert items[0]==(0x16,b'IM4P') and items[1]==(0x16,b'dtre')
payload=next(v for tag,v in items if tag==4)
raw=lzfse.decompress(payload) if payload[:4] in [b'bvx2',b'bvx1',b'bvxn',b'bvx-'] else payload
out=root/'ipsw/DeviceTree.d22ap.bin';out.write_bytes(raw)
meta={'input':str(src),'im4p_sha256':hashlib.sha256(d).hexdigest(),'payload_bytes':len(payload),'decoded_bytes':len(raw),'decoded_sha256':hashlib.sha256(raw).hexdigest()}
out.with_suffix('.json').write_text(json.dumps(meta,indent=2)+'\n');print(json.dumps(meta,indent=2))

header=subprocess.check_output([sys.executable,str(root/'hakios/xnuloader/mkidentity.py'),str(out)],text=True)
# The native loader must not contain link-time absolute pointers.
header=header.replace('const char *name;', 'char name[32];')
(root/'coreprime-xnu-loader/identity-iphone-x.h').write_text(header)
