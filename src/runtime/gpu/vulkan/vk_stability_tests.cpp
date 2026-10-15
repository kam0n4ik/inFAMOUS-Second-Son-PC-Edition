// P4.15A integration tests: real Vulkan objects, injected API results, isolated cache files.
// No game assets or user caches are touched. API hooks are single-threaded and restored before teardown.
#include <windows.h>
#include <SDL3/SDL.h>
#include <chrono>
#include <fstream>
#include <thread>
#include <vector>
#include <fmt/format.h>
#include "runtime/gpu/vulkan/vk_stability_tests.h"
#include "runtime/gpu/vulkan/vk_device_context.h"
#include "runtime/gpu/vulkan/vk_executor.h"
#include "runtime/gpu/vulkan/vk_buffer_cache.h"
#include "runtime/gpu/vulkan/vk_image_cache.h"
#include "runtime/gpu/vulkan/vk_pipelines.h"
#include "runtime/gpu/vulkan/vk_precompile.h"
#include "runtime/gpu/vulkan/vk_presenter.h"
#include "runtime/gpu/shader/shader_frontend.h"
#include "runtime/gpu/shader/shader_disk_cache.h"

namespace SecondSon::Gpu::Vk {
struct StabilityTestAccess {
    static bool Submit(Executor& e) {
        if (!e.Begin()) return false;
        e.Note(0, 1);
        e.Flush();
        return !e.Failed();
    }
    static bool Quarantined(const Executor& e) {
        return e.in_flight_.empty() && e.abandoned_.size() == 1 && e.free_.empty() &&
               e.Stats().completed_batches == 0;
    }
};
namespace {
Api real_api;
VkResult next_wait = VK_SUCCESS, next_submit = VK_SUCCESS, next_acquire = VK_SUCCESS;
u32 destroys = 0, resets = 0, submits = 0;
VKAPI_ATTR VkResult VKAPI_CALL Wait(VkDevice d, u32 n, const VkFence* f, VkBool32 all, u64 t) {
    if (next_wait != VK_SUCCESS) { const auto r = next_wait; next_wait = VK_SUCCESS; return r; }
    return real_api.vkWaitForFences(d, n, f, all, t);
}
VKAPI_ATTR VkResult VKAPI_CALL Submit(VkQueue q, u32 n, const VkSubmitInfo2* s, VkFence f) {
    ++submits;
    if (next_submit != VK_SUCCESS) { const auto r = next_submit; next_submit = VK_SUCCESS; return r; }
    return real_api.vkQueueSubmit2(q, n, s, f);
}
VKAPI_ATTR VkResult VKAPI_CALL Reset(VkDevice d, u32 n, const VkFence* f) {
    ++resets;
    return real_api.vkResetFences(d, n, f);
}
VKAPI_ATTR void VKAPI_CALL Destroy(VkDevice d, VkSwapchainKHR s, const VkAllocationCallbacks* a) {
    ++destroys;
    real_api.vkDestroySwapchainKHR(d, s, a);
}
VKAPI_ATTR VkResult VKAPI_CALL Acquire(VkDevice d, VkSwapchainKHR s, u64 t, VkSemaphore sem, VkFence f, u32* i) {
    if (next_acquire != VK_SUCCESS) { const auto r = next_acquire; next_acquire = VK_SUCCESS; return r; }
    return real_api.vkAcquireNextImageKHR(d, s, t, sem, f, i);
}
struct Hooks {
    Hooks() {
        real_api = Fn();
        next_wait = next_submit = next_acquire = VK_SUCCESS;
        destroys = resets = submits = 0;
        Fn().vkWaitForFences = Wait; Fn().vkQueueSubmit2 = Submit;
        Fn().vkResetFences = Reset; Fn().vkDestroySwapchainKHR = Destroy; Fn().vkAcquireNextImageKHR = Acquire;
    }
    ~Hooks() { Fn() = real_api; }
};
struct Results {
    std::ofstream out; u32 checks=0, failed=0;
    explicit Results(const std::filesystem::path& path):out(path){}
    void Check(bool ok, const std::string& name) {
        ++checks; failed += !ok;
        out << (ok ? "PASS " : "FAIL ") << name << '\n'; out.flush();
    }
};
std::vector<char> Bytes(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
bool Context(std::unique_ptr<DeviceContext>& ctx, const std::filesystem::path& logs, bool present, Results& r) {
    std::string error;
    ctx = DeviceContext::Create({.validation=true, .presentation=present, .validation_log=logs}, error);
    r.Check(ctx != nullptr, "create real Vulkan context: " + error);
    if (ctx) r.Check(ctx->Caps().validation, "validation layer actually enabled (not silently skipped)");
    return ctx != nullptr;
}
void TestPresenter(const std::filesystem::path& logs, Results& r, VkResult injected) {
    std::unique_ptr<DeviceContext> ctx;
    if (!Context(ctx, logs / fmt::format("present_{}.log", static_cast<int>(injected)), true, r)) return;
    SDL_Window* window = SDL_CreateWindow("SecondSonPC P4.15A tests", 320, 240, SDL_WINDOW_RESIZABLE);
    r.Check(window != nullptr, "create isolated SDL test window");
    if (!window) return;
    {
        Timeline timeline(*ctx); std::string error;
        auto p = Presenter::Create(*ctx, timeline, window, error);
        r.Check(p != nullptr, "create presenter: " + error);
        if (p) {
            {
                Hooks hooks;
                p->Present(-1, 1);
                const u32 before_destroy=destroys, before_reset=resets, before_submit=submits;
                next_wait = injected;
                const bool recreated = p->TestRecreate();
                r.Check(!recreated && destroys==before_destroy && resets==before_reset && submits==before_submit,
                        "failed retirement leaves swapchain/semaphores/fences untouched");
                if (injected==VK_TIMEOUT) {
                    r.Check(!ctx->Failed(), "retirement timeout is retryable");
                    r.Check(p->TestRecreate(), "retry after timeout retires and recreates");
                    p->Present(-1, 2); p->Present(-1, 3);
                    const auto s=submits, rr=resets;
                    next_wait=VK_TIMEOUT; p->Present(-1, 4);
                    r.Check(submits==s && resets==rr, "slot timeout never resets or reuses pending slot");
                    next_acquire=VK_ERROR_OUT_OF_DATE_KHR; p->Present(-1, 5);
                    r.Check(!ctx->Failed(), "out-of-date acquire is recreated without a false submission");
                    p->Present(-1, 6);
                    SDL_SetWindowSize(window, 400, 300); SDL_PumpEvents(); p->Present(-1, 7);
                    r.Check(!ctx->Failed(), "normal resize/present resumes");
                } else {
                    r.Check(ctx->Failure()==injected, "fatal wait result reaches shared backend failure state");
                    const auto s=submits; p->Present(-1, 8);
                    r.Check(submits==s, "failed presenter stops submitting");
                }
            } // real API restored before destructor fences are waited
            p.reset();
        }
        r.Check(ctx->ValidationErrors()==0, "present lifetime validation: " + ctx->FirstValidationError());
    }
    SDL_DestroyWindow(window);
}
void TestExecutor(const std::filesystem::path& logs, Results& r, VkResult injected) {
    std::unique_ptr<DeviceContext> ctx;
    if (!Context(ctx, logs / fmt::format("submit_{}.log", static_cast<int>(injected)), false, r)) return;
    {
        Timeline timeline(*ctx);
        StreamRing ring(*ctx, timeline, 1<<20, HostPlacement::Cached);
        GuestBuffers guest(*ctx);
        BufferCache buffers(*ctx, guest, nullptr, 64ull<<20, false);
        ImageCache images(*ctx, guest);
        GS::DeviceProfile profile; ctx->QueryProfile(profile);
        PipelineFactory pipelines(*ctx, profile);
        GS::ShaderFrontend frontend({.async=false, .profile=&profile});
        OwnedBuffer gds;
        Executor e(*ctx, timeline, ring, guest, buffers, images, pipelines, frontend, nullptr, gds, {});
        {
            Hooks hooks; next_submit=injected;
            const bool ok=StabilityTestAccess::Submit(e);
            if(injected==VK_SUCCESS){
                r.Check(ok && timeline.Wait(timeline.LastSubmitted(), 5'000'000'000ull), "successful submit completes");
                e.Poll();
                r.Check(e.Stats().completed_batches==1, "only successful submission is retired");
            }else{
                r.Check(!ok && ctx->Failure()==injected && StabilityTestAccess::Quarantined(e),
                        "failed submit quarantined, never in-flight or recycled");
                e.Poll(); e.Synchronize(0);
                r.Check(!StabilityTestAccess::Submit(e) && submits==1 && e.Stats().completed_batches==0,
                        "no subsequent submit or false GPU completion after failure");
                const auto begin=std::chrono::steady_clock::now();
                r.Check(!timeline.Wait(timeline.LastSubmitted(), 5'000'000'000ull) &&
                        std::chrono::steady_clock::now()-begin < std::chrono::milliseconds(100),
                        "failed timeline wait returns immediately without waiting for an unsignalled value");
            }
        }
        Fn().vkDeviceWaitIdle(ctx->Device());
    }
    r.Check(ctx->ValidationErrors()==0, "submit validation: " + ctx->FirstValidationError());
}
void TestCache(const std::filesystem::path& logs, Results& r) {
    std::unique_ptr<DeviceContext> ctx;
    if (!Context(ctx, logs / "cache_validation.log", false, r)) return;
    const auto dir=logs / "cache";
    GS::ShaderDiskCache disk; std::string error;
    if(!disk.Open({.dir=dir,.generation="p415a-tests"},error)){r.Check(false,error);return;}
    PipelineCacheFile owner(*ctx,disk);
    r.Check(owner.Load() && owner.Save(), "owner publishes real driver cache");
    const auto original=Bytes(dir/"vulkan.bin");
    {
        GS::ShaderDiskCache reader;
        r.Check(reader.Open({.dir=dir,.generation="p415a-tests"},error) && !reader.Writable(),
                "contending cache is read-only");
        PipelineCacheFile readonly(*ctx,reader);
        r.Check(readonly.Load() && !readonly.Save() && readonly.Saves()==0 &&
                Bytes(dir/"vulkan.bin")==original && !std::filesystem::exists(dir/"vulkan.bin.tmp"),
                "read-only Save cannot write driver file or shared tmp");
    }
    owner.TestTearNextSave(7);
    r.Check(!owner.Save() && Bytes(dir/"vulkan.bin")==original, "torn temp preserves original atomic publication");
    r.Check(owner.Save(), "owner recovers after torn temp");
    SetEnvironmentVariableA("SSPC_DIAG_PIPELINE_CACHE_DRIVER","1");
    PipelineCacheFile mismatch(*ctx,disk);
    r.Check(mismatch.Load() && mismatch.LoadNote().find("another driver")!=std::string::npos,
            "simulated incompatible driver profile is rejected");
    SetEnvironmentVariableA("SSPC_DIAG_PIPELINE_CACHE_DRIVER",nullptr);
    disk.Close();
    r.Check(!owner.Save(), "closing owner revokes all persistent write permission");
    r.Check(ctx->ValidationErrors()==0, "cache Vulkan validation");
}
int CacheProcess(const std::filesystem::path& logs, const std::filesystem::path& dir, bool writer) {
    Results r(logs/"cache-process.txt");
    if (!std::filesystem::exists(dir/"P415A-TEST-ONLY")) {r.Check(false,"missing test-only marker");return 1;}
    std::unique_ptr<DeviceContext> ctx;
    if (!Context(ctx,logs/"cache-process-validation.log",false,r)) return 1;
    GS::ShaderDiskCache disk;std::string error;
    r.Check(disk.Open({.dir=dir,.generation="p415a-tests"},error), "open shared test generation "+error);
    r.Check(disk.Writable()==writer, writer?"exclusive owner acquired":"second process is read-only");
    PipelineCacheFile cache(*ctx,disk);
    r.Check(cache.Load(),"load driver cache in independent process");
    if(writer){
        r.Check(cache.Save(),"initial owner save");
        std::ofstream(dir/"owner.ready")<<"ready";
        const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        while(!std::filesystem::exists(dir/"reader.done") && std::chrono::steady_clock::now()<end){
            if(!cache.Save()){r.Check(false,"owner concurrent save");break;}
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        r.Check(std::filesystem::exists(dir/"reader.done"),"reader finished before deadline");
    }else{
        bool refused=true;
        for(u32 i=0;i<1000;++i){refused &= !cache.Save();}
        r.Check(refused && cache.Saves()==0,"1000 concurrent reader saves refused");
        std::ofstream(dir/"reader.done")<<"done";
    }
    r.Check(ctx->ValidationErrors()==0,"two-process validation");
    return r.failed?1:0;
}
} // namespace

int RunStabilityTests(const std::filesystem::path& logs) {
    std::filesystem::create_directories(logs);
    char role[16]={}; wchar_t root[32768]={};
    if(GetEnvironmentVariableA("SSPC_P415A_CACHE_ROLE",role,sizeof(role)) &&
       GetEnvironmentVariableW(L"SSPC_P415A_CACHE_ROOT",root,32768)){
        return CacheProcess(logs,root,std::string_view(role)=="writer");
    }
    Results r(logs/"p415a-tests.txt");
    if(!SDL_Init(SDL_INIT_VIDEO)){r.Check(false,SDL_GetError());return 1;}
    TestPresenter(logs,r,VK_TIMEOUT);
    TestPresenter(logs,r,VK_ERROR_DEVICE_LOST);
    TestPresenter(logs,r,VK_ERROR_OUT_OF_HOST_MEMORY);
    TestExecutor(logs,r,VK_SUCCESS);
    TestExecutor(logs,r,VK_ERROR_OUT_OF_HOST_MEMORY);
    TestExecutor(logs,r,VK_ERROR_OUT_OF_DEVICE_MEMORY);
    TestExecutor(logs,r,VK_ERROR_DEVICE_LOST);
    TestCache(logs,r);
    SDL_Quit();
    r.out << fmt::format("TOTAL {} checks, {} failures\n",r.checks,r.failed);
    return r.failed?1:0;
}
} // namespace SecondSon::Gpu::Vk
