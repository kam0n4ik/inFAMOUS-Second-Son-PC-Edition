#include "scene/character_model.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_map>

#include "core/clock.h"
#include "core/file.h"
#include "core/log.h"
#include "resources/kcap.h"
#include "resources/spcmesh.h"
#include "resources/texture.h"
#include "resources/texture_library.h"

namespace sspc {
namespace {

bool SameShape(const ModelPartInfo& a, const ModelPartInfo& b) {
    auto close = [](const Vec3& x, const Vec3& y) {
        return std::abs(x.x - y.x) < 1e-4f && std::abs(x.y - y.y) < 1e-4f && std::abs(x.z - y.z) < 1e-4f;
    };
    return a.triangle_count == b.triangle_count && close(a.bounds_min, b.bounds_min) && close(a.bounds_max, b.bounds_max);
}

void FillParts(LoadedModel& m) {
    m.parts.clear();
    for (const auto& md : m.meshes) {
        ModelPartInfo p;
        p.source_offset = md.source_offset;
        p.view_mask = md.view_mask;
        p.vertex_count = static_cast<u32>(md.positions.size());
        p.triangle_count = static_cast<u32>(md.indices.size() / 3);
        p.bounds_min = md.bounds_min;
        p.bounds_max = md.bounds_max;
        m.parts.push_back(p);
    }
    ChooseDefaultParts(m);
}

// Colour maps (material category 1) of every part: looked up in the model's package first, then in
// the shared texture packages; decoded on the job system.
void LoadPartTextures(const res::KcapPackage& pkg, const std::vector<std::wstring>& texture_packages, JobSystem& jobs,
                      LoadedModel& m) {
    res::TextureLibrary lib;
    lib.AddPackage(pkg);
    for (const auto& p : texture_packages) {
        if (!lib.AddPackage(p)) log::Warn("model", "texture package {} not available", log::Narrow(p));
    }
    std::vector<u64> wanted;
    u32 missing = 0;
    m.part_albedo.assign(m.meshes.size(), 0);
    for (size_t i = 0; i < m.meshes.size(); ++i) {
        const auto mt = res::ReadMaterialTextures(pkg, m.meshes[i].source_offset);
        if (!mt || !mt->slots[1]) continue;
        if (!lib.Contains(mt->slots[1])) {
            ++missing;
            continue;
        }
        m.part_albedo[i] = mt->slots[1];
        if (std::find(wanted.begin(), wanted.end(), mt->slots[1]) == wanted.end()) wanted.push_back(mt->slots[1]);
    }
    m.textures.resize(wanted.size());
    jobs.ParallelFor(static_cast<u32>(wanted.size()), [&](u32 i) { lib.Decode(wanted[i], m.textures[i], nullptr); });
    std::erase_if(m.textures, [](const res::TextureData& t) { return t.mips.empty(); });
    log::Info("model", "{} colour maps for {} parts; {} parts reference colour maps not found in {} packages",
              m.textures.size(), std::count_if(m.part_albedo.begin(), m.part_albedo.end(), [](u64 v) { return v != 0; }),
              missing, 1 + texture_packages.size());
}

// Skeleton of the package; skin data that refers to joints outside it is dropped (rigid fallback).
void AttachSkeleton(const res::KcapPackage& pkg, LoadedModel& m) {
    m.skeleton = res::FindSkeleton(pkg);
    const size_t n = m.skeleton ? m.skeleton->JointCount() : 0;
    u32 skinned = 0, dropped = 0;
    for (auto& md : m.meshes) {
        if (md.joints.empty()) continue;
        bool ok = n > 0;
        for (const auto& j : md.joints) ok = ok && j[0] < n && j[1] < n && j[2] < n && j[3] < n;
        if (ok) {
            ++skinned;
        } else {
            md.joints.clear();
            md.weights.clear();
            ++dropped;
        }
    }
    log::Info("model", "skeleton: {} joints; {} skinned parts{}", n, skinned,
              dropped ? std::format(", {} parts left rigid (joint out of range)", dropped) : std::string{});
}

// Helper-joint drivers and rigid-part attachments (see LoadedModel). Needs the CPU meshes.
void ComputeAttachments(LoadedModel& m) {
    m.joint_driver.clear();
    m.part_attach.assign(m.meshes.size(), -1);
    if (!m.skeleton) return;
    const res::Skeleton& sk = *m.skeleton;
    const size_t n = sk.JointCount();
    auto bind_pos = [&](size_t j) { return Vec3{sk.bind[j].m[3][0], sk.bind[j].m[3][1], sk.bind[j].m[3][2]}; };
    std::vector<char> helper(n, 0);
    for (size_t j = 2; j < n; ++j) helper[j] = Length(bind_pos(j)) < 1e-5f && sk.parents[j] <= 0;
    // Co-weight of every helper joint with every body joint.
    std::vector<std::unordered_map<u16, float>> score(n);
    std::vector<Vec3> centroid(n);
    std::vector<float> centroid_w(n, 0);
    for (const auto& md : m.meshes) {
        for (size_t v = 0; v < md.joints.size(); ++v) {
            const auto& jj = md.joints[v];
            const auto& ww = md.weights[v];
            for (int a = 0; a < 4; ++a) {
                if (!ww[a] || jj[a] >= n || !helper[jj[a]]) continue;
                centroid[jj[a]] += md.positions[v] * static_cast<float>(ww[a]);
                centroid_w[jj[a]] += ww[a];
                for (int b = 0; b < 4; ++b) {
                    if (ww[b] && jj[b] < n && !helper[jj[b]]) score[jj[a]][jj[b]] += static_cast<float>(ww[a]) * ww[b];
                }
            }
        }
    }
    auto nearest_body = [&](const Vec3& p) {
        s16 best = -1;
        float bd = 1e30f;
        for (size_t j = 0; j < n; ++j) {
            if (helper[j]) continue;
            const float d = Length(bind_pos(j) - p);
            if (d < bd) {
                bd = d;
                best = static_cast<s16>(j);
            }
        }
        return best;
    };
    m.joint_driver.assign(n, -1);
    u32 drivers = 0;
    for (size_t h = 0; h < n; ++h) {
        if (!helper[h]) continue;
        if (!score[h].empty()) {
            const auto best = std::max_element(score[h].begin(), score[h].end(),
                                               [](const auto& x, const auto& y) { return x.second < y.second; });
            m.joint_driver[h] = static_cast<s16>(best->first);
        } else if (centroid_w[h] > 0) {
            m.joint_driver[h] = nearest_body(centroid[h] * (1.0f / centroid_w[h]));
        }
        drivers += m.joint_driver[h] >= 0;
    }
    // Rigid parts: lowest common ancestor of the joints nearest to a sample of their vertices.
    auto ancestors = [&](s16 j) {
        std::vector<s16> chain;
        for (; j >= 0; j = sk.parents[static_cast<size_t>(j)]) chain.push_back(j);
        return chain;
    };
    u32 attached = 0;
    for (size_t i = 0; i < m.meshes.size(); ++i) {
        const auto& md = m.meshes[i];
        if (!md.joints.empty() || md.positions.empty() || !m.parts[i].default_visible) continue;
        std::vector<s16> common;
        const size_t step = std::max<size_t>(1, md.positions.size() / 48);
        for (size_t v = 0; v < md.positions.size(); v += step) {
            const auto chain = ancestors(nearest_body(md.positions[v]));
            if (common.empty()) {
                common = chain;
            } else {
                std::erase_if(common, [&](s16 x) { return std::find(chain.begin(), chain.end(), x) == chain.end(); });
            }
        }
        if (!common.empty()) {
            m.part_attach[i] = common.front();  // deepest shared joint
            ++attached;
        }
    }
    log::Info("model", "approximations: {} helper joints follow body joints, {} rigid parts ride on joints", drivers, attached);
}

}  // namespace

void ChooseDefaultParts(LoadedModel& m) {
    bool any = false;
    for (size_t i = 0; i < m.parts.size(); ++i) {
        ModelPartInfo& p = m.parts[i];
        p.default_visible = (p.view_mask & 0x2) != 0;
        for (size_t j = 0; j < i && p.default_visible; ++j) {
            if (m.parts[j].default_visible && SameShape(m.parts[j], p)) p.default_visible = false;
        }
        if (!p.default_visible) continue;
        if (!any) {
            m.bounds_min = p.bounds_min;
            m.bounds_max = p.bounds_max;
            any = true;
        } else {
            m.bounds_min = {std::min(m.bounds_min.x, p.bounds_min.x), std::min(m.bounds_min.y, p.bounds_min.y),
                            std::min(m.bounds_min.z, p.bounds_min.z)};
            m.bounds_max = {std::max(m.bounds_max.x, p.bounds_max.x), std::max(m.bounds_max.y, p.bounds_max.y),
                            std::max(m.bounds_max.z, p.bounds_max.z)};
        }
    }
}

bool LoadCharacterModel(const std::wstring& package_path, const std::wstring& cache_path,
                        const std::vector<std::wstring>& texture_packages, JobSystem& jobs, LoadedModel& out,
                        std::string& error) {
    const Clock clock;
    out = LoadedModel{};
    res::KcapFile pkg_file;
    if (const res::KcapError e = pkg_file.Open(package_path); e != res::KcapError::Ok) {
        error = std::format("{}: {}", log::Narrow(package_path), res::ToString(e));
        return false;
    }
    const res::KcapPackage& pkg = pkg_file.Package();
    out.source_hash = pkg.Header().hash;

    if (!cache_path.empty() && file::Exists(cache_path)) {
        file::MappedFile cache;
        std::string cache_error;
        if (cache.Open(cache_path) && cache.Bytes().size() >= sizeof(res::SpcMeshHeader)) {
            res::SpcMeshHeader h;
            std::memcpy(&h, cache.Bytes().data(), sizeof(h));
            if (std::memcmp(h.source_hash, out.source_hash.data(), 16) == 0 &&
                res::ReadSpcMesh(cache.Bytes(), out.meshes, &cache_error)) {
                out.from_cache = true;
                FillParts(out);
                LoadPartTextures(pkg, texture_packages, jobs, out);
                AttachSkeleton(pkg, out);
                ComputeAttachments(out);
                out.load_seconds = clock.Now();
                log::Info("model", "{} meshes from cache {} in {:.1f} ms", out.meshes.size(),
                          log::Narrow(cache_path), out.load_seconds * 1000);
                return true;
            }
            log::Warn("model", "cache {} rejected ({}), decoding the package", log::Narrow(cache_path),
                      cache_error.empty() ? "source hash differs" : cache_error);
        }
    }

    std::vector<res::GeometryRecord> records;
    if (auto root = res::ReadRootGeometryList(pkg)) {
        for (u32 i = 0; i < root->count; ++i) {
            if (auto ptr = pkg.PointerAt(root->list + i * 8)) {
                if (auto rec = res::ReadGeometry(pkg, *ptr)) records.push_back(std::move(*rec));
            }
        }
        log::Info("model", "root geometry list: {} entries, {} valid records", root->count, records.size());
    }
    if (records.empty()) {
        records = res::FindGeometry(pkg);
        log::Info("model", "no root list: {} geometry records found by link scan", records.size());
    }
    if (records.empty()) {
        error = "no geometry records in package";
        return false;
    }
    std::vector<res::MeshData> meshes(records.size());
    std::vector<std::string> errors(records.size());
    std::vector<char> ok(records.size(), 0);
    jobs.ParallelFor(static_cast<u32>(records.size()), [&](u32 i) {
        ok[i] = res::DecodeGeometry(pkg, records[i], meshes[i], &errors[i]) ? 1 : 0;
    });
    for (size_t i = 0; i < records.size(); ++i) {
        if (ok[i]) {
            out.meshes.push_back(std::move(meshes[i]));
        } else {
            log::Warn("model", "skipped part: {}", errors[i]);
        }
    }
    if (out.meshes.empty()) {
        error = "no decodable geometry";
        return false;
    }
    FillParts(out);
    LoadPartTextures(pkg, texture_packages, jobs, out);
    AttachSkeleton(pkg, out);
    ComputeAttachments(out);
    if (!cache_path.empty()) {
        const auto bytes = res::WriteSpcMesh(out.meshes, out.source_hash);
        const size_t slash = cache_path.find_last_of(L"\\/");
        if (slash != std::wstring::npos) file::CreateDirectories(cache_path.substr(0, slash));
        if (!file::WriteAtomic(cache_path, bytes)) log::Warn("model", "cannot write cache {}", log::Narrow(cache_path));
    }
    out.load_seconds = clock.Now();
    log::Info("model", "{} meshes decoded from {} in {:.1f} ms", out.meshes.size(), log::Narrow(package_path),
              out.load_seconds * 1000);
    return true;
}

}  // namespace sspc
