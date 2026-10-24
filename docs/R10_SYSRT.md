# R10 sysrt: оставшиеся системные функции игры в собственном runtime

Ветка `feature/r10-sysrt` (worktree `E:\SecondSonPC-r10-sysrt`, база origin/main 6d74185). Коммиты кода: a18986e
(реализация, self-test, инструменты), 02bf2eb (инструменты: ожидание флага тишины).
Сырые данные: `E:\research\r10\runs\sysrt\`. Метки: [E] измерено, [A] анализ бинарника/кода, [H] гипотеза.

## 1. Итог

| Что | До (`bridge`) | После (`SSPC_R10_SYSRT=native`) |
|---|---|---|
| Владельцы 417 импортов: NATIVE / BRIDGE / COMMON_STUB / UNRESOLVED [E, binding table] | 338 / 59 / 18 / 2 | 350 / 49 / 17 / 1 |
| Вызванные за запуск импорты не у SecondSonPC (обычный путь: запуск, загрузка, город) [E] | 9: CommonDialog 1, Net 3, libc 3 (`vprintf` — COMMON_STUB), Ajm 2 | 2: Ajm 2 (зона агента ajm) |
| То же с ручным сохранением из «Настройки → Сохранение» [E] | +3 SaveDataDialog | +3 SaveDataDialog (заблокировано, §5) |

Перенесены все вызываемые функции из зоны sysrt: `sceCommonDialogInitialize`, `sceNetInit`, `sceNetPoolCreate`,
`sceNetInetNtop`, `vprintf`, `fflush`, `sceLibcHeapGetTraceInfo`. Вместе с ними перенесены функции с общим
состоянием (`sceNetTerm`, `sceNetPoolDestroy`, `sceNetInetPton`, `sceNetErrnoLoc`) и объект `_Stdout`.
Вызываемыми и не у SecondSonPC остались:
* `sceAjmInitialize` и `sceAjmModuleRegister` — libSceAjm, зона агента ajm.
* `sceSaveDataDialog{Initialize,Open,UpdateStatus}` — системный UI-диалог списка сохранений. Он вызывается
  только из ручного «Сохранение» в настройках. Причина блокировки в §5.

Флаг: `SSPC_R10_SYSRT=native` включает группы `sysrt-dialog`, `sysrt-net`, `sysrt-libc`. При `bridge` или без
флага всё работает как раньше. Каждую группу можно включить отдельно (`--native <группа>`) или вернуть мосту
(`--bridge <группа>`).

## 2. Список вызываемых функций: откуда и как сверено

* `generated/CALLS.md` в репозитории есть только в старом виде (Phase 0, 2026-10-08, все 4622 через мост; лежит в
  `E:\SecondSonPC-r9\docs\generated\`). В публичном дереве `docs/generated` нет. Поэтому актуальный список
  взят из прогонов (счётчик импортов, `logs\calls.json`) и из сводки загрузчика (`binding table` в
  SecondSonPC.log).
* Сверено с `E:\SecondSonPC-r9\logs\calls.json` (22:04 10-23) и с моими прогонами (§6). Вызванных импортов,
  которые обслуживает не SecondSonPC, было 9: CommonDialog 1, Net 3, libc 3 (из них `vprintf` — COMMON_STUB),
  Ajm 2. Каждый вызывается один раз за запуск.
* Поправка к ТЗ: `SSPC_R9_IMPORTS` к счётчику импортов отношения не имеет. Это журнал host-pointer импортов
  Vulkan в `vk_resources.cpp`. Счётчик включается в toml (`importCounter = true`, по умолчанию включён).

## 3. Таблица (генератор)

`python -I tools/r10/sysrt_deps.py --run before=<прогон> --run after=<прогон> --md F --csv F`

Генератор ничего не хранит вручную, поэтому его нужно перезапускать после каждого слияния. Входы:
`src/loader/generated/import_table.inc` (все импорты eboot/libc/Fios2, вид, импортёры), блок `binding table`
из SecondSonPC.log прогона (владельцы) и `calls.json` (число вызовов). Ручной словарь `NOTES` нужен только для
пояснений (ABI, побочные эффекты, адреса вызова), владельцев он не задаёт. Прогоны снимаются через
`tools/r10/sysrt_run.ps1`: это r9run плюс копирование calls.json и call_order.txt.

Классы: `own` — у SecondSonPC; `ajm` — зона агента ajm; `called` — вызывается, но не у SecondSonPC (после
переноса таких нет); `object` — неразрешённый объект; `stub` — COMMON_STUB, не вызывался; `idle` — у моста,
импортирован, но ни разу не вызывался.

Прогоны таблицы: before = s1/b_city_menu (bridge), after = s0/x_save_native (native, с ручным сохранением).

| run | imports | NATIVE | OBSERVED | BRIDGE | COMMON_STUB | UNRESOLVED | LLE |
|---|---:|---:|---:|---:|---:|---:|---:|
| before | 417 | 338 | 0 | 59 | 18 | 2 | 147 |
| after | 417 | 350 | 0 | 49 | 17 | 1 | 147 |

| NID | function | library/module | importers | ABI | before owner/calls | after owner/calls | class | priority | side effects | note |
|---|---|---|---|---|---|---|---|---|---|---|
| `s9e3+YpRnzw` | `sceSaveDataDialogInitialize` | libSceSaveDataDialog/libSceSaveDataDialog | eboot | SysV | BRIDGE / 0 | BRIDGE / 1 | called | P1 | system UI (save list) over the game | the save-list system dialog: Settings > Save opens it [E]; drawn by the bridge's UI layer - an own one needs a system dialog UI on SecondSonPC's presenter (blocked: not a runtime function, separate task); CommonDialog state shared through the sysrt-dialog adapter |
| `4tPhsP6FpDI` | `sceSaveDataDialogOpen` | libSceSaveDataDialog/libSceSaveDataDialog | eboot | SysV | BRIDGE / 0 | BRIDGE / 1 | called | P1 | system UI (save list) over the game | the save-list system dialog: Settings > Save opens it [E]; drawn by the bridge's UI layer - an own one needs a system dialog UI on SecondSonPC's presenter (blocked: not a runtime function, separate task); CommonDialog state shared through the sysrt-dialog adapter |
| `KK3Bdg1RWK0` | `sceSaveDataDialogUpdateStatus` | libSceSaveDataDialog/libSceSaveDataDialog | eboot | SysV | BRIDGE / 0 | BRIDGE / 121 | called | P1 | system UI (save list) over the game | the save-list system dialog: Settings > Save opens it [E]; drawn by the bridge's UI layer - an own one needs a system dialog UI on SecondSonPC's presenter (blocked: not a runtime function, separate task); CommonDialog state shared through the sysrt-dialog adapter |
| `uoUpLGNkygk` | `sceCommonDialogInitialize` | libSceCommonDialog/libSceCommonDialog | eboot | SysV, () -> s32 | BRIDGE / 1 | NATIVE / 1 | own | P1 | module state: initialized flag read by MsgDialog/SaveData-/ErrorDialog | sysrt-dialog; eboot 0x15d7d9 ignores the result [A] |
| `2sWzhYqFH4E` | `_Stdout` | libSceLibcInternal/libSceLibcInternal | Fios2 | data object (FILE) | UNRESOLVED / 0 | NATIVE / 0 | own | P1 | - | sysrt-libc exports the object: Fios2's fflush(_Stdout) gets a real FILE* instead of NULL [A] |
| `MUjC4lbHrK4` | `fflush` | libSceLibcInternal/libSceLibcInternal | Fios2 | SysV, (FILE*) -> int | BRIDGE / 1 | NATIVE / 1 | own | P1 | libc stdout buffer | sysrt-libc; Fios2 0x38650 after vprintf, FILE = _Stdout slot [A] |
| `GMpvxPFW924` | `vprintf` | libSceLibcInternal/libSceLibcInternal | Fios2 | SysV, (const char*, va_list) -> int; guest va_list = SysV struct | COMMON_STUB / 1 | NATIVE / 1 | own | P1 | stdout text | sysrt-libc; Fios2 0x38650 log printer [A] |
| `NWtTN10cJzE` | `sceLibcHeapGetTraceInfo` | libSceLibcInternalExt/libSceLibcInternal | libc | SysV, (HeapTraceInfo* 32 B) -> void | BRIDGE / 1 | NATIVE / 1 | own | P1 | heap trace state of libc.prx | sysrt-libc; libc.prx 0x5b8dc/0x5bd79 [A] |
| `HQOwnfMGipQ` | `sceNetErrnoLoc` | libSceNet/libSceNet | eboot | SysV | BRIDGE / 0 | NATIVE / 0 | own | P3 | host sockets | offline: NetCtl (NATIVE) reports no network, the game opens no socket [E]; errno shared through the sysrt-net adapter |
| `9vA2aW+CHuA` | `sceNetInetNtop` | libSceNet/libSceNet | eboot | SysV, (s32 af, const void*, char*, u32) -> const char* | BRIDGE / 1 | NATIVE / 1 | own | P1 | net errno | sysrt-net; eboot 0x5fd253: AF_INET, buffer 0x20 [A] |
| `8Kcp5d-q1Uo` | `sceNetInetPton` | libSceNet/libSceNet | eboot | SysV | BRIDGE / 0 | NATIVE / 0 | own | P3 | host sockets | offline: NetCtl (NATIVE) reports no network, the game opens no socket [E]; errno shared through the sysrt-net adapter |
| `Nlev7Lg8k3A` | `sceNetInit` | libSceNet/libSceNet | eboot | SysV, () -> s32 | BRIDGE / 1 | NATIVE / 1 | own | P1 | libSceNet state | sysrt-net; eboot 0x5ed272 compares with 0 and 0x804101c8 [A] |
| `dgJBaeJnGpo` | `sceNetPoolCreate` | libSceNet/libSceNet | eboot | SysV, (const char*, s32 size, s32 flags) -> s32 id | BRIDGE / 1 | NATIVE / 1 | own | P1 | pool table 3000..3030 | sysrt-net; eboot 0x23699f: size 0x4000, flags 0; id -> sceSslInit/sceHttpInit [A] |
| `K7RlrTkI-mw` | `sceNetPoolDestroy` | libSceNet/libSceNet | eboot | SysV | BRIDGE / 0 | NATIVE / 0 | own | P3 | host sockets | offline: NetCtl (NATIVE) reports no network, the game opens no socket [E]; errno shared through the sysrt-net adapter |
| `cTGkc6-TBlI` | `sceNetTerm` | libSceNet/libSceNet | eboot | SysV | BRIDGE / 0 | NATIVE / 0 | own | P3 | host sockets | offline: NetCtl (NATIVE) reports no network, the game opens no socket [E]; errno shared through the sysrt-net adapter |
| `WfAiBW8Wcek` | `sceAjmBatchErrorDump` | libSceAjm/libSceAjm | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | ajm | P3 |  | libSceAjm: R10 agent ajm |
| `dmDybN--Fn8` | `sceAjmBatchJobControlBufferRa` | libSceAjm/libSceAjm | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | ajm | P3 |  | libSceAjm: R10 agent ajm |
| `7jdAXK+2fMo` | `sceAjmBatchJobRunSplitBufferRa` | libSceAjm/libSceAjm | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | ajm | P3 |  | libSceAjm: R10 agent ajm |
| `fFFkk0xfGWs` | `sceAjmBatchStartBuffer` | libSceAjm/libSceAjm | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | ajm | P3 |  | libSceAjm: R10 agent ajm |
| `-qLsfDAywIY` | `sceAjmBatchWait` | libSceAjm/libSceAjm | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | ajm | P3 |  | libSceAjm: R10 agent ajm |
| `MHur6qCsUus` | `sceAjmFinalize` | libSceAjm/libSceAjm | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | ajm | P3 |  | libSceAjm: R10 agent ajm |
| `dl+4eHSzUu4` | `sceAjmInitialize` | libSceAjm/libSceAjm | eboot | SysV | BRIDGE / 1 | BRIDGE / 1 | ajm | P3 |  | libSceAjm: R10 agent ajm |
| `AxoDrINp4J8` | `sceAjmInstanceCreate` | libSceAjm/libSceAjm | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | ajm | P3 |  | libSceAjm: R10 agent ajm |
| `RbLbuKv8zho` | `sceAjmInstanceDestroy` | libSceAjm/libSceAjm | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | ajm | P3 |  | libSceAjm: R10 agent ajm |
| `Q3dyFuwGn64` | `sceAjmModuleRegister` | libSceAjm/libSceAjm | eboot | SysV | BRIDGE / 1 | BRIDGE / 1 | ajm | P3 |  | libSceAjm: R10 agent ajm |
| `Wi7DtlLV+KI` | `sceAjmModuleUnregister` | libSceAjm/libSceAjm | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | ajm | P3 |  | libSceAjm: R10 agent ajm |
| `ZT4ODD2Ts9o` | `Need_sceLibcInternal` | libSceLibcInternal/libSceLibcInternal | Fios2 | data object (link marker) | UNRESOLVED / 0 | UNRESOLVED / 0 | object | P3 | - | link marker of libSceLibcInternal; its slot (Fios2 0x60068) is read by no instruction (data_refs.py) [A] |
| `H2e8t5ScQGc` | `__cxa_finalize` | libSceLibcInternal/libSceLibcInternal | Fios2 | SysV | COMMON_STUB / 0 | COMMON_STUB / 0 | stub | P3 | returns 0, logged | imported by Fios2, never called in any run [E] (the loader logs each COMMON_STUB call); exit/crash paths and Fios2's unused helpers |
| `tIhsqj0qsFE` | `free` | libSceLibcInternal/libSceLibcInternal | Fios2 | SysV | COMMON_STUB / 0 | COMMON_STUB / 0 | stub | P3 | returns 0, logged | imported by Fios2, never called in any run [E] (the loader logs each COMMON_STUB call); exit/crash paths and Fios2's unused helpers |
| `1mecP7RgI2A` | `gmtime` | libSceLibcInternal/libSceLibcInternal | Fios2 | SysV | COMMON_STUB / 0 | COMMON_STUB / 0 | stub | P3 | returns 0, logged | imported by Fios2, never called in any run [E] (the loader logs each COMMON_STUB call); exit/crash paths and Fios2's unused helpers |
| `5bBacGLyLOs` | `gmtime_s` | libSceLibcInternal/libSceLibcInternal | Fios2 | SysV | COMMON_STUB / 0 | COMMON_STUB / 0 | stub | P3 | returns 0, logged | imported by Fios2, never called in any run [E] (the loader logs each COMMON_STUB call); exit/crash paths and Fios2's unused helpers |
| `efhK-YSUYYQ` | `localtime` | libSceLibcInternal/libSceLibcInternal | Fios2 | SysV | COMMON_STUB / 0 | COMMON_STUB / 0 | stub | P3 | returns 0, logged | imported by Fios2, never called in any run [E] (the loader logs each COMMON_STUB call); exit/crash paths and Fios2's unused helpers |
| `gQX+4GDQjpM` | `malloc` | libSceLibcInternal/libSceLibcInternal | Fios2 | SysV | COMMON_STUB / 0 | COMMON_STUB / 0 | stub | P3 | returns 0, logged | imported by Fios2, never called in any run [E] (the loader logs each COMMON_STUB call); exit/crash paths and Fios2's unused helpers |
| `n7AepwR0s34` | `mktime` | libSceLibcInternal/libSceLibcInternal | Fios2 | SysV | COMMON_STUB / 0 | COMMON_STUB / 0 | stub | P3 | returns 0, logged | imported by Fios2, never called in any run [E] (the loader logs each COMMON_STUB call); exit/crash paths and Fios2's unused helpers |
| `hcuQgD53UxM` | `printf` | libSceLibcInternal/libSceLibcInternal | Fios2 | SysV | COMMON_STUB / 0 | COMMON_STUB / 0 | stub | P3 | returns 0, logged | imported by Fios2, never called in any run [E] (the loader logs each COMMON_STUB call); exit/crash paths and Fios2's unused helpers |
| `wLlFkwG9UcQ` | `time` | libSceLibcInternal/libSceLibcInternal | Fios2 | SysV | COMMON_STUB / 0 | COMMON_STUB / 0 | stub | P3 | returns 0, logged | imported by Fios2, never called in any run [E] (the loader logs each COMMON_STUB call); exit/crash paths and Fios2's unused helpers |
| `Q2V+iqvjgC0` | `vsnprintf` | libSceLibcInternal/libSceLibcInternal | Fios2 | SysV | COMMON_STUB / 0 | COMMON_STUB / 0 | stub | P3 | returns 0, logged | imported by Fios2, never called in any run [E] (the loader logs each COMMON_STUB call); exit/crash paths and Fios2's unused helpers |
| `Fjc4-n1+y2g` | `__elf_phdr_match_addr` | libkernel/libkernel | libc | SysV | COMMON_STUB / 0 | COMMON_STUB / 0 | stub | P3 | returns 0, logged | imported by libc, never called in any run [E] (the loader logs each COMMON_STUB call); exit/crash paths and Fios2's unused helpers |
| `kbw4UHHSYy0` | `__pthread_cxa_finalize` | libkernel/libkernel | libc | SysV | COMMON_STUB / 0 | COMMON_STUB / 0 | stub | P3 | returns 0, logged | imported by libc, never called in any run [E] (the loader logs each COMMON_STUB call); exit/crash paths and Fios2's unused helpers |
| `crb5j7mkk1c` | `_is_signal_return` | libkernel/libkernel | libc | SysV | COMMON_STUB / 0 | COMMON_STUB / 0 | stub | P3 | returns 0, logged | imported by libc, never called in any run [E] (the loader logs each COMMON_STUB call); exit/crash paths and Fios2's unused helpers |
| `hHlZQUnlxSM` | `getrusage` | libkernel/libkernel | libc | SysV | COMMON_STUB / 0 | COMMON_STUB / 0 | stub | P3 | returns 0, logged | imported by libc, never called in any run [E] (the loader logs each COMMON_STUB call); exit/crash paths and Fios2's unused helpers |
| `-YTW+qXc3CQ` | `sceKernelInternalMemoryGetModuleSegmentInfo` | libkernel/libkernel | libc | SysV | COMMON_STUB / 0 | COMMON_STUB / 0 | stub | P3 | returns 0, logged | imported by libc, never called in any run [E] (the loader logs each COMMON_STUB call); exit/crash paths and Fios2's unused helpers |
| `Wl2o5hOVZdw` | `sceKernelPrintBacktraceWithModuleInfo` | libkernel/libkernel | libc | SysV | COMMON_STUB / 0 | COMMON_STUB / 0 | stub | P3 | returns 0, logged | imported by libc, never called in any run [E] (the loader logs each COMMON_STUB call); exit/crash paths and Fios2's unused helpers |
| `mo0bFmWppIw` | `sigreturn` | libkernel/libkernel | libc | SysV | COMMON_STUB / 0 | COMMON_STUB / 0 | stub | P3 | returns 0, logged | imported by libc, never called in any run [E] (the loader logs each COMMON_STUB call); exit/crash paths and Fios2's unused helpers |
| `I88KChlynSs` | `sceErrorDialogInitialize` | libSceErrorDialog/libSceErrorDialog | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | system UI | system error dialog; CommonDialog state shared through the sysrt-dialog adapter |
| `M2ZF-ClLhgY` | `sceErrorDialogOpen` | libSceErrorDialog/libSceErrorDialog | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | system UI | system error dialog; CommonDialog state shared through the sysrt-dialog adapter |
| `9XAxK2PMwk8` | `sceErrorDialogTerminate` | libSceErrorDialog/libSceErrorDialog | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | system UI | system error dialog; CommonDialog state shared through the sysrt-dialog adapter |
| `WWiGuh9XfgQ` | `sceErrorDialogUpdateStatus` | libSceErrorDialog/libSceErrorDialog | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | system UI | system error dialog; CommonDialog state shared through the sysrt-dialog adapter |
| `lDqxaY1UbEo` | `sceMsgDialogInitialize` | libSceMsgDialog/libSceMsgDialog | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | system UI | system message dialog; CommonDialog state shared through the sysrt-dialog adapter |
| `b06Hh0DPEaE` | `sceMsgDialogOpen` | libSceMsgDialog/libSceMsgDialog | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | system UI | system message dialog; CommonDialog state shared through the sysrt-dialog adapter |
| `6fIC3XKt2k0` | `sceMsgDialogUpdateStatus` | libSceMsgDialog/libSceMsgDialog | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | system UI | system message dialog; CommonDialog state shared through the sysrt-dialog adapter |
| `PIWqhn9oSxc` | `sceNetAccept` | libSceNet/libSceNet | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | host sockets | offline: NetCtl (NATIVE) reports no network, the game opens no socket [E]; errno shared through the sysrt-net adapter |
| `bErx49PgxyY` | `sceNetBind` | libSceNet/libSceNet | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | host sockets | offline: NetCtl (NATIVE) reports no network, the game opens no socket [E]; errno shared through the sysrt-net adapter |
| `OXXX4mUk3uk` | `sceNetConnect` | libSceNet/libSceNet | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | host sockets | offline: NetCtl (NATIVE) reports no network, the game opens no socket [E]; errno shared through the sysrt-net adapter |
| `ZVw46bsasAk` | `sceNetEpollControl` | libSceNet/libSceNet | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | host sockets | offline: NetCtl (NATIVE) reports no network, the game opens no socket [E]; pools and errno shared through the sysrt-net adapter |
| `SF47kB2MNTo` | `sceNetEpollCreate` | libSceNet/libSceNet | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | host sockets | offline: NetCtl (NATIVE) reports no network, the game opens no socket [E]; pools and errno shared through the sysrt-net adapter |
| `Inp1lfL+Jdw` | `sceNetEpollDestroy` | libSceNet/libSceNet | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | host sockets | offline: NetCtl (NATIVE) reports no network, the game opens no socket [E]; pools and errno shared through the sysrt-net adapter |
| `drjIbDbA7UQ` | `sceNetEpollWait` | libSceNet/libSceNet | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | host sockets | offline: NetCtl (NATIVE) reports no network, the game opens no socket [E]; pools and errno shared through the sysrt-net adapter |
| `kOj1HiAGE54` | `sceNetListen` | libSceNet/libSceNet | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | host sockets | offline: NetCtl (NATIVE) reports no network, the game opens no socket [E]; errno shared through the sysrt-net adapter |
| `9wO9XrMsNhc` | `sceNetRecv` | libSceNet/libSceNet | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | host sockets | offline: NetCtl (NATIVE) reports no network, the game opens no socket [E]; errno shared through the sysrt-net adapter |
| `304ooNZxWDY` | `sceNetRecvfrom` | libSceNet/libSceNet | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | host sockets | offline: NetCtl (NATIVE) reports no network, the game opens no socket [E]; errno shared through the sysrt-net adapter |
| `C4UgDHHPvdw` | `sceNetResolverCreate` | libSceNet/libSceNet | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | host DNS | offline, never started [E]; the bridge's resolvers take SecondSonPC's pool ids (sysrt-net adapter) |
| `kJlYH5uMAWI` | `sceNetResolverDestroy` | libSceNet/libSceNet | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | host DNS | offline, never started [E]; the bridge's resolvers take SecondSonPC's pool ids (sysrt-net adapter) |
| `Apb4YDxKsRI` | `sceNetResolverStartAton` | libSceNet/libSceNet | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | host DNS | offline, never started [E]; the bridge's resolvers take SecondSonPC's pool ids (sysrt-net adapter) |
| `Nd91WaWmG2w` | `sceNetResolverStartNtoa` | libSceNet/libSceNet | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | host DNS | offline, never started [E]; the bridge's resolvers take SecondSonPC's pool ids (sysrt-net adapter) |
| `beRjXBn-z+o` | `sceNetSend` | libSceNet/libSceNet | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | host sockets | offline: NetCtl (NATIVE) reports no network, the game opens no socket [E]; errno shared through the sysrt-net adapter |
| `gvD1greCu0A` | `sceNetSendto` | libSceNet/libSceNet | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | host sockets | offline: NetCtl (NATIVE) reports no network, the game opens no socket [E]; errno shared through the sysrt-net adapter |
| `2mKX2Spso7I` | `sceNetSetsockopt` | libSceNet/libSceNet | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | host sockets | offline: NetCtl (NATIVE) reports no network, the game opens no socket [E]; errno shared through the sysrt-net adapter |
| `TSM6whtekok` | `sceNetShutdown` | libSceNet/libSceNet | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | host sockets | offline: NetCtl (NATIVE) reports no network, the game opens no socket [E]; errno shared through the sysrt-net adapter |
| `Q4qBuN-c0ZM` | `sceNetSocket` | libSceNet/libSceNet | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | host sockets | offline: NetCtl (NATIVE) reports no network, the game opens no socket [E]; errno shared through the sysrt-net adapter |
| `zJGf8xjFnQE` | `sceNetSocketAbort` | libSceNet/libSceNet | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | host sockets | offline: NetCtl (NATIVE) reports no network, the game opens no socket [E]; errno shared through the sysrt-net adapter |
| `45ggEzakPJQ` | `sceNetSocketClose` | libSceNet/libSceNet | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | host sockets | offline: NetCtl (NATIVE) reports no network, the game opens no socket [E]; errno shared through the sysrt-net adapter |
| `yEiJ-qqr6Cg` | `sceSaveDataDialogGetResult` | libSceSaveDataDialog/libSceSaveDataDialog | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | system UI (save list) over the game | the save-list system dialog: Settings > Save opens it [E]; drawn by the bridge's UI layer - an own one needs a system dialog UI on SecondSonPC's presenter (blocked: not a runtime function, separate task); CommonDialog state shared through the sysrt-dialog adapter |
| `YuH2FA7azqQ` | `sceSaveDataDialogTerminate` | libSceSaveDataDialog/libSceSaveDataDialog | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | system UI (save list) over the game | the save-list system dialog: Settings > Save opens it [E]; drawn by the bridge's UI layer - an own one needs a system dialog UI on SecondSonPC's presenter (blocked: not a runtime function, separate task); CommonDialog state shared through the sysrt-dialog adapter |
| `tZY4+SZNFhA` | `msync` | libkernel/libkernel | libc | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | memory | memory API, never called [E] |
| `0NTHN1NKONI` | `sceKernelConvertLocaltimeToUtc` | libkernel/libkernel | libc | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | - | time zone conversion, never called [E] |
| `-o5uEDpN+oY` | `sceKernelConvertUtcToLocaltime` | libkernel/libkernel | libc | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | - | time zone conversion, never called [E] |
| `L0v2Go5jOuM` | `sceKernelGetPrtAperture` | libkernel/libkernel | eboot | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | GPU memory | partially resident textures: GPU memory (Phase 4) |
| `3k6kx-zOOSQ` | `sceKernelMlock` | libkernel/libkernel | libc | SysV | BRIDGE / 0 | BRIDGE / 0 | idle | P3 | memory | memory API, never called [E] |

classes (last run): called 3, own 12, ajm 11, object 1, stub 17, idle 35

## 4. Реализации

### 4.1 CommonDialog (`src/runtime/system/common_dialog.cpp`, группа sysrt-dialog)

* Вызов в игре [A]: eboot 0x15d7d9, один раз при старте; результат не проверяется.
* Жизненный цикл как у эталона (shadPS4 `commondialog.cpp`). Флаг выставляется через atomic exchange: первый
  вызов возвращает 0, все последующие — `0x80B80002` (ALREADY_SYSTEM_INITIALIZED), и состояние при этом не
  меняется. Функции завершения у библиотеки нет.
* Состояние общее с мостом. MsgDialog, SaveDataDialog и ErrorDialog остаются у моста (импортированы, но не
  вызываются, §5) и проверяют собственный `CommonDialog::g_isInitialized`. Адаптер `sysrt_host.cpp`
  выставляет этот флаг при первом успешном вызове. Без адаптера диалог моста вернул бы `0x80B80001`
  (NOT_SYSTEM_INITIALIZED); это проверено self-test.

### 4.2 libSceNet (`src/runtime/net/net_core.cpp`, группа sysrt-net)

* `sceNetInit` (eboot 0x5ed272 [A]): результат сравнивается с 0 и с `0x804101c8`; ссылки считает сама игра.
  Возвращает 0, потому что инициализацию хоста делает адаптер.
* `sceNetPoolCreate` (eboot 0x23699f [A]: размер 0x4000, flags 0; id уходит в sceSslInit/sceHttpInit).
  Работает как эталон:
  * id выдаются из диапазона 3000..3030, берётся первый свободный;
  * имя длиннее 31 байта → ENAMETOOLONG;
  * flags≠0 или size<4 КиБ → EINVAL;
  * свободных мест нет → ENFILE;
  * код ошибки возвращается как `0x80410100|e`, errno не выставляется.
  Память под пул не выделяется: ни одна функция SecondSonPC её не берёт.
* `sceNetPoolDestroy`: неизвестный id → EBADF. Если пулом владеет резолвер моста → ENOTEMPTY (ответ даёт
  адаптер).
* `sceNetInetNtop` (eboot 0x5fd253 [A]): AF_INET, адрес из sceNetCtlGetInfo(14) (при её ошибке
  255.255.255.255), буфер 0x20.
  * Форматирование своё, по алгоритму FreeBSD inet_ntop: для AF_INET6 это сжатие самой длинной серии нулей
    и встроенный IPv4.
  * Ошибки: NULL → EFAULT; чужое семейство → EAFNOSUPPORT; буфер мал → ENOSPC. Во всех трёх случаях
    возвращается NULL, буфер не трогается.
* `sceNetInetPton`: алгоритм FreeBSD inet_pton. Возвращает 1 при успехе и 0 при неверном тексте; при неверных
  аргументах — `0x80410100|e` и errno.
* `sceNetErrnoLoc`: отдельный thread_local на каждый поток.
* Офлайн-семантика: успех нигде не изображается. NetCtl уже нативный и сообщает «нет сети», поэтому игра
  до сокетов не доходит [E: 0 вызовов сокетов/epoll/резолверов во всех прогонах].
* Адаптер (`sysrt_host.cpp`) поддерживает согласованность с сокетами и резолверами моста:
  * `sceNetInit` SecondSonPC запускает инициализацию хоста у моста;
  * пулы зеркалируются в таблицу моста с теми же id (обе таблицы берут первый свободный);
  * `SetErrno` моста через его хук пишет и errno SecondSonPC, и, как прежний хук `net_kernel.cpp`, errno
    ядра.
  * Не покрыто: прямые записи errno в `ResolverReturn` моста идут мимо хука.

### 4.3 libSceLibcInternal (`src/runtime/libc/{guest_printf,libc_stdio}.cpp`, группа sysrt-libc)

* Вызов в Fios2 [A] (`tools/r10/import_sites.py`): функция печати 0x38650 выполняет
  `if (hook) return hook(fmt, ap); r = vprintf(fmt, ap); fflush(_Stdout); return r`.
  * `_Stdout` берётся через слот R_X86_64_64 по адресу 0x5c400. Раньше объект не разрешался, слот оставался 0,
    и вызов фактически был `fflush(NULL)`.
  * `vprintf` раньше был COMMON_STUB и возвращал 0 вместо числа символов.
* `vprintf`: va_list гостя имеет формат SysV `{gp_offset, fp_offset, overflow_arg_area, reg_save_area}`
  (static_assert на 24 байта). MSVC-формат не используется.
  * Форматирование своё (`FormatGuest`), по образцу FreeBSD vfprintf: флаги `-+ #0'`, ширина и точность
    (числом или `*`), длины hh h l ll q j z t L.
  * long у гостя 64-битный (LP64). `L` берёт x87 из overflow-области (выравнивание 16) и округляет до double.
  * Конечные float форматирует хостовый snprintf с теми же флагами, шириной и точностью; inf/nan — свои.
  * Форма `%p`: "0x…", как во FreeBSD [H для Sony].
  * Позиционные `%n$` и `%lc`/`%ls` не поддерживаются: директива копируется текстом, в лог пишется
    предупреждение (не больше 16 раз).
  * Вывод идёт в SecondSonPC.log строками (`guest stdout: …`), буфер общий для всех потоков, под мьютексом.
* `fflush`: NULL или `&_Stdout` → сброс буфера → 0. Чужой FILE без ожидающих данных (`_Mode & 0x2000`,
  проверка эталона) → 0; с ожидающими данными → EOF и ошибка в лог (путь недостижим: конструкторов FILE игра
  не импортирует).
* `_Stdout`: собственный объект (0x200 байт, обнулён). Счётчик UNRESOLVED уменьшается на 1, вызов становится
  `fflush(stdout)`.
* `sceLibcHeapGetTraceInfo` (libc.prx 0x5b8dc/0x5bd79 [A]): структура 32 байта — size=0x20, flag=1,
  get_segment_info @0xC, mask @0x10, table @0x18.
  * Как у эталона: get_segment_info=0 (трассировка кучи выключена), mask и table — собственные.
  * В таблице 128 записей против 64 у эталона: индекс в заголовке чанка 7-битный, а libc пишет `[id+1]` [A].
    Поэтому записей нужно не меньше 65.

## 5. Остаток (классификация)

Таблица в §3 строится генератором; здесь — классы остатка и причины, по которым он остаётся у моста.

* **ajm (11, из них 2 вызываются).** libSceAjm — зона агента ajm (R10), не sysrt.
* **SaveDataDialog: 3 вызываются, 2 нет.** Вызываются только из ручного «Настройки → Сохранение»
  [E: x_save_native — Initialize 1, Open 1, UpdateStatus 121].
  * Это системный UI: список слотов сохранения поверх игры. Мост рисует его своим UI-слоем (ImGui,
    `savedatadialog_ui.cpp`).
  * Собственная реализация — это не функция runtime, а системный диалог на презентере SecondSonPC (окно, ввод,
    список слотов из `/savedata0`). Это заблокировано как отдельная задача и не входит в sysrt.
  * Состояние CommonDialog общее (адаптер sysrt-dialog). Без него Initialize моста вернул бы
    NOT_SYSTEM_INITIALIZED.
  * Автосохранения (`sceSaveData*`, они уже NATIVE) диалог не используют.
* **MsgDialog (3), ErrorDialog (4): импортированы, не вызывались** [E]. Состояние CommonDialog общее через
  адаптер.
* **Сокеты, epoll, резолверы libSceNet (19): не вызывались** [E]. NetCtl (NATIVE) сообщает, что сети нет, и игра
  не открывает ни одного сокета. Пулы, инициализация хоста и errno согласованы с мостом через адаптер. Перенос
  имеет смысл только вместе с онлайн-режимом.
* **libkernel у моста (5): `msync`, `sceKernelMlock`, `sceKernelConvert{LocaltimeToUtc,UtcToLocaltime}`,
  `sceKernelGetPrtAperture`.** Не вызывались [E]. PRT относится к памяти GPU (Phase 4).
* **COMMON_STUB (17), не вызывались ни в одном прогоне** [E].
  * Fios2 (10): `malloc`, `free`, `printf`, `vsnprintf`, `time`, `mktime`, `localtime`, `gmtime`, `gmtime_s`,
    `__cxa_finalize`.
  * libc.prx (7): `__elf_phdr_match_addr`, `__pthread_cxa_finalize`, `_is_signal_return`, `getrusage`,
    `sceKernelInternalMemoryGetModuleSegmentInfo`, `sceKernelPrintBacktraceWithModuleInfo`, `sigreturn`. Это
    пути выхода и аварий.
  * Каждый вызов COMMON_STUB загрузчик пишет в лог («COMMON_STUB … called»), так что появление вызова будет
    видно.
* **UNRESOLVED (1): `Need_sceLibcInternal`.** Это маркер линковки. Его слот в Fios2 (0x60068) не читает ни одна
  инструкция [A: `tools/r10/data_refs.py`].

## 6. Проверки

### 6.1 Self-test (оба пути на одних входах)

`powershell -File tools/r10/sysrt_selftest.ps1` запускает `SSPC_SELFTEST_ONLY=sysrt --selftest --loader sspc`
под локом игры.

[E] `E:\research\r10\runs\sysrt\selftest2\selftest.txt`: **102 проверки, 0 расхождений**. В том числе:

* **SysrtDialog — 8 проверок.**
  * Пока Initialize не вызван, msgdialog моста возвращает `0x80B80001`.
  * Первый вызов возвращает 0; диалог моста Init/Terminate → 0; второй вызов → `0x80B80002`, как у эталона.
  * Проверено в 4 потоках.
* **SysrtNet — 63 проверки.**
  * Один и тот же сценарий пулов на мосту и в SecondSonPC: id 3000…3030, ENFILE на 32-м, ENAMETOOLONG,
    EINVAL, EBADF.
  * InetNtop — 24 случая, InetPton — 28: все совпадают с эталоном (INFO).
  * errno отдельный на каждый поток.
  * Адаптер: пул зеркалируется, резолвер моста на пуле → destroy `0x80410142` (ENOTEMPTY); после резолвера →
    0; ResolverCreate на освобождённом пуле → `0x80410109` и errno SecondSonPC 9.
* **SysrtLibc — 31 проверка.**
  * Форматы Fios2 и граничные случаи через va_list гостя, собранный в SysV-структуру (регистры и overflow, x87).
  * vprintf возвращает число символов. fflush(&_Stdout)/fflush(NULL) отправляет строку в лог, проверено в
    4 потоках. Чужой FILE: без данных → 0, с данными → EOF.
  * HeapGetTraceInfo — как у эталона.
  * Эталон (`printf.h` shadPS4) печатает `%ld` хостовым 32-битным long: для 64-битного значения он даёт
    `591751049`. Это зафиксировано как INFO; у нас LP64.

### 6.2 Игра

Стенд: `tools/r10/sysrt_run.ps1` (r9run: пресет стенда, свежая копия сохранения, `--press`; дополнительно
копируются calls.json и call_order.txt). Сохранения проверялись по mtime `dis.dat`/`param.sfo` в копии
(`-KeepSave`) и по счётчику `sceSaveDataSetParam`.

| Прогон | Режим | Сцена / маршрут | Итог | FPS avg / 1% low | Ошибок в логе | Сохранение [E] |
|---|---|---|---|---|---|---|
| s0/x_save_native | native | city: титул → Continue → карта (Esc) → Настройки (F) → Сохранение (Space) | закрыт тестом, exit 0, 95,5 с | 56,7 / 38,4 | 0 | автосейв infamousssAuto1 (dis.dat + param.sfo) через 19 с после старта, SetParam 1; SaveDataDialog Open 1, UpdateStatus 121 |
| s1/b_city_menu | bridge | city: титул → Continue → карта → Настройки → вверх/вниз → Назад (E) → Esc → ходьба | exit 0, 95,5 с | 51,7 / 25,0 | 0 | автосейв infamousssAuto1, SetParam 1 |
| s1/b_user_calm | bridge | user: титул → Continue → стоит 120 с | exit 0, 120,4 с | 30,2 / 19,3 | 0 | автосейв Auto0 и Auto1, SetParam 2 |
| s1/n_city_menu | native | как b_city_menu | NOT TESTED: ждёт стенд (флаг тишины pacing) | | | |
| s1/n_user_calm | native | как b_user_calm | NOT TESTED: ждёт стенд (флаг тишины pacing) | | | |
| s1/b_save_dialog | bridge | как x_save_native | NOT TESTED: ждёт стенд (флаг тишины pacing) | | | |
| s2/контрольный | native | запуск → меню → загрузка → геймплей → сохранение → выход в меню | NOT TESTED: ждёт стенд (флаг тишины pacing) | | | |

FPS приведён только для справки: прогоны функциональные, не A/B. Стенд был занят чужими сериями, а b_user_calm
частично шёл под флагом тишины pacing (см. SHARED_NOTES 11:09).

Во всех native-прогонах:
* сводка загрузчика: 350/49/17/1;
* 12 строк `[sysrt-*]` NATIVE;
* в логе есть `guest stdout: FIOS2: built with SDK version 0x04008061` — первая строка, которую игра выводит через
  vprintf;
* `sceNetPoolCreate("HttpMgr Net Pool", 16384, 0x0) = 3000`;
* `sceCommonDialogInitialize: system dialog layer initialised`.

## 7. Не проверено, риски

* **Ручное сохранение (Настройки → Сохранение).** Игра открывает SaveDataDialog моста и опрашивает
  UpdateStatus (статус RUNNING). В native-прогоне на скриншотах диалога не видно, хотя прошло 30 с после Open.
  Сравнение с bridge — в §6.2. Это функция моста, общая для обоих режимов; sysrt её не меняет.
* **Ошибки резолверов моста.** Прямые записи errno в `ResolverReturn` моста не зеркалируются в
  `sceNetErrnoLoc` SecondSonPC. В офлайне резолверы не вызываются [E].
* **Форма `%p` и нулевое заполнение `%s`/`%c`** взяты из FreeBSD [H для Sony]. `%p` есть в отладочных
  форматах Fios2 (`pDH: %p`). Разница с другой формой затронет только текст в SecondSonPC.log. Позиционные
  `%n$` и `%lc`/`%ls` не поддерживаются: печатаются как текст, с предупреждением в лог.
* **Таблица mstate.** В ней 128 записей против 64 у эталона: libc пишет `[id+1]` [A]. Трассировка кучи
  выключена, как у эталона.
* **Инструменты.** `tools/r10/import_sites.py` берёт `tools/ps4elf.py`, а `sysrt_run.ps1` — `tools/r9/r9run.ps1`.
  Обоих файлов в публичном дереве нет, они лежат в worktree как неотслеживаемые. Генератор `sysrt_deps.py`
  самодостаточен.

## 8. Хеши

* a18986e — код: реализация, адаптеры, self-test, CMake/allowlist, `tools/r10`.
* 02bf2eb — `tools/r10/sysrt_run.ps1`, `sysrt_selftest.ps1`: ожидание флага тишины прямо перед запуском.
* Этот документ — отдельным коммитом (см. `git log -- docs/R10_SYSRT.md`).

Сборка проверок: `build\bin\SecondSonPC.exe` от 09:47:43, это код a18986e. Изменения после неё касаются только
`tools/`.
