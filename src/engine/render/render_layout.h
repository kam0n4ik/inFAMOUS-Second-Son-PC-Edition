// SecondSonPC — the layout of the engine's render objects as reconstructed from the eboot (P5.2B/P5.3A;
// docs/native-reconstruction/RENDER_ENGINE_ARCHITECTURE.md). Offsets are the game's; names are SecondSonPC's. Every
// address is module-relative (eboot base + value). Code that reads these objects pins the functions that define them
// by hash (guest_hooks.h), so another game version never reaches it.
#pragma once

#include "base/abi.h"

namespace SecondSon::Engine::Render::Layout {

// --- Frame object (DAT_00f1bcd0; SubmitFrame 0x1f1e30, AcquireContext 0x1f2c60) ------------------------------------
inline constexpr u64 kFrame = 0xf1bcd0;
inline constexpr u64 kFrameVideoOut = 0x14;      ///< s32: the VideoOut handle SubmitFrame flips on
inline constexpr u64 kFrameDisplayPair = 0x20;   ///< s32: display buffers 2n, 2n+1 (the flip's buffer: n * 2 + half)
inline constexpr u64 kFrameArenaPairs = 0x28;    ///< 5 x {base[2], current, end, size u32, half u32}, stride 0x30
inline constexpr u64 kFrameArenaStride = 0x30;
inline constexpr u64 kFrameLabelIndex = 0x124;   ///< s32: the frame's label in kLabelArray
inline constexpr u64 kFrameCounter = 0x128;      ///< u64: fence value written by the frame's EOP
inline constexpr u64 kFramePrevFence = 0x130;    ///< u64: the last submitted frame's fence (SubmitFrame waits for it)
inline constexpr u64 kFrameDisplayHalf = 0x17dc; ///< s32 0/1: the half of kFrameDisplayPair, toggled after each flip
inline constexpr u64 kFramePreContext = 0x1f80;  ///< context R recorded on the main thread before the pool
inline constexpr u64 kFramePostContext = 0xfe10; ///< context R after the pool
inline constexpr u64 kFrameCurrent = 0x1dca0;    ///< R*: the main thread's current context
inline constexpr u64 kFramePool = 0x1dcc0;       ///< 254 pool slots
inline constexpr u64 kPoolSlotStride = 0xdef0;
inline constexpr u32 kPoolSlots = 254;
inline constexpr u64 kFrameAcquired = 0xdf0ef0; ///< s32: slots acquired this frame (atomic)
inline constexpr u64 kFrameRecorded = 0xdf0ef4; ///< s32: slots recorded this frame (atomic)

// --- GPU labels and the async compute queue (P5.4; SubmitFrame 0x1f1e30, compute queue 0x168b70 / 0x1691e0 / 0x1695e0)
inline constexpr u64 kLabelArray = 0xe17e80;   ///< u64*: the engine's labels, 8 bytes each, the value in the low 32 bits
inline constexpr u64 kComputeQueue = 0x7d3cf10; ///< the particles' async compute queue object (main loop 0x29c8d0)
inline constexpr u64 kQueueSubmitted = 0x1b88;  ///< u32: the value its last submission's RELEASE_MEM writes
inline constexpr u64 kQueueLabelIndex = 0x1b8c; ///< s32: its label in kLabelArray
inline constexpr u64 kSubmitMode = 0x1d20388;    ///< s32: where SubmitFrame calls sceGnmSubmitDone (0: before its wait)
inline constexpr u64 kSubmitFlipArg = 0x1d20384; ///< s32: the flip argument SubmitFrame passes

// --- Render context R (0xDE90 bytes, vtable 0xcd14f8; Init 0x11efc0, Reset 0x11f050) --------------------------------
inline constexpr u64 kContextSize = 0xde90;
inline constexpr u64 kContextGfx = 0x8; ///< G: Gnmx-like graphics context; its draw command buffer (DCB) at G+0
inline constexpr u64 kGfxCcb = 0x80;    ///< constant command buffer (BaseGfxContext::init 0x4c80); never used (P5.3A)
inline constexpr u64 kCbBegin = 0x0;          ///< u32*: start of the current chunk
inline constexpr u64 kCbEnd = 0x8;            ///< u32*: end of the reserved space
inline constexpr u64 kCbCmdPtr = 0x10;        ///< u32*: write pointer
inline constexpr u64 kContextCue = 0x128;     ///< constant update engine (= G + 0x120)
inline constexpr u64 kContextChunks = 0xcc70; ///< 64 x {u32* start, u32 dwords, pad}: finished chunks of the DCB
inline constexpr u64 kChunkStride = 0x10;
inline constexpr u32 kMaxChunks = 64;
inline constexpr u64 kContextChunkCount = 0xd070; ///< u32
inline constexpr u64 kContextStateBlock = 0xde48; ///< 0x38 bytes: render state + render target set (+0x20)
inline constexpr u64 kContextColorMask = 0xde80;

// --- Pool slot = context R + header (AcquireContext 0x1f2c60/0x1f2c80/0x1f2d00) -------------------------------------
inline constexpr u64 kSlotPass = 0xde90;     ///< s32 pass 0..40; sort key, first part
inline constexpr u64 kSlotSub = 0xde94;      ///< u32 chunk index / sub key (900, 1900: extra contexts of pass 12)
inline constexpr u64 kSlotRecordFn = 0xde98; ///< record function the job calls (0x1ef5c0)
inline constexpr u64 kSlotRecordArg = 0xdea0;
inline constexpr u64 kSlotIsJob = 0xdea8;      ///< u8: recorded by a job of the frame's batch
inline constexpr u64 kSlotInitialState = 0xdeb0; ///< 0x38 bytes: state block the context starts from

// --- Deformation system (DAT_00e18350): up to 5 contexts R submitted before the pre-context -------------------------
inline constexpr u64 kDeformContexts = 0xe18c80;
inline constexpr u32 kDeformContextCount = 5;

// --- Constant update engine (CUE = R + 0x128) ----------------------------------------------------------------------
inline constexpr u64 kCueShadow = 0x90; ///< 8 stage shadows of 0x1800 bytes
inline constexpr u64 kCueShadowStride = 0x1800;
inline constexpr u64 kCueDcb = 0xc090;    ///< Gnm::CommandBuffer* (= G)
inline constexpr u64 kCueLayout = 0xc180; ///< 8 x user-data layout table pointer

// --- Pass table (0xcd79d0: 41 x {setup, begin, end}) --------------------------------------------------------------
inline constexpr u64 kPassTable = 0xcd79d0;
inline constexpr u32 kPassCount = 41;

// --- Functions ----------------------------------------------------------------------------------------------------
inline constexpr u64 kSubmitFrame = 0x1f1e30;   ///< (Frame*, u32 flags): sorts the contexts, EOP, submit + flip
inline constexpr u64 kContextReset = 0x11f050;  ///< (R*): empties the DCB and the constant caches
inline constexpr u64 kDcbReserve = 0x11eed0;    ///< (cb, dwords, R*): the DCB's out-of-space callback
inline constexpr u64 kSetTextures = 0x16d10;    ///< (CUE*, stage, slot, count, T#[count] of 32 bytes)
inline constexpr u64 kSetRwTextures = 0x16de0;  ///< (CUE*, stage, slot, count, T#[count] of 32 bytes)
inline constexpr u64 kSetBuffers = 0x16b70;     ///< (CUE*, stage, slot, count, V#[count] of 16 bytes)
inline constexpr u64 kSetRwBuffers = 0x16c40;   ///< (CUE*, stage, slot, count, V#[count] of 16 bytes)

} // namespace SecondSon::Engine::Render::Layout
