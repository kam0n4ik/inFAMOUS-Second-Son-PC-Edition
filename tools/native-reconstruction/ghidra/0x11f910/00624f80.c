// FUN_00624f80 @0x624f80 size=126
// callers: 0x50d0 0x51f0 0x19050 0x190c0 0x19100 0x19190 0x19380 0x195a0 0x19770 0x19a00 0x1a1770 0x1a1960 0x1d0ce0 0x2eae80 0x3fbb30 0x3fc3e0 0x43d480 0x4487a0 0x4489a0 0x448b30 0x448ce0 0x457a10 0x4a7880 0x5a8f40
// callees: 

void FUN_00624f80(long param_1,uint param_2,uint param_3)

{
  char cVar1;
  uint uVar2;
  undefined4 *puVar3;
  
  puVar3 = *(undefined4 **)(param_1 + 0x10);
  if ((uint)((ulong)(*(long *)(param_1 + 8) - (long)puVar3) >> 2) < 2) {
    cVar1 = (**(code **)(param_1 + 0x18))(param_1,2,*(undefined8 *)(param_1 + 0x20));
    if (cVar1 == '\0') {
      return;
    }
    puVar3 = *(undefined4 **)(param_1 + 0x10);
  }
  *puVar3 = 0xc0002a00;
  uVar2 = 0;
  if (param_3 != 2) {
    uVar2 = (param_3 & 3) << 6;
  }
  puVar3[1] = (uint)(param_3 != 2) << 10 | uVar2 | param_2 & 0xfffffb3f;
  *(long *)(param_1 + 0x10) = *(long *)(param_1 + 0x10) + 8;
  return;
}

