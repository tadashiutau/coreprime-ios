import struct,json,sys
from pathlib import Path
NAMES='stage el midr mmfr0 mmfr1 pfr0 cntfrq cntvct sctlr tcr dtb dtbsize fb fbsize width height stride format ramoops ramoops_size uart mem_count'.split()
NAMES += [f'memory_{i}' for i in range(16)]
NAMES += 'exception_slot esr elr far'.split()
V2_NAMES = 'gicd gicc vtimer_intid vtimer_flags initrd_start initrd_end reserved_count'.split() + [f'reserved_{i}' for i in range(32)]
def decode(data):
 offset=data.find(b'CPDIAG64')
 if offset<0: raise ValueError('Diagnostic record not found')
 magic,version,size=struct.unpack_from('<8sII',data,offset)
 if version not in (1,2):raise ValueError(f'Unexpected record version: {version}')
 names=NAMES+(V2_NAMES if version==2 else [])+['checksum']
 if size!=16+8*len(names):raise ValueError(f'Unexpected record size: {size}')
 values=struct.unpack_from('<'+str(len(names))+'Q',data,offset+16)
 h=14695981039346656037
 for b in data[offset:offset+size-8]:h=((h^b)*1099511628211)&((1<<64)-1)
 result=dict(zip(names,values))
 if h!=result['checksum']:raise ValueError('Diagnostic checksum mismatch')
 result['record_offset']=offset
 result['text']=data[offset+4096:offset+4096+8192].split(b'\0')[0].decode(errors='replace')
 return result
if __name__=='__main__':
 result=decode(Path(sys.argv[1]).read_bytes())
 print(json.dumps(result,indent=2))
