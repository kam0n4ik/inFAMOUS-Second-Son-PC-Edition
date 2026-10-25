// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>

//
// SecondSonPC — R11 neon (docs/R11_NEON.md): the async compute lane's diagnostics in Neon Dash (SSPC_R11_DIAG_AQ=1).
#include "runtime/gpu/vulkan/vk_executor.h"

#include <algorithm>
#include <vector>

#include <fmt/format.h>

#include "base/log.h"
#include "runtime/gpu/critical_compute.h"
#include "runtime/gpu/shader/shader_frontend.h"

namespace SecondSon::Gpu::Vk {

namespace {
bool RangesOverlap(const std::vector<std::pair<u64, u64>>& a, const std::vector<std::pair<u64, u64>>& b) {
    for (const auto& [ab, ae] : a) {
        for (const auto& [bb, be] : b) {
            if (ab < be && bb < ae) {
                return true;
            }
        }
    }
    return false;
}

std::string GdsUserName(u64 key) {
    if ((key >> 62) == 1) {
        return fmt::format("DMA q{} kind {} gds+{:#x}", (key >> 48) & 0x3FFF, (key >> 32) & 0xFFFF, key & 0xFFFF'FFFFull);
    }
    if ((key >> 63) != 0) {
        return fmt::format("draw vs {:#x}", key & ~(1ull << 63));
    }
    return fmt::format("cs {:#x}", key);
}
} // namespace

void Executor::R11DiagInit() {
    char v[4] = {};
    if (GetEnvironmentVariableA("SSPC_R11_DIAG_AQ", v, sizeof(v)) == 1 && v[0] == '1') {
        r11_diag_ = std::make_unique<R11AqDiag>();
        SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_R11_DIAG_AQ=1 - where compute is recorded (lane / main and why), its "
                               "GDS/LDS/images, why batches split and wait (gpu_native.txt)");
    }
}

void Executor::R11DiagDispatch(const GS::StageView& cs, u32 queue, bool indirect, bool main_only, u64 splits_before) {
    R11AqDiag::Prog& p = r11_diag_->progs[cs.address];
    ++p.n;
    const bool waiting = CriticalCompute::Waiting();
    u8 place = 0;
    if (aq_family_ && cur_.pool_lane != 0) {
        place = 0;
    } else if (main_only) {
        place = 1;
    } else if (cur_.aq_main) {
        place = 6;
    } else if (cur_.work[Cp::kGraphicsQueue] != 0) {
        place = waiting ? 2 : 3;
    } else {
        place = !aq_family_ ? 0 : waiting ? 5 : 4; // (the main family's lane: decided at the batch's end)
    }
    ++p.place[place];
    bool gds = false, lds = false, unknown = false;
    for (const auto& b : cs.plan.buffers) {
        gds = gds || b.kind == GS::BufferKind::GdsBuffer;
        lds = lds || b.kind == GS::BufferKind::SharedMemory;
        unknown = unknown || (b.kind != GS::BufferKind::Guest && b.kind != GS::BufferKind::Flatbuf &&
                              b.kind != GS::BufferKind::ClipPlanes && b.kind != GS::BufferKind::GdsBuffer &&
                              b.kind != GS::BufferKind::SharedMemory);
    }
    bool storage = false;
    for (const auto& im : cs.plan.images) {
        storage = storage || im.storage;
    }
    p.gds += gds ? 1 : 0;
    p.lds += lds ? 1 : 0;
    p.unknown += unknown ? 1 : 0;
    p.images += cs.plan.images.empty() ? 0 : 1;
    p.storage += storage ? 1 : 0;
    p.indirect += indirect ? 1 : 0;
    if (stats_.aq_splits != splits_before) {
        ++(main_only ? p.split_main : p.split_lane);
    }
    p.queue_mask |= queue < 32 ? 1u << queue : 0u;
}

void Executor::R11DiagGds(u64 key) {
    if ((key >> 62) == 1) {
        ++r11_diag_->gds_dma[key];
    }
    if (open_ && cur_.r11_gds.size() < 16 && std::find(cur_.r11_gds.begin(), cur_.r11_gds.end(), key) == cur_.r11_gds.end()) {
        cur_.r11_gds.push_back(key);
    }
}

void Executor::R11DiagLane(u64 main_done) {
    auto& w = r11_diag_->lane_why;
    if (cur_.aq_wait_main_min > main_done) {
        ++w[0];
        return;
    }
    if (cur_.aq_wait_all || cur_.aq_images || cur_.aq_overflow) {
        ++w[cur_.aq_wait_all ? 1 : cur_.aq_images ? 2 : 3];
        return;
    }
    bool range = false, ovf_ranges = false, ovf_unknown = false, gds = false;
    std::vector<std::pair<u64, u64>> pairs;
    for (const Batch& b : in_flight_) {
        if (b.lane != 0 || b.done || b.value <= main_done) {
            continue;
        }
        if (b.aq_overflow) {
            ((b.r11_ovf & 1) != 0 ? ovf_ranges : ovf_unknown) = true;
            continue;
        }
        range = range || RangesOverlap(cur_.aq_ranges, b.aq_ranges) || (b.aq_lds & cur_.aq_lds) != 0;
        if (b.aq_gds && cur_.aq_gds) {
            gds = true;
            for (const u64 lu : cur_.r11_gds) {
                for (const u64 mu : b.r11_gds) {
                    pairs.emplace_back(lu, mu);
                }
            }
        }
    }
    ++w[range ? 7 : ovf_ranges ? 4 : ovf_unknown ? 5 : gds ? 6 : 8];
    if (!range && !ovf_ranges && !ovf_unknown && gds) {
        std::sort(pairs.begin(), pairs.end());
        pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
        for (const auto& [lu, mu] : pairs) {
            const u64 k = (lu * 0x9E3779B97F4A7C15ull) ^ mu;
            ++r11_diag_->gds_pairs[k];
            r11_diag_->gds_pair_users[k] = {lu, mu};
        }
    }
}

void Executor::R11DiagMain(u64 lane_done) {
    bool same = true;
    for (const u32 q : cur_.queues) {
        const u64 v = r10_q_lane_[q];
        if (v <= lane_done) {
            continue;
        }
        const auto b = std::find_if(in_flight_.begin(), in_flight_.end(), [&](const Batch& x) { return x.value == v; });
        if (b == in_flight_.end()) {
            continue;
        }
        const auto el = std::find_if(b->r11_epoch.begin(), b->r11_epoch.end(), [&](const auto& e) { return e.first == q; });
        const auto em = std::find_if(cur_.r11_epoch.begin(), cur_.r11_epoch.end(), [&](const auto& e) { return e.first == q; });
        if (el == b->r11_epoch.end() || em == cur_.r11_epoch.end() || el->second != em->second) {
            same = false;
        }
    }
    ++r11_diag_->order[same ? 0 : 1];
}

std::string Executor::R11DiagReport() const {
    std::string out;
    if (r11_aq_images_) {
        out += fmt::format("R11 lane dispatches with textures (SSPC_R11_AQ_IMAGES): pre-check passed {}, refused {}; on the "
                           "lane {}, the cache changed meanwhile (the images rule) {}; main batches that waited for them "
                           "{}\n",
                           r11_img_stats_[0], r11_img_stats_[1], r11_img_stats_[3], r11_img_stats_[4], r11_img_stats_[2]);
        out += fmt::format("  refused: a binding kind {}, no memo {}, memo stale {}, image state {}, layout/access {}, no "
                           "main barrier {}\n",
                           r11_img_why_[0], r11_img_why_[1], r11_img_why_[2], r11_img_why_[3], r11_img_why_[4],
                           r11_img_why_[5]);
    }
    if (r11_order_epoch_) {
        out += fmt::format("R11 order waits by the guest epoch (SSPC_R11_AQ_ORDER_EPOCH): skipped {}, kept (another epoch) "
                           "{}\n",
                           r11_order_skipped_, r11_order_kept_);
    }
    if (r11_diag_ == nullptr) {
        return out;
    }
    const R11AqDiag& d = *r11_diag_;
    out += "R11 AQ diagnostics (SSPC_R11_DIAG_AQ=1):\n";
    std::array<u64, 7> place{};
    u64 n = 0, split_lane = 0, split_main = 0;
    std::vector<std::pair<u64, const R11AqDiag::Prog*>> progs;
    for (const auto& [a, p] : d.progs) {
        n += p.n;
        for (size_t i = 0; i < place.size(); ++i) {
            place[i] += p.place[i];
        }
        split_lane += p.split_lane;
        split_main += p.split_main;
        progs.emplace_back(a, &p);
    }
    out += fmt::format("  dispatches {} ({} programs): lane {}; main - binding images {}, in a graphics batch while the "
                       "game waits {} / not waiting {}, compute-only main batch not waiting {} / waiting {}, kept "
                       "(aq_main) {}\n",
                       n, progs.size(), place[0], place[1], place[2], place[3], place[4], place[5], place[6]);
    out += fmt::format("  splits: by a draw {}, by a dispatch to the lane {}, by a dispatch with images to the main queue "
                       "{} (all splits {})\n", d.draw_splits, split_lane, split_main, stats_.aq_splits);
    out += fmt::format("  lane batches: after a held fence's main batch {}, own wait-all {}, own images {}, own unknown "
                       "{}; a main batch in flight with overflow (ranges) {}, overflow (unknown access) {}, only GDS "
                       "(both) {}, a range conflict {}; free {}\n",
                       d.lane_why[0], d.lane_why[1], d.lane_why[2], d.lane_why[3], d.lane_why[4], d.lane_why[5],
                       d.lane_why[6], d.lane_why[7], d.lane_why[8]);
    out += fmt::format("  main batches with aq_overflow: too many ranges {}, an access of unknown extent {}\n",
                       d.overflow_batches[0], d.overflow_batches[1]);
    out += fmt::format("  MAIN_FREE queue-order waits: no guest sync of the queue between (same epoch) {}, a sync "
                       "between {}; guest waits met by an unknown fence {}\n",
                       d.order[0], d.order[1], d.unknown_fence_waits);
    std::sort(progs.begin(), progs.end(), [](const auto& a, const auto& b) { return a.second->n > b.second->n; });
    out += "  programs (by dispatches): cs n | lane img gfxW gfx cmp cmpW kept | gds lds unk images storage indirect | "
           "splits->lane splits->main | queues\n";
    u32 shown = 0;
    for (const auto& [a, pp] : progs) {
        const R11AqDiag::Prog& p = *pp;
        if (shown++ >= 60 && p.gds == 0) {
            continue; // (every GDS user listed)
        }
        out += fmt::format("    cs {:#x} {} | {} {} {} {} {} {} {} | {} {} {} {} {} {} | {} {} | {:#x}\n", a, p.n,
                           p.place[0], p.place[1], p.place[2], p.place[3], p.place[4], p.place[5], p.place[6], p.gds,
                           p.lds, p.unknown, p.images, p.storage, p.indirect, p.split_lane, p.split_main, p.queue_mask);
    }
    std::vector<std::pair<u64, u64>> dma(d.gds_dma.begin(), d.gds_dma.end());
    std::sort(dma.begin(), dma.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < dma.size() && i < 40; ++i) {
        out += fmt::format("  GDS {}: {}\n", GdsUserName(dma[i].first), dma[i].second);
    }
    std::vector<std::pair<u64, u64>> pairs(d.gds_pairs.begin(), d.gds_pairs.end());
    std::sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < pairs.size() && i < 40; ++i) {
        const auto& u = d.gds_pair_users.at(pairs[i].first);
        out += fmt::format("  GDS-only lane wait: lane {} / main {}: {}\n", GdsUserName(u.first), GdsUserName(u.second),
                           pairs[i].second);
    }
    return out;
}

} // namespace SecondSon::Gpu::Vk
