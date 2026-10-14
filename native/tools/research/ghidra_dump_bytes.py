# Snippet for E:\SecondSonPC\tools\native-reconstruction\ghidra\run.py (read-only project session).
# args: addr_hex size out_path ; writes the raw bytes of the eboot image at [addr, addr+size).
from jpype import JArray, JByte

a = int(args[0], 16)
n = int(args[1], 0)
buf = JArray(JByte)(n)
mem.getBytes(A(a), buf)
with open(args[2], 'wb') as f:
    f.write(bytes(x & 0xff for x in buf))
print('dumped', n, 'bytes from', hex(a))
