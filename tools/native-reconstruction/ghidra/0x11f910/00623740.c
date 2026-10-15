// FUN_00623740 @0x623740 size=104
// callers: 0x50d0 0x19050 0x190c0 0x19100 0x19190 0x19380 0x195a0 0x19770 0x19a00 0x1a1770 0x1a1960 0x1d0ce0 0x2eae80 0x3f2080 0x3fa200 0x3fbb30 0x3fc3e0 0x3fd5a0 0x43d480 0x4487a0 0x4489a0 0x448b30 0x448ce0 0x454680 0x457a10 0x4a7880 0x5a8f40
// callees: 0x63bb50(FUN_0063bb50)

void FUN_00623740(long param_1,uint param_2)

{
  undefined4 *puVar1;
  char cVar2;
  int iVar3;
  undefined4 uVar4;
  
  if (((uint)((ulong)(*(long *)(param_1 + 8) - *(long *)(param_1 + 0x10)) >> 2) < 3) &&
     (cVar2 = (**(code **)(param_1 + 0x18))(param_1,3,*(undefined8 *)(param_1 + 0x20)),
     cVar2 == '\0')) {
    return;
  }
  iVar3 = FUN_0063bb50();
  puVar1 = *(undefined4 **)(param_1 + 0x10);
  uVar4 = 0x242;
  if (iVar3 == 1) {
    uVar4 = 0x10000242;
  }
  *puVar1 = 0xc0017900;
  puVar1[1] = uVar4;
  puVar1[2] = param_2 & 0x3f;
  *(undefined4 **)(param_1 + 0x10) = puVar1 + 3;
  return;
}

