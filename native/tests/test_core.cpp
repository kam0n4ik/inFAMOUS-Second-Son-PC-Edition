#include <windows.h>

#include <atomic>
#include <cstring>

#include "core/config.h"
#include "core/crc32.h"
#include "core/file.h"
#include "core/jobs.h"
#include "core/math.h"
#include "test.h"

using namespace sspc;

TEST(config_parses_sections_types_and_comments) {
    Config c;
    const bool ok = c.Parse("\xEF\xBB\xBF; comment\n[Game]\nDataDir = \"C:\\Games\\SS\"\n  Hz=0x3c \n"
                            "# another\n[render]\nVSync = off\nScale=0.75\n");
    CHECK(ok);
    CHECK(c.Errors().empty());
    CHECK_EQ(c.GetString("game", "datadir", ""), std::string("C:\\Games\\SS"));
    CHECK_EQ(c.GetInt("Game", "Hz", 0), 60);
    CHECK_EQ(c.GetBool("Render", "VSync", true), false);
    CHECK(std::abs(c.GetFloat("Render", "Scale", 0) - 0.75) < 1e-9);
    CHECK_EQ(c.GetInt("Render", "Missing", 7), 7);
    CHECK_EQ(c.GetInt("Render", "Scale", 7), 7);  // not an integer -> default
}

TEST(config_reports_malformed_lines_with_numbers) {
    Config c;
    CHECK(!c.Parse("key_outside = 1\n[Ok]\nno_equals_sign\n[broken\nGood = yes\n"));
    CHECK_EQ(c.Errors().size(), static_cast<size_t>(3));
    CHECK(c.Errors()[0].find("line 1") != std::string::npos);
    CHECK(c.Errors()[1].find("line 3") != std::string::npos);
    CHECK_EQ(c.GetBool("Ok", "Good", false), true);  // valid lines still applied
}

TEST(crc32_matches_reference_vector) {
    const char* s = "123456789";
    CHECK_EQ(Crc32(std::span(reinterpret_cast<const u8*>(s), 9)), 0xCBF43926u);
    CHECK_EQ(Crc32(std::span<const u8>{}), 0u);
}

TEST(atomic_write_and_mapped_read_roundtrip) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring path = std::wstring(tmp) + L"sspc_native_test.bin";
    std::vector<u8> data(100000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<u8>(i * 31);
    CHECK(file::WriteAtomic(path, data));
    CHECK(!file::Exists(path + L".tmp"));
    file::MappedFile m;
    CHECK(m.Open(path));
    CHECK(m.Bytes().size() == data.size() && std::memcmp(m.Bytes().data(), data.data(), data.size()) == 0);
    m.Close();
    std::vector<u8> small = {1, 2, 3};
    CHECK(file::WriteAtomic(path, small));  // replace existing
    std::vector<u8> back;
    CHECK(file::ReadAll(path, back) && back == small);
    DeleteFileW(path.c_str());
}

TEST(job_system_parallel_for_runs_every_index_once) {
    JobSystem jobs(3);
    std::vector<std::atomic<int>> hits(1000);
    jobs.ParallelFor(1000, [&](u32 i) { hits[i].fetch_add(1); });
    bool all_once = true;
    for (auto& h : hits) all_once &= h.load() == 1;
    CHECK(all_once);
    std::atomic<int> n{0};
    for (int i = 0; i < 50; ++i) jobs.Submit([&] { n.fetch_add(1); });
    jobs.WaitIdle();
    CHECK_EQ(n.load(), 50);
}

TEST(math_projection_maps_near_far_to_vulkan_depth) {
    const Mat4 p = Mat4::Perspective(1.0f, 1.0f, 0.1f, 100.0f);
    auto depth = [&](float z) {  // view space point on the -Z axis
        const float clip_z = p.m[2][2] * z + p.m[3][2];
        const float clip_w = p.m[2][3] * z;
        return clip_z / clip_w;
    };
    CHECK(std::abs(depth(-0.1f) - 0.0f) < 1e-5f);
    CHECK(std::abs(depth(-100.0f) - 1.0f) < 1e-5f);
    const Mat4 v = Mat4::LookAt({0, -5, 0}, {0, 0, 0}, {0, 0, 1});
    const Vec3 t = v.TransformPoint({0, 0, 0});
    CHECK(std::abs(t.z + 5.0f) < 1e-5f && std::abs(t.x) < 1e-5f && std::abs(t.y) < 1e-5f);
}
