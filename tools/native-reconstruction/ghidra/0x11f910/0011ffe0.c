// FUN_0011ffe0 @0x11ffe0 size=51
// callers: 0x198a70 0x1f2c80 0x1f2d00
// callees: 

void FUN_0011ffe0(long param_1,undefined8 *param_2)

{
  undefined8 uVar1;
  undefined8 uVar2;
  undefined8 uVar3;
  undefined8 uVar4;
  undefined8 uVar5;
  undefined8 uVar6;
  undefined8 uVar7;
  
  uVar1 = *param_2;
  uVar2 = param_2[1];
  uVar3 = param_2[2];
  uVar4 = param_2[3];
  uVar5 = param_2[4];
  uVar6 = param_2[5];
  uVar7 = param_2[6];
  *(undefined8 *)(param_1 + 0xde60) = param_2[3];
  *(undefined8 *)(param_1 + 0xde68) = uVar5;
  *(undefined8 *)(param_1 + 0xde70) = uVar6;
  *(undefined8 *)(param_1 + 0xde78) = uVar7;
  *(undefined8 *)(param_1 + 0xde48) = uVar1;
  *(undefined8 *)(param_1 + 0xde50) = uVar2;
  *(undefined8 *)(param_1 + 0xde58) = uVar3;
  *(undefined8 *)(param_1 + 0xde60) = uVar4;
  *(int *)(param_1 + 0xde80) = (1 << (*(byte *)(param_2[4] + 0x77c) & 0x1f)) + -1;
  return;
}

