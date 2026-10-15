// FUN_00120020 @0x120020 size=518
// callers: 0x1f1e30
// callees: 0x11f910(FUN_0011f910) 0x6229a0(FUN_006229a0) 0x622bb0(FUN_00622bb0) 0x622c10(FUN_00622c10) 0x626a80(FUN_00626a80) 0x626c60(FUN_00626c60) 0x626e10(FUN_00626e10) 0x6270e0(FUN_006270e0)

void FUN_00120020(long param_1,int *param_2)

{
  long lVar1;
  uint uVar2;
  byte bVar3;
  
  uVar2 = param_2[6];
  if (((uVar2 & 1) != 0) &&
     (((*(uint *)(param_1 + 0xde60) & 1) == 0 || (*(int *)(param_1 + 0xde48) != *param_2)))) {
    *(int *)(param_1 + 0xde48) = *param_2;
    *(uint *)(param_1 + 0xde60) = *(uint *)(param_1 + 0xde60) | 1;
    FUN_006270e0(param_1 + 8);
    uVar2 = param_2[6];
  }
  if (((uVar2 & 2) != 0) &&
     (((*(uint *)(param_1 + 0xde60) & 2) == 0 || (*(int *)(param_1 + 0xde4c) != param_2[1])))) {
    *(int *)(param_1 + 0xde4c) = param_2[1];
    *(uint *)(param_1 + 0xde60) = *(uint *)(param_1 + 0xde60) | 2;
    FUN_00626e10(param_1 + 8);
    uVar2 = param_2[6];
  }
  if (((uVar2 & 4) != 0) &&
     (((*(uint *)(param_1 + 0xde60) & 4) == 0 || (*(int *)(param_1 + 0xde50) != param_2[2])))) {
    *(int *)(param_1 + 0xde50) = param_2[2];
    *(uint *)(param_1 + 0xde60) = *(uint *)(param_1 + 0xde60) | 4;
    FUN_00626c60(param_1 + 8);
    uVar2 = param_2[6];
  }
  if (((uVar2 & 8) != 0) &&
     (((*(uint *)(param_1 + 0xde60) & 8) == 0 || (*(int *)(param_1 + 0xde54) != param_2[3])))) {
    *(int *)(param_1 + 0xde54) = param_2[3];
    *(uint *)(param_1 + 0xde60) = *(uint *)(param_1 + 0xde60) | 8;
    FUN_00626a80(param_1 + 8);
    uVar2 = param_2[6];
  }
  if (((uVar2 & 0x10) != 0) &&
     (((*(uint *)(param_1 + 0xde60) & 0x10) == 0 || (*(int *)(param_1 + 0xde58) != param_2[4])))) {
    *(int *)(param_1 + 0xde58) = param_2[4];
    *(uint *)(param_1 + 0xde60) = *(uint *)(param_1 + 0xde60) | 0x10;
    FUN_00622bb0(param_1 + 8);
    uVar2 = param_2[6];
  }
  if (((uVar2 & 0x20) != 0) &&
     (((*(uint *)(param_1 + 0xde60) & 0x20) == 0 || (*(int *)(param_1 + 0xde5c) != param_2[5])))) {
    *(int *)(param_1 + 0xde5c) = param_2[5];
    *(uint *)(param_1 + 0xde60) = *(uint *)(param_1 + 0xde60) | 0x20;
    FUN_00622c10(param_1 + 8);
  }
  if (*(int *)(param_1 + 0xde64) != param_2[7]) {
    uVar2 = param_2[7];
    bVar3 = (byte)(uVar2 >> 8);
    if ((uVar2 & 0xff | (uint)bVar3 << 8 | uVar2 & 0xff0000 | uVar2 & 0xff000000) !=
        *(uint *)(param_1 + 0xde64)) {
      *(char *)(param_1 + 0xde64) = (char)uVar2;
      *(byte *)(param_1 + 0xde65) = bVar3;
      *(char *)(param_1 + 0xde66) = (char)(uVar2 >> 0x10);
      *(char *)(param_1 + 0xde67) = (char)(uVar2 >> 0x18);
      FUN_006229a0(param_1 + 8);
    }
  }
  lVar1 = *(long *)(param_2 + 8);
  if ((lVar1 != 0) && (lVar1 != *(long *)(param_1 + 0xde68))) {
    FUN_0011f910(param_1,lVar1,param_2 + 10);
    return;
  }
  return;
}

