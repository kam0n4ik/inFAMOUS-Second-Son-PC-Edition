"""R11: disassemble eboot.bin at module-relative addresses (capstone over tools/ps4elf.py's image).

  py -I -X utf8 tools/r11/dis.py <eboot.bin> <rva>[:<bytes>] [...]
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import capstone  # noqa: E402
import ps4elf  # noqa: E402

img, _ = ps4elf.Ps4Elf(sys.argv[1]).build_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for spec in sys.argv[2:]:
    rva, _, n = spec.partition(":")
    rva = int(rva, 0)
    n = int(n, 0) if n else 0x80
    print(f"== {rva:#x} ({n} bytes)")
    for ins in md.disasm(bytes(img[rva:rva + n]), rva):
        print(f"  {ins.address:#08x}  {ins.bytes.hex():<24} {ins.mnemonic} {ins.op_str}")
