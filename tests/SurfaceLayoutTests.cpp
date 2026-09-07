#include "TestSupport.h"
#include "graph/SurfaceLayout.h"
#include "io/SurfaceLayoutIo.h"
#include "app/UndoHistory.h"
#include <nlohmann/json.hpp>
#include <limits>

void RunSurfaceLayoutTests() {
    using namespace tg;
    using tests::Check;
    tests::Section("配置記述 — 保存・参照・独立区間・Undo");
    graph::SurfaceLayoutDocument document;
    std::array<graph::SurfaceId, 6> presets{};
    const char* names[] = {"新舗装", "荒れた舗装", "砂利道", "砂利路肩", "草地", "歩道"};
    for (size_t i = 0; i < presets.size(); ++i) {
        graph::SurfacePreset p;
        p.id = document.AllocateId();
        presets[i] = p.id;
        p.name = names[i];
        p.role = i < 3 ? graph::SurfaceRole::Road : i == 5 ? graph::SurfaceRole::Sidewalk : graph::SurfaceRole::Ground;
        p.section = {{document.AllocateId(), 0, 0}, {document.AllocateId(), 2, i == 5 ? 0.15f : 0.0f}};
        p.materials.push_back({static_cast<uint32_t>(i + 1)});
        p.displacementMeters = 0.01f * static_cast<float>(i);
        p.parameters.push_back({document.AllocateId(), "荒れ具合", 0, 1, 0.5f});
        p.boundaries[0].mode = i == 5 ? graph::BoundaryMode::KeepStep : graph::BoundaryMode::Blend;
        p.boundaries[0].preserveOutline = i == 5;
        document.presets.push_back(p);
    }
    graph::RoadLayout layout;
    layout.id = document.AllocateId(); layout.roadNode = 10;
    for (size_t side = 0; side < 2; ++side) {
        graph::SurfaceBand band;
        band.id = document.AllocateId();
        band.side = side == 0 ? graph::SurfaceSide::Road : graph::SurfaceSide::Left;
        const float cuts[2][4] = {{0, 12, 28, 50}, {0, 17, 35, 50}};
        for (size_t i = 0; i < 3; ++i) {
            graph::SurfaceSpan span;
            span.id = document.AllocateId(); span.preset = presets[side * 3 + i];
            span.startMeters = cuts[side][i]; span.endMeters = cuts[side][i+1];
            span.blendInMeters = 2; span.blendOutMeters = 3; span.seed = 17;
            span.parameters.push_back({document.presets[side * 3 + i].parameters[0].id, 0.2f, 0.8f});
            band.spans.push_back(span);
        }
        layout.bands.push_back(band);
    }
    document.layouts.push_back(layout);
    std::string error;
    Check(graph::ValidateSurfaceLayouts(document, error), "道路3種・沿道3種の独立した区間列を保持できる");
    graph::NodeGraph sceneGraph;
    Check(!graph::ValidateSurfaceLayoutRoads(document, sceneGraph, error), "存在しないRoad参照を保存・読込前に拒否する");
    auto linked = document;
    linked.layouts[0].roadNode = sceneGraph.CreateNode(graph::NodeKind::Road);
    Check(graph::ValidateSurfaceLayoutRoads(linked, sceneGraph, error), "配置は明示したRoadにだけ接続する");
    const auto encoded = io::WriteSurfaceLayouts(document);
    graph::SurfaceLayoutDocument decoded;
    Check(io::ReadSurfaceLayouts(encoded, decoded, error) && io::WriteSurfaceLayouts(decoded) == encoded,
          "安定ID・断面・境界条件・材質・公開値の始終端が完全に往復する");
    auto assertRejected = [&](nlohmann::json broken) {
        Check(!io::ReadSurfaceLayouts(broken, decoded, error) && !error.empty() && io::WriteSurfaceLayouts(decoded) == encoded,
              "不正な配置を理由付きで拒否し、既存データを保持する");
    };
    auto broken = encoded;
    broken["layouts"][0]["bands"][0]["spans"][0]["preset"] = 99999;
    assertRejected(broken);
    broken = encoded; broken["presets"][1]["id"] = document.presets[0].id; assertRejected(broken);
    broken = encoded; broken["layouts"][0]["bands"][0]["spans"][1]["start"] = 4; assertRejected(broken);
    broken = encoded; broken["layouts"][0]["bands"][0]["spans"][0]["parameters"][0]["end"] = 2; assertRejected(broken);
    broken = encoded; broken["nextId"] = -1; assertRejected(broken);
    broken = encoded; broken["nextId"] = uint64_t(1) << 40; assertRejected(broken);
    broken = encoded; broken["version"] = 2; assertRejected(broken);
    broken = encoded; broken["presets"][0]["section"] = "invalid"; assertRejected(broken);
    broken = encoded; broken["presets"][0]["boundaries"][0]["mode"] = 99; assertRejected(broken);
    broken = encoded; broken["presets"][0].erase("materials"); assertRejected(broken);
    auto invalid = document;
    invalid.layouts[0].bands[0].spans[0].endMeters = std::numeric_limits<float>::quiet_NaN();
    Check(!graph::ValidateSurfaceLayouts(invalid, error), "非有限の区間を保存前に拒否する");
    invalid.nextId = std::numeric_limits<graph::SurfaceId>::max();
    Check(invalid.AllocateId() == 0 && invalid.nextId == std::numeric_limits<graph::SurfaceId>::max(), "ID枯渇時に既存IDを再利用しない");

    DocumentSnapshot before;
    before.surfaceLayouts = document;
    DocumentSnapshot after = before;
    after.surfaceLayouts.layouts[0].bands[0].spans[0].parameters[0].endValue = 0.4f;
    after.surfaceLayouts.presets[5].section.back().height = 0.2f;
    UndoHistory history;
    history.Push(before, 0);
    const auto undone = history.Undo(after);
    Check(io::WriteSurfaceLayouts(undone.surfaceLayouts) == encoded, "区間と断面の編集をIDごとUndoする");
    const auto redone = history.Redo(undone);
    Check(io::WriteSurfaceLayouts(redone.surfaceLayouts) == io::WriteSurfaceLayouts(after.surfaceLayouts), "Redoで公開値と断面を復元する");
}
