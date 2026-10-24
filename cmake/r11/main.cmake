# SecondSonPC R11 main (docs/R11_MAIN_THREAD.md): the game's main thread - its label waits asleep (SSPC_R11_LABEL_WAIT=1),
# off by default. SSPC_R11_MAIN_SOURCES go into sspc_runtime.
set(SSPC_R11_MAIN_SOURCES
    src/runtime/gpu/r11/label_wait.cpp
)
