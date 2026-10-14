#include "resources/spcmesh.h"

#include <cmath>
#include <cstring>
#include <format>

#include "core/crc32.h"

namespace sspc::res {
namespace {

void Append(std::vector<u8>& buf, const void* p, size_t n) {
    const auto* b = static_cast<const u8*>(p);
    buf.insert(buf.end(), b, b + n);
}

void Align(std::vector<u8>& buf, size_t a) {
    while (buf.size() % a) buf.push_back(0);
}

u32 StrideFor(u32 flags) {
    return 12 + ((flags & kVertexNormal) ? 12 : 0) + ((flags & kVertexUv0) ? 8 : 0);
}

}  // namespace

std::vector<u8> WriteSpcMesh(std::span<const MeshData> meshes, const std::array<u8, 16>& source_hash) {
    std::vector<u8> buf(sizeof(SpcMeshHeader) + meshes.size() * sizeof(SpcMeshEntry), 0);
    std::vector<SpcMeshEntry> entries(meshes.size());
    for (size_t m = 0; m < meshes.size(); ++m) {
        const MeshData& md = meshes[m];
        SpcMeshEntry& e = entries[m];
        std::memset(&e, 0, sizeof(e));
        std::memcpy(e.id, md.id.data(), 16);
        e.source_offset = md.source_offset;
        e.vertex_count = static_cast<u32>(md.positions.size());
        e.index_count = static_cast<u32>(md.indices.size());
        e.vertex_flags = kVertexPosition | (md.normals.size() == md.positions.size() && !md.normals.empty() ? kVertexNormal : 0) |
                         (md.uv0.size() == md.positions.size() && !md.uv0.empty() ? kVertexUv0 : 0);
        e.vertex_stride = StrideFor(e.vertex_flags);
        e.bounds_min[0] = md.bounds_min.x; e.bounds_min[1] = md.bounds_min.y; e.bounds_min[2] = md.bounds_min.z;
        e.bounds_max[0] = md.bounds_max.x; e.bounds_max[1] = md.bounds_max.y; e.bounds_max[2] = md.bounds_max.z;
        e.view_mask = md.view_mask;
        Align(buf, 16);
        e.vertex_offset = buf.size();
        for (size_t i = 0; i < md.positions.size(); ++i) {
            Append(buf, &md.positions[i], 12);
            if (e.vertex_flags & kVertexNormal) Append(buf, &md.normals[i], 12);
            if (e.vertex_flags & kVertexUv0) Append(buf, &md.uv0[i], 8);
        }
        Align(buf, 16);
        e.index_offset = buf.size();
        Append(buf, md.indices.data(), md.indices.size() * 4);
    }
    std::memcpy(buf.data() + sizeof(SpcMeshHeader), entries.data(), entries.size() * sizeof(SpcMeshEntry));
    SpcMeshHeader h{};
    std::memcpy(h.magic, kSpcMeshMagic, 4);
    h.version = kSpcMeshVersion;
    h.byte_order = kSpcMeshByteOrder;
    h.header_size = sizeof(SpcMeshHeader);
    h.mesh_count = static_cast<u32>(meshes.size());
    h.table_offset = sizeof(SpcMeshHeader);
    h.entry_size = sizeof(SpcMeshEntry);
    h.file_size = buf.size();
    std::memcpy(h.source_hash, source_hash.data(), 16);
    h.crc32 = Crc32(std::span(buf).subspan(sizeof(SpcMeshHeader)));
    std::memcpy(buf.data(), &h, sizeof(h));
    return buf;
}

bool ReadSpcMesh(std::span<const u8> file, std::vector<MeshData>& out, std::string* error) {
    auto fail = [&](std::string msg) {
        if (error) *error = std::move(msg);
        return false;
    };
    SpcMeshHeader h;
    if (file.size() < sizeof(h)) return fail("file smaller than the header");
    std::memcpy(&h, file.data(), sizeof(h));
    if (std::memcmp(h.magic, kSpcMeshMagic, 4) != 0) return fail("bad magic");
    if (h.byte_order != kSpcMeshByteOrder) return fail("byte order marker mismatch");
    if (h.version != kSpcMeshVersion) return fail(std::format("unsupported version {}", h.version));
    if (h.header_size != sizeof(SpcMeshHeader) || h.entry_size != sizeof(SpcMeshEntry)) {
        return fail("unexpected header/entry size");
    }
    if (h.file_size != file.size()) return fail("file size field does not match the file");
    if (h.mesh_count > (1u << 20)) return fail("mesh count out of range");
    const u64 table_end = static_cast<u64>(h.table_offset) + static_cast<u64>(h.mesh_count) * h.entry_size;
    if (h.table_offset < sizeof(h) || table_end > file.size()) return fail("mesh table outside the file");
    if (Crc32(file.subspan(sizeof(h))) != h.crc32) return fail("CRC mismatch");

    std::vector<MeshData> meshes(h.mesh_count);
    for (u32 m = 0; m < h.mesh_count; ++m) {
        SpcMeshEntry e;
        std::memcpy(&e, file.data() + h.table_offset + static_cast<u64>(m) * h.entry_size, sizeof(e));
        if ((e.vertex_flags & kVertexPosition) == 0 || (e.vertex_flags & ~7u) != 0) {
            return fail(std::format("mesh {}: bad vertex flags", m));
        }
        if (e.vertex_stride != StrideFor(e.vertex_flags)) return fail(std::format("mesh {}: bad stride", m));
        if (e.index_count % 3 != 0) return fail(std::format("mesh {}: index count not a multiple of 3", m));
        const u64 vbytes = static_cast<u64>(e.vertex_count) * e.vertex_stride;
        const u64 ibytes = static_cast<u64>(e.index_count) * 4;
        if (e.vertex_offset > file.size() || vbytes > file.size() - e.vertex_offset) {
            return fail(std::format("mesh {}: vertex data outside the file", m));
        }
        if (e.index_offset > file.size() || ibytes > file.size() - e.index_offset) {
            return fail(std::format("mesh {}: index data outside the file", m));
        }
        MeshData& md = meshes[m];
        std::memcpy(md.id.data(), e.id, 16);
        md.source_offset = e.source_offset;
        md.view_mask = e.view_mask;
        md.bounds_min = {e.bounds_min[0], e.bounds_min[1], e.bounds_min[2]};
        md.bounds_max = {e.bounds_max[0], e.bounds_max[1], e.bounds_max[2]};
        md.positions.resize(e.vertex_count);
        if (e.vertex_flags & kVertexNormal) md.normals.resize(e.vertex_count);
        if (e.vertex_flags & kVertexUv0) md.uv0.resize(e.vertex_count);
        const u8* v = file.data() + e.vertex_offset;
        for (u32 i = 0; i < e.vertex_count; ++i) {
            const u8* p = v + static_cast<u64>(i) * e.vertex_stride;
            std::memcpy(&md.positions[i], p, 12);
            p += 12;
            if (e.vertex_flags & kVertexNormal) {
                std::memcpy(&md.normals[i], p, 12);
                p += 12;
            }
            if (e.vertex_flags & kVertexUv0) std::memcpy(&md.uv0[i], p, 8);
            const Vec3& q = md.positions[i];
            if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z)) {
                return fail(std::format("mesh {}: non-finite position", m));
            }
        }
        md.indices.resize(e.index_count);
        std::memcpy(md.indices.data(), file.data() + e.index_offset, ibytes);
        for (u32 idx : md.indices) {
            if (idx >= e.vertex_count) return fail(std::format("mesh {}: index out of range", m));
        }
    }
    out = std::move(meshes);
    return true;
}

}  // namespace sspc::res
