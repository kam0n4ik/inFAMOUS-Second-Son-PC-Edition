// sspc_inspect - KCAP (.xpps) package inspector.
//
//   sspc_inspect info <pkg>                header, segments, block types, service chunks, links
//   sspc_inspect blocks <pkg>              block table
//   sspc_inspect links <pkg> [kind]        link table (location -> target)
//   sspc_inspect geometry <pkg>            geometry records and their vertex streams
//   sspc_inspect dump <pkg> <off> <len>    hex dump of the data region with link annotations
//   sspc_inspect obj <pkg> <outdir>        decode all geometry and write Wavefront OBJ files
//   sspc_inspect scan <dir>                parse every .xpps in a directory, summarise results
//   sspc_inspect near <pkg> x y z r dz     chunk instances within r metres (XY) of a world point
//
// Never crashes on damaged input: every read goes through the validated package.
#include <windows.h>

#include <cstdio>
#include <format>
#include <map>
#include <string>

#include <algorithm>
#include <cmath>

#include "core/file.h"
#include "core/log.h"
#include "resources/geometry.h"
#include "resources/kcap.h"
#include "resources/world_chunk.h"

using namespace sspc;
using namespace sspc::res;

namespace {

void Print(const std::string& s) { std::fwrite(s.data(), 1, s.size(), stdout); }

std::string TagName(u32 tag) {
    switch (tag) {
        case kTagDic: return "DIC";
        case kTagKnli: return "KNLI";
        case kTagKnlx: return "KNLX";
        case kTagPyto: return "PYTO";
        case kTagPlps: return "PLPS";
        case kTagEnd: return "END";
        default: return std::format("{:08x}", tag);
    }
}

int Open(const std::wstring& path, KcapFile& f) {
    const KcapError e = f.Open(path);
    if (e != KcapError::Ok) {
        Print(std::format("error: {}: {}\n", log::Narrow(path), ToString(e)));
        return 2;
    }
    return 0;
}

int CmdInfo(const KcapPackage& p) {
    const auto& h = p.Header();
    std::string hash;
    for (u8 b : h.hash) hash += std::format("{:02x}", b);
    Print(std::format("KCAP version {} kind {} header {:#x} data {:#x} ({:.2f} MiB) hash {}\n", h.version, h.kind,
                      h.header_size, h.data_size, h.data_size / 1048576.0, hash));
    Print(std::format("table region {:#x}+{:#x}; {} segments, {} blocks\n", h.table_offset, h.table_size,
                      h.segment_count, h.block_count));
    for (size_t i = 0; i < p.Segments().size(); ++i) {
        const auto& s = p.Segments()[i];
        Print(std::format("  segment {} pool {} flags {:#04x} offset {:#10x} size {:#10x} blocks [{}..+{}]\n", i, s.pool,
                          s.flags, s.offset, s.size, s.first_block, s.block_count));
    }
    std::map<u16, std::pair<u32, u64>> types;
    for (const auto& b : p.Blocks()) {
        types[b.type].first++;
        types[b.type].second += b.size;
    }
    Print("  block types:");
    for (const auto& [t, cs] : types) Print(std::format(" {}:{}x/{:#x}", t, cs.first, cs.second));
    Print("\n");
    for (const auto& c : p.Chunks()) {
        Print(std::format("  chunk {:4} payload {:#10x} size {:#x}\n", TagName(c.tag), c.payload, c.size));
    }
    u32 kinds[5] = {};
    for (const auto& l : p.Links()) kinds[static_cast<u8>(l.kind)]++;
    Print(std::format("  links {} (pointer {}, texture {}, buffer {}, pointer64 {}, addr256 {}), invalid {}\n",
                      p.Links().size(), kinds[0], kinds[1], kinds[2], kinds[3], kinds[4], p.InvalidLinkCount()));
    return 0;
}

int CmdBlocks(const KcapPackage& p) {
    for (size_t i = 0; i < p.Blocks().size(); ++i) {
        const auto& b = p.Blocks()[i];
        Print(std::format("{:5} type {:3} align {:#6x} offset {:#10x} size {:#10x} segment {}{}\n", i, b.type, b.align,
                          b.offset, b.size, p.SegmentIndexOf(b.offset), b.is_virtual ? " (load-time, not in file)" : ""));
    }
    return 0;
}

int CmdLinks(const KcapPackage& p, int kind) {
    for (const auto& l : p.Links()) {
        if (kind >= 0 && static_cast<int>(l.kind) != kind) continue;
        Print(std::format("{:#10x} -> {:#10x} kind {}\n", l.location, l.target, static_cast<int>(l.kind)));
    }
    return 0;
}

int CmdGeometry(const KcapPackage& p) {
    const auto recs = FindGeometry(p);
    Print(std::format("{} geometry records\n", recs.size()));
    for (const auto& r : recs) {
        Print(std::format("geom {:#x}: {} indices ({} tris), view mask {:#06x}, LOD {}..{}, AABB ({:.3f} {:.3f} {:.3f})..({:.3f} {:.3f} {:.3f})\n",
                          r.offset, r.index_count, r.triangle_count, r.view_mask, r.lod_min, r.lod_max, r.aabb_min.x,
                          r.aabb_min.y, r.aabb_min.z, r.aabb_max.x, r.aabb_max.y, r.aabb_max.z));
        for (size_t i = 0; i < r.streams.size(); ++i) {
            const auto& v = r.streams[i].vsharp;
            Print(std::format("    stream {} buffer {:#x} stride {} records {} format {}/{}\n", i, v.base, v.stride,
                              v.num_records, ToString(v.dfmt), ToString(v.nfmt)));
        }
    }
    return 0;
}

int CmdDump(const KcapPackage& p, u64 off, u64 len) {
    if (!p.InRange(off, 0)) {
        Print("offset outside the data region\n");
        return 1;
    }
    len = std::min<u64>(len, p.Data().size() - off);
    for (u64 o = off; o < off + len; o += 16) {
        std::string line = std::format("{:08x}:", o);
        for (u64 i = 0; i < 16 && o + i < off + len; ++i) line += std::format(" {:02x}", p.Data()[o + i]);
        for (u64 i = 0; i < 16; i += 4) {
            if (const KcapLink* l = p.LinkAt(static_cast<u32>(o + i))) {
                line += std::format("  [+{:x}: kind {} -> {:#x}]", i, static_cast<int>(l->kind), l->target);
            }
        }
        Print(line + "\n");
    }
    return 0;
}

int CmdObj(const KcapPackage& p, const std::wstring& outdir) {
    file::CreateDirectories(outdir);
    int written = 0, failed = 0;
    for (const auto& r : FindGeometry(p)) {
        MeshData m;
        std::string err;
        if (!DecodeGeometry(p, r, m, &err)) {
            Print(err + "\n");
            ++failed;
            continue;
        }
        std::string text;
        for (const auto& v : m.positions) text += std::format("v {} {} {}\n", v.x, v.y, v.z);
        for (const auto& n : m.normals) text += std::format("vn {} {} {}\n", n.x, n.y, n.z);
        for (const auto& t : m.uv0) text += std::format("vt {} {}\n", t.x, 1.0f - t.y);
        for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
            const u32 a = m.indices[i] + 1, b = m.indices[i + 1] + 1, c = m.indices[i + 2] + 1;
            text += std::format("f {} {} {}\n", a, b, c);
        }
        const std::wstring path = outdir + log::Widen(std::format("\\geom_{:06x}.obj", r.offset));
        if (file::WriteAtomic(path, std::span(reinterpret_cast<const u8*>(text.data()), text.size()))) ++written;
    }
    Print(std::format("{} OBJ files written, {} geometry records failed to decode\n", written, failed));
    return failed ? 1 : 0;
}

int CmdScan(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.xpps").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        Print("no .xpps files\n");
        return 1;
    }
    std::map<std::string, u32> results;
    u64 geometry = 0, decoded = 0, decode_failed = 0, links = 0, invalid_links = 0;
    u32 files = 0;
    do {
        ++files;
        KcapFile f;
        const KcapError e = f.Open(dir + L"\\" + fd.cFileName);
        results[ToString(e)]++;
        if (e != KcapError::Ok) {
            Print(std::format("  {}: {}\n", log::Narrow(fd.cFileName), ToString(e)));
            continue;
        }
        links += f.Package().Links().size();
        invalid_links += f.Package().InvalidLinkCount();
        for (const auto& r : FindGeometry(f.Package())) {
            ++geometry;
            MeshData m;
            if (DecodeGeometry(f.Package(), r, m, nullptr)) ++decoded;
            else ++decode_failed;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    Print(std::format("{} packages scanned\n", files));
    for (const auto& [k, v] : results) Print(std::format("  {:40} {}\n", k, v));
    Print(std::format("links {} (invalid {}), geometry records {}, decoded {}, decode failures {}\n", links,
                      invalid_links, geometry, decoded, decode_failed));
    return 0;
}

// Chunk scene instances whose matrix origin lies within `r` metres (XY) of (x, y): count, height range
// and how many sit within `dz` metres of z.
int CmdNear(const KcapPackage& p, float x, float y, float z, float r, float dz) {
    u32 n = 0, near_z = 0, kinds[8] = {};
    float zmin = 1e30f, zmax = -1e30f;
    for (const ChunkScene& sc : FindChunkScenes(p)) {
        for (const ChunkInstance& inst : sc.instances) {
            const Mat4 w = ChunkMatrixToWorld(sc.matrices[inst.matrix]);
            const float dx = w.m[3][0] - x, dy = w.m[3][1] - y;
            if (dx * dx + dy * dy > r * r) continue;
            ++n;
            ++kinds[inst.kind & 7];
            zmin = std::min(zmin, w.m[3][2]);
            zmax = std::max(zmax, w.m[3][2]);
            if (std::abs(w.m[3][2] - z) < dz) ++near_z;
        }
    }
    Print(std::format("{} instances within {} m (kinds 0:{} 2:{} 4:{} 6:{}), z {:.1f}..{:.1f}, {} within {} m of z={}\n", n, r,
                      kinds[0], kinds[2], kinds[4], kinds[6], n ? zmin : 0.0f, n ? zmax : 0.0f, near_z, dz, z));
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        Print("usage: sspc_inspect info|blocks|links|geometry|dump|obj <package> [args] | scan <dir>\n");
        return 1;
    }
    const std::wstring cmd = argv[1];
    if (cmd == L"scan") return CmdScan(argv[2]);
    KcapFile f;
    if (int rc = Open(argv[2], f)) return rc;
    const KcapPackage& p = f.Package();
    if (cmd == L"info") return CmdInfo(p);
    if (cmd == L"blocks") return CmdBlocks(p);
    if (cmd == L"links") return CmdLinks(p, argc > 3 ? _wtoi(argv[3]) : -1);
    if (cmd == L"geometry") return CmdGeometry(p);
    if (cmd == L"dump" && argc > 4) return CmdDump(p, wcstoull(argv[3], nullptr, 0), wcstoull(argv[4], nullptr, 0));
    if (cmd == L"obj" && argc > 3) return CmdObj(p, argv[3]);
    if (cmd == L"near" && argc > 7) {
        return CmdNear(p, static_cast<float>(_wtof(argv[3])), static_cast<float>(_wtof(argv[4])), static_cast<float>(_wtof(argv[5])),
                       static_cast<float>(_wtof(argv[6])), static_cast<float>(_wtof(argv[7])));
    }
    Print("unknown command\n");
    return 1;
}
