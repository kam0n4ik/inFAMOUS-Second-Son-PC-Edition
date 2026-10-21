# SecondSonPC R8: the sources of R8-A3 Native Render (feature/r8-render-layer): the Native Render Layer's working path
# (docs/NATIVE_ENGINE_R8_CORE.md §3). SSPC_R8_RENDER_SOURCES go into sspc_runtime, SSPC_R8_RENDER_TEST_SOURCES into
# sspc_native_engine_tests.
set(SSPC_R8_RENDER_SOURCES
)
# (the tests run the layer's native commit - engine/render/native_draw.cpp - against the eboot's commit and the Native
# Resource State: the engine's render side links into sspc_native_engine_tests here, nowhere else)
set(SSPC_R8_RENDER_TEST_SOURCES
    tools/native_engine_tests/r8_render_tests.cpp
    src/engine/render/native_draw.cpp
    src/engine/render/native_render_layer.cpp
    src/engine/render/native_render_ops.cpp
    src/engine/render/native_state.cpp
)
