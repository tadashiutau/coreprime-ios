"""Derive 4 KiB ad-hoc cache signatures without changing any Apple cache bytes.
Verify every original 16 KiB SHA-256 code slot before subdividing. Original
cache signatures must already be ad-hoc; authenticated signatures are rejected.
"""
from pathlib import Path
import hashlib,json,struct
base=Path(__file__).resolve().parent
source=base.parent/'hakios/sysimage/20F66__iPhone10,3_6'
dest=base/'out/cache-signatures';dest.mkdir(exist_ok=True)
results=[]
for file in sorted(source.glob('dyld_shared_cache_arm64*')):
 if file.suffix=='.symbols':continue
 with file.open('rb') as f:
  header=f.read(64);offset,size=struct.unpack_from('<QQ',header,40)
  f.seek(offset);sig=f.read(size)
  magic,total,count=struct.unpack_from('>III',sig)
  assert magic==0xfade0cc0 and total<=size and count<=10
  entries=[]
  for i in range(count):
   typ,off=struct.unpack_from('>II',sig,12+8*i)
   blobmagic,blen=struct.unpack_from('>II',sig,off)
   blob=sig[off:off+blen];assert len(blob)==blen
   if typ==0:
    assert blobmagic==0xfade0c02
    cd=bytearray(blob)
    version,flags,hashoff,ident,special,slots,limit=struct.unpack_from('>7I',cd,8)
    assert flags&2 and cd[36]==32 and cd[37]==2 and cd[39] in (12,14)
    assert not (version>=0x20100 and struct.unpack_from('>I',cd,44)[0]),'scatter unsupported'
    assert limit==offset and slots==(limit+(1<<cd[39])-1)>>cd[39]
    assert hashoff>=special*32 and len(cd)==hashoff+slots*32
    shift=cd[39];hashes=[];f.seek(0)
    for index in range(slots):
     data=f.read(min(1<<shift,limit-(index<<shift)))
     assert hashlib.sha256(data).digest()==cd[hashoff+index*32:hashoff+(index+1)*32],f'{file.name}: original hash mismatch {index}'
     hashes.extend(hashlib.sha256(data[i:i+4096]).digest() for i in range(0,len(data),4096))
    cd=cd[:hashoff]+b''.join(hashes)
    struct.pack_into('>I',cd,4,len(cd));struct.pack_into('>I',cd,28,len(hashes));cd[39]=12
    blob=bytes(cd)
   elif typ==0x10000:
    assert blen==8,'non-empty CMS signature cannot be replaced'
   entries.append((typ,blob))
  out=bytearray(struct.pack('>III',magic,0,count));cursor=12+count*8
  for typ,blob in entries:out.extend(struct.pack('>II',typ,cursor));cursor+=len(blob)
  for typ,blob in entries:out.extend(blob)
  struct.pack_into('>I',out,4,len(out))
  (dest/file.name).write_bytes(out)
  results.append({'name':file.name,'bytes':len(out),'sha256':hashlib.sha256(out).hexdigest(),'original_slots_verified':slots,'new_slots':len(hashes),'code_limit':limit})
print(json.dumps({'files':len(results),'signature_bytes':sum(r['bytes'] for r in results),'results':results},indent=2))
