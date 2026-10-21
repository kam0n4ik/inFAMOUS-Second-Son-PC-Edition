// SecondSonPC R7 — the Native Render Layer (native_render_layer.h). R7-Core C3: Submit carries the G-buffer's instanced
// draw into the P5.3 native stream (native_draw.cpp writes it); everything else falls back to the game's path.
#include <cstring>

#include <windows.h>

#include "base/log.h"
#include "engine/render/native_draw.h"
#include "engine/render/native_render_layer.h"

namespace SecondSon::Engine::Render::R7 {
namespace {

Mode ReadMode() {
    char v[32] = {};
    const DWORD n = GetEnvironmentVariableA("SSPC_R7_NATIVE_RENDER", v, sizeof(v));
    if (n == 0 || n >= sizeof(v) || std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0) {
        return Mode::Off;
    }
    if (std::strcmp(v, "gbuffer") == 0 || std::strcmp(v, "1") == 0) {
        return Mode::GBuffer;
    }
    if (std::strcmp(v, "gbuffer-verify") == 0) {
        return Mode::GBufferVerify;
    }
    SSPC_LOG(Engine, Error, "SSPC_R7_NATIVE_RENDER={}: want 0, gbuffer or gbuffer-verify - off", v);
    return Mode::Off;
}

bool g_gbuffer_path = false;
thread_local DrawScope t_scope;

/// SSPC_R8_RENDER_PATHS: a comma list of mesh, shadow (unknown words logged and ignored).
u32 ReadPaths() {
    char v[64] = {};
    const DWORD n = GetEnvironmentVariableA("SSPC_R8_RENDER_PATHS", v, sizeof(v));
    if (n == 0 || n >= sizeof(v)) {
        return 0;
    }
    u32 bits = 0;
    const char* p = v;
    while (*p != 0) {
        const char* e = p;
        while (*e != 0 && *e != ',') {
            ++e;
        }
        const size_t len = static_cast<size_t>(e - p);
        if (len == 4 && std::strncmp(p, "mesh", 4) == 0) {
            bits |= kPathMesh;
        } else if (len == 6 && std::strncmp(p, "shadow", 6) == 0) {
            bits |= kPathShadow;
        } else if (len != 0) {
            SSPC_LOG(Engine, Error, "SSPC_R8_RENDER_PATHS={}: want mesh and/or shadow", v);
        }
        p = *e == ',' ? e + 1 : e;
    }
    return bits;
}

bool ReadOn(const char* name) {
    char v[8] = {};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    return n > 0 && n < sizeof(v) && v[0] != '0';
}

constexpr u64 kFnvBasis = 0xcbf29ce484222325ull;
constexpr u64 kFnvPrime = 0x100000001b3ull;

void Mix(u64& h, u64 v) {
    for (u32 i = 0; i < 8; ++i) {
        h = (h ^ ((v >> (i * 8)) & 0xFF)) * kFnvPrime;
    }
}

} // namespace

Mode GetMode() {
    static const Mode mode = ReadMode();
    return mode;
}

bool Enabled() {
    return GetMode() != Mode::Off;
}

LayerCounters& Counters() {
    static LayerCounters c;
    return c;
}

void SetGBufferPath(bool on) {
    g_gbuffer_path = on;
}

bool GBufferPath() {
    return g_gbuffer_path;
}

ScopeCounters& ScopeStats() {
    static ScopeCounters c;
    return c;
}

u32 R8Paths() {
    static const u32 paths = ReadPaths();
    return paths;
}

bool Carries(EngineFn fn) {
    switch (fn) {
    case EngineFn::GBufferInstanced:
        return Enabled();
    case EngineFn::GBufferMesh:
        return Enabled() && (R8Paths() & kPathMesh) != 0;
    case EngineFn::ShadowInstanced:
        return Enabled() && (R8Paths() & kPathShadow) != 0;
    default:
        return false;
    }
}

bool ResourcesOn() {
    static const bool on = ReadOn("SSPC_R8_RENDER_RESOURCES");
    return on;
}

DrawScope OpenScope(u64 r, u32 tag, EngineFn fn) {
    const DrawScope outer = t_scope;
    t_scope = DrawScope{r, tag, 0, fn};
    ScopeStats().scopes.fetch_add(1, std::memory_order_relaxed);
    return outer;
}

u32 CloseScope(const DrawScope& outer) {
    const u32 native = t_scope.native;
    t_scope = outer;
    if (native != 0) {
        ScopeCounters& s = ScopeStats();
        s.native_calls.fetch_add(1, std::memory_order_relaxed);
        s.native_draws.fetch_add(native, std::memory_order_relaxed);
    }
    return native;
}

bool InScope(u64 r, u32& tag) {
    const DrawScope& s = t_scope;
    if (s.r == 0 || s.r != r) {
        return false;
    }
    tag = s.tag;
    return true;
}

bool InScope(u64 r, u32& tag, EngineFn& fn) {
    if (!InScope(r, tag)) {
        return false;
    }
    fn = t_scope.fn;
    return true;
}

void ScopeNative() {
    ++t_scope.native;
}

SubmitResult Submit(const NativeRenderContext& ctx, const NativeDrawCommand& cmd) {
    const u32 p = static_cast<u32>(cmd.producer) < 5 ? static_cast<u32>(cmd.producer) : 0;
    LayerCounters& c = Counters();
    c.submitted[p].fetch_add(1, std::memory_order_relaxed);
    if (!Enabled()) {
        return SubmitResult::Off;
    }
    // C3: the G-buffer's instanced draw (the scope its probe opened); R8 NR5: the mesh / shadow draws with their switch
    if (Carries(cmd.fn) && NativeDraw::EmitNativeDraw(ctx, cmd, GetMode() == Mode::GBufferVerify)) {
        c.native[p].fetch_add(1, std::memory_order_relaxed);
        return SubmitResult::Native;
    }
    c.fallback[p].fetch_add(1, std::memory_order_relaxed);
    return SubmitResult::Fallback;
}

u64 StateKey(const NativeDrawCommand& cmd) {
    const NativeDrawState& s = cmd.state;
    if (!s.shaders) {
        return 0;
    }
    u64 h = kFnvBasis;
    for (const u32 v : s.vs) {
        Mix(h, v);
    }
    Mix(h, s.has_ps ? 1 : 0);
    for (const u32 v : s.ps) {
        Mix(h, v);
    }
    Mix(h, s.prim_type | (u64{s.index_size} << 32));
    Mix(h, s.num_instances);
    Mix(h, cmd.resources.cue);
    for (const NativeStageResources& st : cmd.resources.stage) {
        if (!st.present) {
            Mix(h, 0);
            continue;
        }
        if (st.version == 0) {
            return 0; // resources not tracked: never reused
        }
        Mix(h, st.version);
        Mix(h, st.shape.dwords | (u64{st.shape.refs} << 16));
        for (u32 i = 0; i < st.shape.refs && i < 7; ++i) {
            Mix(h, st.shape.sgpr[i] | (u64{st.shape.offset[i]} << 8));
        }
        Mix(h, st.direct_mask);
    }
    return h == 0 ? 1 : h;
}

} // namespace SecondSon::Engine::Render::R7
