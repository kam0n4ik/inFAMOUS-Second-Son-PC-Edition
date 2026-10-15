// FUN_006227e0 @0x6227e0 size=86
// callers: 0x19190 0x19380 0x195a0 0x19770 0x11f910 0x175150 0x190300 0x190320 0x190c90 0x19e110 0x19e300 0x1a3b10 0x1a4080 0x1a4fd0 0x1a5a30 0x1edd80 0x1edfb0 0x1feed0 0x3012b0 0x301410 0x301720 0x301c90 0x3bfb90 0x453e30 0x454230 0x454680 0x455180 0x459030 0x4e66a0 0x589a80 0x597a60
// callees: 

void FUN_006227e0(long param_1,undefined4 param_2)

{
  char cVar1;
  undefined4 *puVar2;
  
  puVar2 = *(undefined4 **)(param_1 + 0x10);
  if ((uint)((ulong)(*(long *)(param_1 + 8) - (long)puVar2) >> 2) < 3) {
    cVar1 = (**(code **)(param_1 + 0x18))(param_1,3,*(undefined8 *)(param_1 + 0x20));
    if (cVar1 == '\0') {
      return;
    }
    puVar2 = *(undefined4 **)(param_1 + 0x10);
  }
  *puVar2 = 0xc0016900;
  puVar2[1] = 0x8e;
  puVar2[2] = param_2;
  *(undefined4 **)(param_1 + 0x10) = puVar2 + 3;
  return;
}

