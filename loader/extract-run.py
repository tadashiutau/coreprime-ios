"""Extract loader/kernel logs and full-resolution monochrome screen captures."""
from pathlib import Path
import json,struct,sys
from PIL import Image
p=Path(sys.argv[1]); data=p.read_bytes()
loader=data[0x1000:0x3000].split(b'\0')[0].decode(errors='replace')
valid='KERNEL RETURNED TO LOADER' in loader and 'LOADER EXCEPTION' not in loader
kernel=data[0x4000:0x28000].split(b'\0')[0].decode(errors='replace') if valid else '(No verified kernel return; remaining bytes excluded.)'
p.with_suffix('.txt').write_text('=== loader ===\n'+loader+'\n=== kernel ===\n'+kernel)

meta={'verified_kernel_return':valid}
for name,off in [('before',0x28000),('after',0x34000)]:
 if not valid or data[off:off+8]!=b'CPFBMON1':continue
 w,h,size,version,checksum=struct.unpack_from('<IIIIQ',data,off+8)
 assert (w,h,size,version)==(480,800,48000,1)
 Image.frombytes('1',(w,h),data[off+32:off+32+size]).save(p.with_name(p.stem+'-'+name+'.png'))
 meta[name]={'width':w,'height':h,'framebuffer_fnv64':f'{checksum:016x}'}
p.with_suffix('.json').write_text(json.dumps(meta,indent=2)+'\n')
print(p.with_suffix('.txt'));print(json.dumps(meta,indent=2))
