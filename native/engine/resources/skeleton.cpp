#include "resources/skeleton.h"

#include <cmath>
#include <cstring>

namespace sspc::res {
namespace {

constexpr u32 kMaxJoints = 4096;

bool ReadParents(const KcapPackage& pkg, u32 offset, u32 n, std::vector<s16>& out) {
    if (!pkg.InRange(offset, static_cast<u64>(n) * 2)) return false;
    out.resize(n);
    std::memcpy(out.data(), pkg.Data().data() + offset, static_cast<size_t>(n) * 2);
    if (out[0] != -1) return false;
    for (u32 i = 1; i < n; ++i) {
        if (out[i] < -1 || out[i] >= static_cast<s32>(i)) return false;
    }
    return true;
}

}  // namespace

Mat4 InverseAffine(const Mat4& m) {
    // Inverse of [A t; 0 1] = [A^-1, -A^-1 t] for a general (non-orthonormal) 3x3 A.
    const float a00 = m.m[0][0], a01 = m.m[1][0], a02 = m.m[2][0];
    const float a10 = m.m[0][1], a11 = m.m[1][1], a12 = m.m[2][1];
    const float a20 = m.m[0][2], a21 = m.m[1][2], a22 = m.m[2][2];
    const float det = a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) + a02 * (a10 * a21 - a11 * a20);
    Mat4 r = Mat4::Identity();
    if (std::abs(det) < 1e-20f) return r;
    const float inv = 1.0f / det;
    const float b[3][3] = {{(a11 * a22 - a12 * a21) * inv, (a02 * a21 - a01 * a22) * inv, (a01 * a12 - a02 * a11) * inv},
                           {(a12 * a20 - a10 * a22) * inv, (a00 * a22 - a02 * a20) * inv, (a02 * a10 - a00 * a12) * inv},
                           {(a10 * a21 - a11 * a20) * inv, (a01 * a20 - a00 * a21) * inv, (a00 * a11 - a01 * a10) * inv}};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) r.m[col][row] = b[row][col];
    const float tx = m.m[3][0], ty = m.m[3][1], tz = m.m[3][2];
    for (int row = 0; row < 3; ++row) r.m[3][row] = -(b[row][0] * tx + b[row][1] * ty + b[row][2] * tz);
    return r;
}

std::optional<Skeleton> FindSkeleton(const KcapPackage& pkg) {
    std::optional<Skeleton> best;
    for (const KcapLink& l : pkg.Links()) {
        if (l.kind != LinkKind::Pointer && l.kind != LinkKind::Pointer64) continue;
        const auto next = pkg.PointerAt(l.location + 8);
        if (!next || *next <= l.target || (*next - l.target) % 64 != 0) continue;
        const u32 n = (*next - l.target) / 64;
        if (n < 2 || n > kMaxJoints || (best && best->JointCount() >= n)) continue;
        if (!pkg.InRange(l.target, static_cast<u64>(n) * 64) || !pkg.InRange(*next, static_cast<u64>(n) * 48)) continue;
        // Bind matrices: affine, row-vector convention (last column 0 0 0 1).
        const float* bind = reinterpret_cast<const float*>(pkg.Data().data() + l.target);
        bool ok = true;
        for (u32 j = 0; j < n && ok; ++j) {
            const float* r = bind + j * 16;
            ok = std::abs(r[3]) < 1e-4f && std::abs(r[7]) < 1e-4f && std::abs(r[11]) < 1e-4f && std::abs(r[15] - 1) < 1e-4f;
            for (int k = 0; k < 16 && ok; ++k) ok = std::isfinite(r[k]);
        }
        if (!ok) continue;
        // Parent array: a pointer field near the matrix pointers.
        std::vector<s16> parents;
        bool found = false;
        for (s32 d = -0x80; d <= 0x80 && !found; d += 8) {
            const auto p = pkg.PointerAt(static_cast<u32>(static_cast<s64>(l.location) + d));
            found = p && ReadParents(pkg, *p, n, parents);
        }
        if (!found) continue;
        Skeleton s;
        s.bind_offset = l.target;
        s.parents = std::move(parents);
        s.bind.resize(n);
        s.inverse_bind.resize(n);
        const float* inv = reinterpret_cast<const float*>(pkg.Data().data() + *next);
        for (u32 j = 0; j < n; ++j) {
            std::memcpy(s.bind[j].m, bind + j * 16, 64);  // row-vector row-major == column-vector column-major
            for (int k = 0; k < 3; ++k) s.bind[j].m[3][k] *= 0.01f;
            Mat4 ib = Mat4::Identity();
            const float* r = inv + j * 12;
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) ib.m[col][row] = r[row * 4 + col];
                ib.m[3][row] = r[row * 4 + 3] * 0.01f;
            }
            s.inverse_bind[j] = ib;
        }
        best = std::move(s);
    }
    return best;
}

std::vector<Mat4> BindLocals(const Skeleton& s) {
    std::vector<Mat4> locals(s.JointCount());
    for (size_t j = 0; j < s.JointCount(); ++j) {
        locals[j] = s.parents[j] < 0 ? s.bind[j] : InverseAffine(s.bind[static_cast<size_t>(s.parents[j])]) * s.bind[j];
    }
    return locals;
}

void ComposeModelSpace(const Skeleton& s, const std::vector<Mat4>& locals, std::vector<Mat4>& model) {
    model.resize(s.JointCount());
    for (size_t j = 0; j < s.JointCount(); ++j) {
        model[j] = s.parents[j] < 0 ? locals[j] : model[static_cast<size_t>(s.parents[j])] * locals[j];
    }
}

}  // namespace sspc::res
