"""Create the dedicated picker copy; original input and installed FilePilot are untouched."""
from pathlib import Path
import hashlib,json,struct,sys
import lief

EXPECTED='ab5e7bbfa50872225f24856697ef7341a58e51a5e521435ba0dfeeb8ab3b2676'
source,out=map(Path,sys.argv[1:3]);raw=source.read_bytes()
if hashlib.sha256(raw).hexdigest()!=EXPECTED:raise SystemExit('Picker currently requires the verified original FilePilot 0.8.5 x64 input')
b=lief.PE.parse(list(raw));align=b.optional_header.section_alignment
lief.logging.disable()  # Cleared certificate directory is intentionally not rebuilt.
start=(max(s.virtual_address+max(s.virtual_size,s.sizeof_raw_data) for s in b.sections)+align-1)&~(align-1)
iat={e.name:d.import_address_table_rva+i*8 for d in b.imports for i,e in enumerate(d.entries) if e.name}
old=b.optional_header.addressof_entrypoint
code=bytearray();fix=[]
def emit(s):code.extend(bytes.fromhex(s))
def relative(prefix,target):
    emit(prefix);at=len(code);code.extend(struct.pack('<i',target-(start+at+4)))
def local(prefix,label):
    emit(prefix);at=len(code);code.extend(b'\0'*4);fix.append((at,label))
emit('48 83 ec 28')
local('48 8d 0d','dll');relative('ff 15',iat['LoadLibraryW'])
emit('48 85 c0');local('0f 84','failure')
emit('48 89 c1');local('48 8d 15','initialize');relative('ff 15',iat['GetProcAddress'])
emit('48 85 c0');local('0f 84','failure')
relative('48 8d 0d',0);emit('ff d0 85 c0');local('0f 84','failure')
emit('48 83 c4 28');relative('e9',old)
labels={'failure':len(code)};emit('b9 01 00 00 00');relative('ff 15',iat['ExitProcess']);emit('cc')
labels['dll']=len(code);code.extend('FPilotPickerRuntime.dll\0'.encode('utf-16le'))
labels['initialize']=len(code);code.extend(b'PickerInitialize\0')
for at,label in fix:struct.pack_into('<i',code,at,labels[label]-at-4)
# The injected entry stub has one 40-byte stack allocation. Keep native unwind entries.
code.extend(b'\0'*((-len(code))%4));unwind=start+len(code);code.extend(bytes([1,4,1,0,4,0x42,0,0]))
code.extend(b'\0'*((-len(code))%4));pdata=start+len(code)
directory=b.data_directories[3]
code.extend(bytes(b.get_content_from_virtual_address(directory.rva,directory.size)))
code.extend(struct.pack('<III',start,start+labels['dll'],unwind))
s=lief.PE.Section('.fpp');s.virtual_address=start;s.content=list(code);s.characteristics=0x60000020;b.add_section(s)
b.optional_header.addressof_entrypoint=start;directory.rva=pdata;directory.size+=12
b.optional_header.checksum=0;b.data_directories[4].rva=0;b.data_directories[4].size=0
builder=lief.PE.Builder(b,lief.PE.Builder.config_t());builder.build();out.parent.mkdir(parents=True,exist_ok=True);out.write_bytes(bytes(builder.raw_bytes()))
out.with_suffix('.json').write_text(json.dumps({'input_sha256':EXPECTED,'output_sha256':hashlib.sha256(out.read_bytes()).hexdigest(),'entry_rva':hex(start),'native_entry_rva':hex(old),'runtime':'FPilotPickerRuntime.dll','profile':'FilePilot 0.8.5 x64'},indent=2)+'\n')
print(out)
