# SecondSonPC R10 sched (docs/R10_CPU_GPU_SCHEDULER.md): the input-to-present latency probe (SSPC_R10_LATENCY=1) and the
# bounded work-stealing pool of the pipeline workers (SSPC_R10_SCHED=1), off by default. SSPC_R10_SCHED_SOURCES go into
# sspc_runtime.
set(SSPC_R10_SCHED_SOURCES
    src/runtime/gpu/r10/latency.cpp
    src/runtime/gpu/r10/spread.cpp
    src/runtime/gpu/r10/work_pool.cpp
)
