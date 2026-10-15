// Stencil-art minigame tests on a synthetic definition (no game data).
#include <cmath>
#include <vector>

#include "gameplay/events.h"
#include "gameplay/graffiti.h"
#include "test.h"

using namespace sspc;

namespace {

res::GraffitiDef SyntheticDef() {
    res::GraffitiDef d;
    d.oid = 0xabcdef;
    d.phys_dimensions_cm = {7, 600, 300};
    d.has_camera = true;
    d.camera_pos_cm = {-500, 0, 150};  // 5 m in front of the wall (wall faces -X here)
    d.camera_euler = {0, 0, 0};        // looking +X
    for (int s = 0; s < 2; ++s) {
        res::GraffitiStageDef st;
        st.stencil_geometry = 1;
        st.paint_geometry = 2;
        st.color = s == 0 ? Vec4{1, 1, 1, 0} : Vec4{0, 0, 0, 0};
        st.markers_cm = {{3, -200, 50}, {3, 0, 0}, {3, 150, -80}};
        d.stages.push_back(st);
    }
    return d;
}

WallFrame Wall() {
    WallFrame w;
    w.origin = {0, 0, 1.5f};
    w.rotation = {0, 0, 1, 0};  // 180 degrees about Z: the wall faces -X
    w.half_extent = {3.0f, 1.5f};
    return w;
}

CameraPose Gameplay() { return {{-3, 0, 1.6f}, {1, 0, 0}, 1.047f}; }

struct Run {
    GraffitiMinigame g;
    GameEventBus bus;
    std::vector<GameEvent> events;
    Run() {
        g.Configure(SyntheticDef(), Wall());
        bus.Subscribe(EventType::None, [this](const GameEvent& e) { events.push_back(e); });
    }
    void Tick(const GraffitiInput& in, float dt = 1.0f / 60, int n = 1) {
        for (int i = 0; i < n; ++i) {
            g.Update(dt, in, Gameplay(), bus);
            bus.Dispatch();
        }
    }
    int Count(EventType t) const {
        int c = 0;
        for (const auto& e : events) c += e.type == t ? 1 : 0;
        return c;
    }
    void PaintStage() {
        for (size_t m = 0; m < g.MarkerCount(g.Stage()); ++m) {
            g.SetCursor(g.MarkerUv(g.Stage(), m));
            for (int k = 0; k < 30; ++k) g.SprayAt(g.MarkerUv(g.Stage(), m), 1.0f / 60);
        }
        Tick({});
    }
};

}  // namespace

TEST(graffiti_markers_map_to_wall_uv) {
    Run r;
    // Local y = -200 cm on a 6 m wide wall -> u = (-2 + 3) / 6.
    const Vec2 uv = r.g.MarkerUv(0, 0);
    CHECK(std::abs(uv.x - 1.0f / 6.0f) < 1e-5f);
    CHECK(std::abs(uv.y - (0.5f + 1.5f) / 3.0f) < 1e-5f);
    // The wall point of uv (0.5, 0.5) is the wall centre; the normal faces -X.
    const Vec3 c = r.g.WallPoint({0.5f, 0.5f});
    CHECK(std::abs(c.x) < 1e-5f && std::abs(c.z - 1.5f) < 1e-5f);
    CHECK(std::abs(r.g.WallNormal().x + 1.0f) < 1e-5f);
}

TEST(graffiti_enter_paint_reveal_complete) {
    Run r;
    CHECK(r.g.Phase() == GraffitiPhase::Idle);
    CHECK(r.g.Begin(Gameplay()));
    CHECK(!r.g.Begin(Gameplay()));  // already running
    CHECK(r.g.Phase() == GraffitiPhase::Entering);
    CHECK(r.g.CameraBlend() == 0.0f);
    r.Tick({}, 1.0f / 60, 30);
    CHECK(r.g.CameraBlend() > 0.0f && r.g.CameraBlend() < 1.0f);  // blending into the minigame camera
    r.Tick({}, 1.0f / 60, 40);
    CHECK(r.g.Phase() == GraffitiPhase::Painting);
    CHECK_EQ(r.Count(EventType::GraffitiStarted), 1);
    // Camera reached the definition's pose.
    const CameraPose cam = r.g.Camera(Gameplay());
    CHECK(std::abs(cam.eye.x + 5.0f) < 1e-4f && std::abs(cam.eye.z - 1.5f) < 1e-4f);
    // Spraying away from the markers does not finish the stage.
    GraffitiInput spray;
    spray.spray = true;
    r.g.SetCursor({0.95f, 0.95f});
    r.Tick(spray, 1.0f / 60, 60);
    CHECK(r.g.Phase() == GraffitiPhase::Painting);
    CHECK(r.g.StageProgress() == 0.0f);
    // Cover the markers -> reveal -> stage 2.
    r.PaintStage();
    CHECK(r.g.Phase() == GraffitiPhase::Revealing);
    r.Tick({}, 1.0f / 60, 60);
    CHECK_EQ(r.g.Stage(), 1);
    CHECK(r.g.Phase() == GraffitiPhase::Painting);
    CHECK_EQ(r.Count(EventType::GraffitiStageCompleted), 1);
    // Stage 1 mask is fully revealed after its reveal.
    bool full = true;
    for (u8 v : r.g.Mask(0)) full = full && v == 255;
    CHECK(full);
    r.PaintStage();
    r.Tick({}, 1.0f / 60, 60);  // reveal
    CHECK(r.g.Phase() == GraffitiPhase::Exiting);
    r.Tick({}, 1.0f / 60, 60);
    CHECK(r.g.Phase() == GraffitiPhase::Done);
    CHECK(r.g.Completed());
    CHECK_EQ(r.Count(EventType::GraffitiCompleted), 1);
    // Completed: no new session, no second completion event.
    CHECK(!r.g.Begin(Gameplay()));
    r.Tick({}, 1.0f / 60, 60);
    CHECK_EQ(r.Count(EventType::GraffitiCompleted), 1);
    CHECK(r.g.CameraBlend() == 0.0f);  // back to the gameplay camera
}

TEST(graffiti_cancel_keeps_progress_and_resumes) {
    Run r;
    r.g.Begin(Gameplay());
    r.Tick({}, 1.0f / 60, 70);
    // Cover two of three markers, then leave.
    for (size_t m = 0; m < 2; ++m) {
        for (int k = 0; k < 30; ++k) r.g.SprayAt(r.g.MarkerUv(0, m), 1.0f / 60);
    }
    CHECK(r.g.MarkerCovered(0, 0) && r.g.MarkerCovered(0, 1) && !r.g.MarkerCovered(0, 2));
    GraffitiInput cancel;
    cancel.cancel = true;
    r.Tick(cancel);
    CHECK(r.g.Phase() == GraffitiPhase::Exiting);
    CHECK_EQ(r.Count(EventType::GraffitiAborted), 1);
    r.Tick({}, 1.0f / 60, 60);
    CHECK(r.g.Phase() == GraffitiPhase::Idle);
    CHECK(!r.g.Completed());
    // Interacting again resumes the same stage with the paint kept.
    CHECK(r.g.Begin(Gameplay()));
    r.Tick({}, 1.0f / 60, 70);
    CHECK_EQ(r.g.Stage(), 0);
    CHECK(r.g.MarkerCovered(0, 0) && r.g.MarkerCovered(0, 1));
    CHECK(std::abs(r.g.StageProgress() - 2.0f / 3.0f) < 1e-5f);
}

TEST(graffiti_mouse_and_gamepad_move_the_cursor) {
    Run r;
    r.g.Begin(Gameplay());
    r.Tick({}, 1.0f / 60, 70);
    const Vec2 c0 = r.g.Cursor();
    GraffitiInput mouse;
    mouse.mouse_dx = 100;   // right
    mouse.mouse_dy = -100;  // up
    r.Tick(mouse);
    const Vec2 c1 = r.g.Cursor();
    CHECK(c1.x > c0.x && c1.y > c0.y);
    // Same metres on the wall horizontally and vertically for equal mouse counts.
    const float du_m = (c1.x - c0.x) * 6.0f, dv_m = (c1.y - c0.y) * 3.0f;
    CHECK(std::abs(du_m - dv_m) < 1e-4f);
    GraffitiInput pad;
    pad.stick_x = -1;
    pad.stick_y = -1;
    r.Tick(pad, 0.1f);
    const Vec2 c2 = r.g.Cursor();
    CHECK(c2.x < c1.x && c2.y < c1.y);
    // Clamped to the wall.
    pad.stick_x = 1;
    pad.stick_y = 1;
    r.Tick(pad, 0.5f, 20);
    CHECK(r.g.Cursor().x == 1.0f && r.g.Cursor().y == 1.0f);
    // Spraying through the input path changes the mask revision.
    const u32 rev = r.g.MaskRevision(0);
    GraffitiInput spray;
    spray.spray = true;
    r.Tick(spray);
    CHECK(r.g.MaskRevision(0) != rev);
}

TEST(graffiti_state_round_trip_and_corruption) {
    Run a;
    a.g.Begin(Gameplay());
    a.Tick({}, 1.0f / 60, 70);
    a.PaintStage();
    a.Tick({}, 1.0f / 60, 60);  // stage 1 done
    for (int k = 0; k < 30; ++k) a.g.SprayAt(a.g.MarkerUv(1, 0), 1.0f / 60);
    const std::vector<u8> saved = a.g.Serialize();
    CHECK(saved.size() < 64 * 1024);  // masks are run-length coded

    Run b;
    std::string err;
    CHECK(b.g.Deserialize(saved, &err));
    CHECK_EQ(b.g.Stage(), 1);
    CHECK(b.g.Phase() == GraffitiPhase::Idle);  // a session in progress is saved as stopped
    CHECK(b.g.Mask(0) == a.g.Mask(0));
    CHECK(b.g.Mask(1) == a.g.Mask(1));
    CHECK(b.g.MarkerCovered(1, 0));
    // Finishing after loading still posts exactly one completion.
    b.g.Begin(Gameplay());
    b.Tick({}, 1.0f / 60, 70);
    b.PaintStage();
    b.Tick({}, 1.0f / 60, 130);
    CHECK(b.g.Completed());
    CHECK_EQ(b.Count(EventType::GraffitiCompleted), 1);
    // A completed state loads as completed (no repeated session or event).
    Run c;
    CHECK(c.g.Deserialize(b.g.Serialize(), &err));
    CHECK(c.g.Completed());
    CHECK(!c.g.Begin(Gameplay()));
    // Corruption: truncation, wrong wall id, broken run-length data.
    for (size_t n = 0; n + 1 < saved.size(); n += 97) {
        Run d;
        CHECK(!d.g.Deserialize(std::span<const u8>(saved.data(), n), &err));
    }
    std::vector<u8> wrong = saved;
    wrong[4] ^= 1;  // oid
    Run e;
    CHECK(!e.g.Deserialize(wrong, &err));
    std::vector<u8> zero_run = saved;
    zero_run[saved.size() - 2] = 0;  // a run of length 0
    Run f;
    CHECK(!f.g.Deserialize(zero_run, &err));
    CHECK(f.g.Phase() == GraffitiPhase::Idle && f.g.Stage() == 0);  // unchanged
}

TEST(graffiti_save_during_final_exit_completes_after_load) {
    // Saved in the last exit transition, before the completion event was posted.
    Run a;
    a.g.Begin(Gameplay());
    a.Tick({}, 1.0f / 60, 70);
    a.PaintStage();
    a.Tick({}, 1.0f / 60, 60);  // stage 1 revealed
    a.PaintStage();
    a.Tick({}, 1.0f / 60, 60);  // stage 2 revealed
    CHECK(a.g.Phase() == GraffitiPhase::Exiting);
    CHECK_EQ(a.Count(EventType::GraffitiCompleted), 0);
    const std::vector<u8> saved = a.g.Serialize();

    Run b;
    std::string err;
    CHECK(b.g.Deserialize(saved, &err));
    CHECK(b.g.CameraBlend() == 0.0f);  // already at the gameplay camera
    b.Tick({});
    CHECK(b.g.Completed());
    CHECK_EQ(b.Count(EventType::GraffitiCompleted), 1);
    b.Tick({}, 1.0f / 60, 60);
    CHECK_EQ(b.Count(EventType::GraffitiCompleted), 1);
    CHECK(!b.g.Begin(Gameplay()));
}
