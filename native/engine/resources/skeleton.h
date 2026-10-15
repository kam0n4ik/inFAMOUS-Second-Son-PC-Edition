// SecondSonPC native runtime - character skeleton from a KCAP package.
//
// Confirmed on hero.xpps (653 joints, docs/native/RESOURCE_FORMATS.md "Skeleton"):
//   bind pose      N x 64 bytes: 4x4 row-major (row-vector convention), translation in row 3, cm
//   inverse bind   N x 48 bytes: 3x4 rows, translation in the 4th column, cm (stored right after)
//   parents        N x int16, -1 for the root, parent index < joint index
// The owning object stores the two matrix pointers next to each other; the parent array pointer
// is within a few fields of them. Skin weights: joint[0] gets 255 - (w1 + w2 + w3).
#pragma once

#include <optional>
#include <vector>

#include "core/math.h"
#include "core/types.h"
#include "resources/anim_clip.h"
#include "resources/kcap.h"

namespace sspc::res {

struct Skeleton {
    u32 bind_offset = 0;
    std::vector<s16> parents;
    std::vector<Mat4> bind;          // model space, metres, column-vector convention
    std::vector<Mat4> inverse_bind;  // as stored (also metres); inverse_bind[i] * bind[i] == I
    // Local rest pose of the '60SE' skeleton object (centimetres), empty if not found. Composed with
    // `parents` it reproduces `bind`; animation clips overwrite the joints they have tracks for.
    std::vector<JointPose> rest;
    u32 rest_offset = 0;
    size_t JointCount() const { return parents.size(); }
};

// '60SE' skeleton object with `joints` joints: {magic, u32 size, u32, u32, u16 joints, ..., +0x18
// self-relative offset of the local pose: joints x {quat xyzw, translation xyz 1, scale xyz 1}}.
std::optional<std::vector<JointPose>> FindRestPose(const KcapPackage& pkg, u32 joints, u32* offset = nullptr);

// The largest skeleton found in the package (characters have one), if any.
std::optional<Skeleton> FindSkeleton(const KcapPackage& pkg);

// Local transform of every joint relative to its parent in the bind pose.
std::vector<Mat4> BindLocals(const Skeleton& s);
// Model-space transforms from local transforms (parents before children).
void ComposeModelSpace(const Skeleton& s, const std::vector<Mat4>& locals, std::vector<Mat4>& model);

Mat4 InverseAffine(const Mat4& m);

}  // namespace sspc::res
