#include "TestSupport.h"
#include "graph/SurfaceBandGeometry.h"
#include "graph/SurfaceLayout.h"
#include "graph/SurfaceLayoutEditing.h"
#include "graph/SurfaceLayoutEvaluation.h"
#include "graph/RoadMask.h"
#include "io/SurfaceLayoutIo.h"
#include "app/UndoHistory.h"
#include <nlohmann/json.hpp>
#include <limits>
#include <cmath>

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
    broken = encoded; broken["version"] = 99; assertRejected(broken);
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

    const auto& roadBand = document.layouts[0].bands[0];
    const auto midpoint = graph::SampleSurfaceBand(document, roadBand, 6);
    Check(midpoint.size() == 1 && midpoint[0].preset == presets[0] && std::abs(midpoint[0].parameters[0].value - 0.5f) < 1e-6f,
          "実距離から区間の公開値を補間する");
    const auto blend = graph::SampleSurfaceBand(document, roadBand, 11.5f);
    Check(blend.size() == 2 && std::abs(blend[0].weight - 0.5f) < 1e-6f && blend[1].weight == blend[0].weight,
          "前区間の終端3mと次区間の始端2mで連続移行する");
    const auto side = graph::SampleSurfaceBand(document, document.layouts[0].bands[1], 11.5f);
    Check(side.size() == 1 && side[0].preset == presets[3], "道路の切り替えは沿道の区切りへ影響しない");
    auto shortBand = roadBand;
    shortBand.spans[0].endMeters = shortBand.spans[1].startMeters = 1;
    shortBand.spans[1].endMeters = shortBand.spans[2].startMeters = 2;
    bool partition = true;
    for (int step = 0; step <= 5000; ++step) {
        const auto samples = graph::SampleSurfaceBand(document, shortBand, static_cast<float>(step) / 100);
        float sum = 0;
        for (const auto& sample : samples) { sum += sample.weight; partition &= sample.weight >= 0 && sample.weight <= 1; }
        partition &= samples.size() <= 2 && std::abs(sum - 1) < 1e-6f;
    }
    Check(partition, "短い区間の前後に長い移行を指定しても被覆率の和が1で二重合成しない");
    auto gapBand = roadBand;
    gapBand.spans[1].startMeters = 14;
    Check(graph::SampleSurfaceBand(document, gapBand, 13).empty() &&
          graph::SampleSurfaceBand(document, roadBand, -1).empty() &&
          graph::SampleSurfaceBand(document, roadBand, std::numeric_limits<float>::quiet_NaN()).empty(),
          "空白区間・範囲外・非有限値を隣の素材で埋めない");
    auto hardBand = roadBand;
    hardBand.spans[0].blendOutMeters = hardBand.spans[1].blendInMeters = 0;
    const auto hard = graph::SampleSurfaceBand(document, hardBand, 12);
    Check(hard.size() == 1 && hard[0].preset == presets[1], "移行距離0の境界は次区間に属する");
    auto defaultsBand = roadBand;
    defaultsBand.spans[0].parameters.clear();
    Check(graph::SampleSurfaceBand(document, defaultsBand, 0)[0].parameters[0].value == 0.5f,
          "公開値を指定しない区間はプリセットの既定値を使う");

    const auto pathId = sceneGraph.CreateNode(graph::NodeKind::Path);
    graph::PathSettings path;
    const auto first = graph::AddPathPoint(path, 0, 0, 0);
    graph::AddPathPoint(path, 0, 50, first);
    std::get<graph::PathNodeSettings>(sceneGraph.FindMutableNode(pathId)->settings).path = path;
    sceneGraph.CreateLink(sceneGraph.FindNode(pathId)->outputs[0].id, sceneGraph.FindNode(linked.layouts[0].roadNode)->inputs[0].id);
    const auto preview = graph::CompileSurfaceLayoutPreview(sceneGraph, linked, linked.layouts[0].roadNode);
    Check(preview.error.empty() && preview.scene.meshes.size() == 4 && renderer::ValidateMeshScene(preview.scene),
          "配置から道路と3素材の評価コンテキストを生成する");
    if (preview.scene.meshes.size() == 4) {
        const auto& mask = preview.scene.meshes[0].roadMask;
        Check(mask.IsValid() && mask.rgba[0] == 0 && mask.rgba[1] == 0 && mask.rgba[mask.rgba.size() - 3] == 255,
              "道路始端は第1素材、終端は第3素材の共通マスクになる");
        Check(preview.scene.meshes[3].materialStack->Layers()[0].material == 3 &&
              preview.scene.meshes[3].displacementMeters == linked.presets[2].displacementMeters,
              "プリセットの素材参照と変位量を既存評価器へ渡す");
    }
    auto layered = linked;
    auto& preset = layered.presets[0];
    preset.layerBlendRange = 0.37f;
    for (uint32_t slot = 1; slot < 4; ++slot) {
        graph::PresetMaterial material;
        material.material = slot + 10;
        material.uvRepeatMeters = 1.5f * static_cast<float>(slot);
        material.worldUv = slot == 3;
        material.metallic = 0.1f; material.ambientOcclusion = 0.7f;
        material.mask.emplace();
        material.mask->shape = static_cast<graph::RoadMaskShape>(slot - 1);
        material.mask->seed = slot * 13;
        material.mask->edgeSide = graph::RoadMaskSide::Left;
        material.mask->strength = 0.6f;
        material.heightGate = slot % 3;
        material.heightGateThreshold = 0.3f;
        material.heightGateSoftness = 0.13f;
        material.blendMode = slot % 2;
        preset.materials.push_back(material);
    }
    const auto layeredJson = io::WriteSurfaceLayouts(layered);
    graph::SurfaceLayoutDocument layeredDecoded;
    Check(io::ReadSurfaceLayouts(layeredJson, layeredDecoded, error) && io::WriteSurfaceLayouts(layeredDecoded) == layeredJson,
          "4層の道路マスク・高さ条件・UV・PBR値が保存往復する");
    const auto layeredPreview = graph::CompileSurfaceLayoutPreview(sceneGraph, layeredDecoded, linked.layouts[0].roadNode);
    Check(layeredPreview.error.empty() && renderer::ValidateMeshScene(layeredPreview.scene), "多層プリセットをGPU評価用のシーンへ変換する");
    if (layeredPreview.scene.meshes.size() == 4) {
        const auto& context = layeredPreview.scene.meshes[1];
        graph::RoadGeometry evaluatedRoad;
        graph::EvaluateRoad(sceneGraph, linked.layouts[0].roadNode, evaluatedRoad, error);
        const auto lanes = graph::ComputeRoadLanes(evaluatedRoad.settings, sceneGraph.RoadNetwork().leftHandTraffic);
        const graph::RoadMaskNodeSettings* channels[] = {&*preset.materials[1].mask, &*preset.materials[2].mask, &*preset.materials[3].mask};
        const auto expected = graph::BakeRoadMask(channels, evaluatedRoad.settings.widthMeters, 50, &lanes, &evaluatedRoad);
        Check(context.roadMask.rgba == expected.rgba && context.layerStacks[2].has_value() &&
              context.layerStacks[2]->Layers()[0].material == 13 && context.layerWorldUv[3] &&
              context.layerUvRepeat[2] == 3 && context.layerHeightGate[2] == 2 && context.layerBlendRange == 0.37f,
              "既存Roadと同じマスクを生成し、各層の設定を失わない");
    }
    auto legacyJson = encoded;
    legacyJson["version"] = 1;
    for (auto& p : legacyJson["presets"]) {
        p.erase("layerBlendRange");
        for (auto& m : p["materials"])
            for (const auto* key : {"mask", "blendMode", "heightGate", "heightGateThreshold", "heightGateSoftness", "metallic", "ambientOcclusion"}) m.erase(key);
    }
    Check(io::ReadSurfaceLayouts(legacyJson, decoded, error) && io::WriteSurfaceLayouts(decoded) == encoded,
          "旧版の単層記述を既定値で移行する");
    for (const auto* field : {"mask", "heightGate", "metallic"}) {
        auto missing = layeredJson;
        missing["presets"][0]["materials"][1].erase(field);
        Check(!io::ReadSurfaceLayouts(missing, layeredDecoded, error) && io::WriteSurfaceLayouts(layeredDecoded) == layeredJson,
              "新版の必須項目欠落を既存文書を変更せず拒否する");
    }
    auto invalidMask = layeredJson;
    invalidMask["presets"][0]["materials"][1]["mask"]["shape"] = 99;
    Check(!io::ReadSurfaceLayouts(invalidMask, layeredDecoded, error), "不正な道路マスクを拒否する");
    auto invalidLayer = layered;
    invalidLayer.presets[0].materials[1].heightGateSoftness = std::numeric_limits<float>::quiet_NaN();
    Check(!graph::ValidateSurfaceLayouts(invalidLayer, error), "非有限の層条件をGPUへ渡さない");
    DocumentSnapshot layeredBefore;
    layeredBefore.surfaceLayouts = layered;
    auto layeredAfter = layeredBefore;
    layeredAfter.surfaceLayouts.presets[0].materials[1].mask->strength = 0.1f;
    history.Clear();
    history.Push(layeredBefore, 0);
    Check(io::WriteSurfaceLayouts(history.Undo(layeredAfter).surfaceLayouts) == layeredJson, "層マスクの編集をUndoで復元する");
    graph::SurfaceLayoutDocument editing;
    const auto roadId = linked.layouts[0].roadNode;
    Check(graph::CreateRoadLayout(editing, sceneGraph, roadId, error), "現在の道路から区間と材質プリセットを作る");
    auto* band = graph::FindRoadBand(editing, roadId);
    Check(band && band->spans.size() == 1 && band->spans[0].endMeters == 50, "初期区間は道路の全長を覆う");
    if (band) {
        const auto originalId = band->spans[0].id;
        Check(graph::SplitSurfaceSpan(editing, *band, 0) && band->spans.size() == 2 &&
              band->spans[0].endMeters == band->spans[1].startMeters && band->spans[0].id == originalId,
              "分割で左のIDを保ち、右に新IDを割り当てる");
        Check(graph::DuplicateSurfacePreset(editing, *band, 1) && band->spans[0].preset != band->spans[1].preset,
              "複製した材質は別区間の編集から独立する");
        Check(graph::ValidateSurfaceLayouts(editing, error), "分割と複製後も全ID・参照が有効");
        const auto editingSnapshot = io::WriteSurfaceLayouts(editing);
        Check(graph::RemoveSurfaceSpan(*band, 0) && band->spans[0].startMeters == 0 && band->spans[0].endMeters == 50,
              "区間を削除すると隣の区間が埋める");
        Check(!graph::RemoveSurfaceSpan(*band, 0), "最後の区間は削除しない");
        Check(graph::ResizeSurfaceBand(*band, 24) && band->spans.back().endMeters == 24, "道路長の変更へ区間を合わせる");
        Check(io::ReadSurfaceLayouts(editingSnapshot, editing, error), "操作前の保存記述へ戻せる");
    }
    const auto markingId = sceneGraph.CreateNode(graph::NodeKind::RoadMarking);
    const auto outputId = sceneGraph.CreateNode(graph::NodeKind::MeshOutput);
    sceneGraph.CreateLink(sceneGraph.FindNode(roadId)->outputs[0].id, sceneGraph.FindNode(markingId)->inputs[0].id);
    sceneGraph.CreateLink(sceneGraph.FindNode(markingId)->outputs[0].id, sceneGraph.FindNode(outputId)->inputs[0].id);
    const auto normalScene = graph::CompileMeshGraphWithLayouts(sceneGraph, editing);
    Check(normalScene.error.empty() && renderer::ValidateMeshScene(normalScene.scene) && normalScene.scene.meshes.size() == 4,
          "通常のMesh Outputへ道路2材質と白線を統合する");
    if (normalScene.scene.meshes.size() == 4) {
        Check(normalScene.scene.meshes[0].connectionSources[0] == 2 && normalScene.scene.meshes[0].connectionSources[1] == 3 &&
              normalScene.scene.meshes[1].displacementSource == 0 && normalScene.scene.meshes[1].useBlendMode,
              "白線の材質を保ち、変位元の道路と内部コンテキストの参照を維持する");
    }
    auto badRange = editing;
    graph::FindRoadBand(badRange, roadId)->spans.back().endMeters = 40;
    const auto fallback = graph::CompileMeshGraphWithLayouts(sceneGraph, badRange);
    Check(!fallback.error.empty() && fallback.scene.meshes.size() == 2 && fallback.scene.meshes[0].connectionSources[0] == -1,
          "道路長と区間が合わない間は元の道路を表示し、エラーを残す");

    auto retained = linked;
    const auto roadsideBefore = io::WriteSurfaceLayouts(retained)["layouts"][0]["bands"][1];
    auto* retainedRoad = graph::FindRoadBand(retained, roadId);
    const auto bandId = retainedRoad->id;
    retainedRoad->spans.clear();
    Check(graph::CompileMeshGraphWithLayouts(sceneGraph, retained).error.empty(), "解除した空の道路帯は元のRoad材質で表示する");
    Check(graph::CreateRoadLayout(retained, sceneGraph, roadId, error) &&
          graph::FindRoadBand(retained, roadId)->id == bandId &&
          io::WriteSurfaceLayouts(retained)["layouts"][0]["bands"][1] == roadsideBefore,
          "解除後に再開しても沿道と道路帯のIDを保持する");
    DocumentSnapshot editBefore, editAfter;
    editBefore.surfaceLayouts = editing;
    editAfter = editBefore;
    graph::SplitSurfaceSpan(editAfter.surfaceLayouts, *graph::FindRoadBand(editAfter.surfaceLayouts, roadId), 0);
    history.Clear(); history.Push(editBefore, 0);
    const auto editUndo = history.Undo(editAfter);
    Check(io::WriteSurfaceLayouts(editUndo.surfaceLayouts) == io::WriteSurfaceLayouts(editBefore.surfaceLayouts) &&
          io::WriteSurfaceLayouts(history.Redo(editUndo).surfaceLayouts) == io::WriteSurfaceLayouts(editAfter.surfaceLayouts),
          "区間分割の全IDをUndo・Redoで復元する");

    tests::Section("沿道断面 — 左右・移行・高さ・保存");
    graph::SurfaceLayoutDocument roadside;
    Check(graph::CreateRoadsideExample(roadside, sceneGraph, roadId, graph::SurfaceSide::Left, error),
          "左側に路肩から歩道へ移る記述を作れる");
    const auto roadsideJson = io::WriteSurfaceLayouts(roadside);
    Check(!graph::CreateRoadsideExample(roadside, sceneGraph, roadId, graph::SurfaceSide::Left, error) &&
          io::WriteSurfaceLayouts(roadside) == roadsideJson, "既存の沿道は上書きしない");
    graph::RoadGeometry bandRoad;
    graph::EvaluateRoad(sceneGraph, roadId, bandRoad, error);
    renderer::MeshData bandMesh;
    Check(graph::BuildSurfaceBandGeometry(bandRoad, roadside, roadside.layouts[0].bands[1], bandMesh, error),
          "路肩から歩道の連続した断面を生成する");
    renderer::SceneMesh bandSceneMesh; bandSceneMesh.geometry = bandMesh;
    renderer::MeshScene bandScene; bandScene.meshes.push_back(bandSceneMesh);
    Check(renderer::ValidateMeshScene(bandScene), "沿道の頂点・法線・接線・インデックスが有効");
    if (!bandMesh.vertices.empty()) {
        Check(std::abs(bandMesh.vertices.front().position.x - bandRoad.surface.vertices[bandRoad.stride - 1].position.x) < 1e-5f &&
              std::abs(bandMesh.vertices.back().position.y - 0.15f) < 1e-5f,
              "内端は道路端に一致し、終端の歩道は15 cm上がる");
        bool hasSlope = false;
        for (const auto& v : bandMesh.vertices) if (v.uv.x == 1 && v.position.y > 0 && v.position.y < 0.14f) hasSlope = true;
        Check(hasSlope, "移行区間には中間の高さが存在する");
    }
    auto rightSide = roadside;
    rightSide.layouts[0].bands[1].side = graph::SurfaceSide::Right;
    renderer::MeshData rightMesh;
    Check(graph::BuildSurfaceBandGeometry(bandRoad, rightSide, rightSide.layouts[0].bands[1], rightMesh, error),
          "右側にも同じ断面を生成できる");
    if (!rightMesh.vertices.empty() && rightMesh.vertices.size() == bandMesh.vertices.size()) {
        bool mirrored = true;
        for (size_t i = 0; i < rightMesh.vertices.size(); ++i)
            mirrored &= std::abs(rightMesh.vertices[i].position.x + bandMesh.vertices[i].position.x) < 1e-5f &&
                        std::abs(rightMesh.vertices[i].normal.y - bandMesh.vertices[i].normal.y) < 1e-5f;
        Check(mirrored, "左右を反転しても面の表裏は反転しない");
        for (const auto* mesh : {&bandMesh, &rightMesh}) {
            const auto& v = mesh->vertices.front();
            const float bitangentZ = (v.normal.x * v.tangent.y - v.normal.y * v.tangent.x) * v.tangent.w;
            Check(bitangentZ > 0, "左右とも法線マップのV方向が道路の進行方向と一致する");
        }
    }
    auto bentRoad = bandRoad;
    for (auto& v : bentRoad.surface.vertices) {
        v.position.x += std::sin(v.position.z * 0.05f);
        v.position.y += v.position.z * 0.02f;
    }
    Check(graph::BuildSurfaceBandGeometry(bentRoad, roadside, roadside.layouts[0].bands[1], rightMesh, error),
          "曲がりと縦断高さを持つ道路格子に沿道が追従する");
    if (!rightMesh.vertices.empty()) Check(std::abs(rightMesh.vertices.back().position.y - 1.15f) < 1e-5f,
          "沿道の高さは道路の縦断高さを基準にする");
    auto roundedRoad = bentRoad;
    roundedRoad.rowDistances[1] = 0.25000003f;
    Check(graph::BuildSurfaceBandGeometry(roundedRoad, roadside, roadside.layouts[0].bands[1], rightMesh, error),
          "等間隔点と道路格子の距離が丸め誤差で重なっても沿道を生成できる");
    auto brokenSide = roadside;
    brokenSide.layouts[0].bands[1].spans[0].blendOutMeters = 0;
    brokenSide.layouts[0].bands[1].spans[1].blendInMeters = 0;
    const auto retainedCount = rightMesh.vertices.size();
    Check(!graph::BuildSurfaceBandGeometry(bandRoad, brokenSide, brokenSide.layouts[0].bands[1], rightMesh, error) &&
          !error.empty() && rightMesh.vertices.size() == retainedCount, "移行なしの異種断面は拒否し、出力を保持する");
    graph::SurfaceLayoutDocument roadsideReloaded;
    Check(io::ReadSurfaceLayouts(roadsideJson, roadsideReloaded, error) &&
          graph::BuildSurfaceBandGeometry(bandRoad, roadsideReloaded, roadsideReloaded.layouts[0].bands[1], rightMesh, error) &&
          rightMesh.vertices.size() == bandMesh.vertices.size() && rightMesh.indices == bandMesh.indices,
          "保存した仮沿道から同じ分割の形状を再生成する");

    tests::Section("沿道材質 — 下地と区間混合");
    roadside.presets[0].materials[0].material = 13;
    roadside.presets[0].materials[0].uvRepeatMeters = 3;
    roadside.presets[0].materials[0].worldUv = true;
    roadside.presets[1].materials[0].baseColor = {0.2f, 0.3f, 0.4f};
    roadside.presets[1].materials[0].roughness = 0.67f;
    roadside.presets[1].displacementMeters = 0.2f;
    const auto bandPreview = graph::CompileSurfaceBandPreview(sceneGraph, roadside, roadId, roadside.layouts[0].bands[1].id);
    Check(bandPreview.error.empty() && bandPreview.scene.meshes.size() == 3 && renderer::ValidateMeshScene(bandPreview.scene),
          "沿道を形状1件と材質2件で描画できる構成へ変換する");
    if (bandPreview.scene.meshes.size() == 3) {
        const auto& surface = bandPreview.scene.meshes[0];
        const auto& ground = bandPreview.scene.meshes[1];
        const auto& walk = bandPreview.scene.meshes[2];
        Check(surface.connectionSources == std::array<int, 3>{1, 2, 2} && ground.materialOnly && walk.materialOnly,
              "沿道の素材参照は評価専用コンテキストを指す");
        Check(ground.materialStack->Layers()[0].material == 13 && ground.layerUvRepeat[0] == 3 && ground.layerWorldUv[0] &&
              walk.materialStack->Layers()[0].roughness == 0.67f && walk.materialStack->Layers()[0].baseColor.y == 0.3f,
              "下地の素材参照・反復長・座標・PBR定数を保持する");
        Check(surface.displacementMeters == 0 && walk.displacementMeters == 0, "断面の継ぎ目を独立した材質変位で割らない");
        Check(std::abs(surface.geometry.vertices.back().uv.x - 2.15f) < 1e-5f &&
              surface.geometry.vertices.back().roadUv.y == 50, "垂直面を含む断面長と進行方向の実距離をUVに保持する");
        const auto& mask = surface.roadMask;
        Check(mask.rgba.front() == 0 && mask.rgba[(mask.height - 1) * 4] == 255 &&
              mask.rgba[(mask.height / 2) * 4] >= 127 && mask.rgba[(mask.height / 2) * 4] <= 130,
              "断面と同じ移行位置で下地材質の重みが0から1へ変わる");
        Check(surface.geometry.indices == bandMesh.indices && surface.geometry.vertices.front().position.x == bandMesh.vertices.front().position.x,
              "材質を付けても沿道の形状は変わらない");
    }
    auto unsupportedBand = roadside;
    unsupportedBand.presets[0].materials.emplace_back();
    unsupportedBand.presets[0].materials.back().mask.emplace();
    const auto rejectedBand = graph::CompileSurfaceBandPreview(sceneGraph, unsupportedBand, roadId, unsupportedBand.layouts[0].bands[1].id);
    Check(!rejectedBand.error.empty() && rejectedBand.scene.meshes.empty(), "未対応の上層を黙って捨てずに理由を返す");
    graph::SurfaceLayoutDocument materialReload;
    Check(io::ReadSurfaceLayouts(io::WriteSurfaceLayouts(roadside), materialReload, error), "沿道の下地材質が保存往復する");
    const auto reloadedBand = graph::CompileSurfaceBandPreview(sceneGraph, materialReload, roadId, materialReload.layouts[0].bands[1].id);
    Check(reloadedBand.error.empty() && reloadedBand.scene.meshes[0].roadMask.rgba == bandPreview.scene.meshes[0].roadMask.rgba,
          "再読込後も沿道の材質の移行が一致する");

    tests::Section("横接続 — 共通境界と段差の保持");
    auto lateralScene = graph::CompileMeshGraph(sceneGraph);
    const size_t originalMeshes = lateralScene.scene.meshes.size();
    const auto lateralBandId = roadside.layouts[0].bands[1].id;
    Check(graph::ConnectSurfaceBandMaterials(lateralScene, sceneGraph, roadside, roadId, lateralBandId, error) &&
          renderer::ValidateMeshScene(lateralScene.scene), "道路1構成と左沿道2構成の材質を接続する");
    if (lateralScene.scene.meshes.size() == originalMeshes + 4) {
        const auto& roadSurface = lateralScene.scene.meshes[0];
        const auto& sideSurface = lateralScene.scene.meshes.back();
        Check(roadSurface.connectionSources == sideSurface.connectionSources &&
              roadSurface.connectionOrigins[1].x == bandRoad.settings.widthMeters &&
              roadSurface.roadWidthMeters == sideSurface.roadWidthMeters,
              "道路と沿道が同じ材質参照・境界座標・マスク縮尺を使う");
        const auto& a = roadSurface.roadMask; const auto& b = sideSurface.roadMask;
        bool sameGround = true;
        for (uint32_t x = 0; x < a.width * 4; ++x) sameGround &= a.rgba[x] == b.rgba[x];
        Check(sameGround, "馴染ませる路肩区間では両面の境界比率が完全に一致する");
        const uint32_t edge = static_cast<uint32_t>(bandRoad.settings.widthMeters / roadSurface.roadWidthMeters * float(a.width));
        const auto roadEnd = (size_t(a.height - 1) * a.width + edge) * 4;
        Check(a.rgba[roadEnd] == 0 && a.rgba[roadEnd + 1] == 0 && b.rgba[roadEnd + 1] == 255,
              "段差を残す歩道は道路へ滲ませず、歩道側の材質を保持する");
        Check(a.rgba[edge * 4] > 0 && a.rgba[edge * 4] < 255,
              "路肩との境界では両側の材質が混ざる");
        Check(roadSurface.displacementMeters == 0 && sideSurface.displacementMeters == 0 &&
              lateralScene.scene.meshes[originalMeshes].displacementMeters == 0,
              "材質接続の試作では道路と沿道の変位を停止する");
        Check(lateralScene.scene.meshes[1].displacementSource == 0 &&
              lateralScene.scene.meshes[1].geometry.indices == normalScene.scene.meshes[1].geometry.indices,
              "白線の形状と参照先は材質接続後も保持する");
    }
    auto unsupportedScene = normalScene;
    const auto countBefore = unsupportedScene.scene.meshes.size();
    Check(!graph::ConnectSurfaceBandMaterials(unsupportedScene, sceneGraph, roadside, roadId, lateralBandId, error) &&
          unsupportedScene.scene.meshes.size() == countBefore &&
          unsupportedScene.scene.meshes[0].connectionSources == normalScene.scene.meshes[0].connectionSources,
          "複数の道路プリセットは部分的に接続せずシーンを保持する");
    auto wrongSideScene = graph::CompileMeshGraph(sceneGraph);
    Check(graph::ConnectSurfaceBandMaterials(wrongSideScene, sceneGraph, rightSide, roadId,
          rightSide.layouts[0].bands[1].id, error, true), "右側にも材質と変位を接続できる");
    if (wrongSideScene.scene.meshes.size() == originalMeshes + 4) {
        const auto& r = wrongSideScene.scene.meshes[0];
        const auto& rightSurface = wrongSideScene.scene.meshes.back();
        Check(r.connectionAcrossSigns[0] == -1 && r.connectionFrameSign == -1 && rightSurface.connectionFrameSign == 1,
              "右接続は道路の素材座標と接線の反転を明示する");
        Check(r.connectionHeightFade.x == bandRoad.settings.widthMeters && renderer::ValidateMeshScene(wrongSideScene.scene),
              "右接続も同じ境界高さへ変位を減衰する");
        const auto original = graph::CompileMeshGraph(sceneGraph);
        bool preserved = true;
        for (size_t m = 0; m < originalMeshes; ++m) {
            for (size_t i = 0; i < original.scene.meshes[m].geometry.vertices.size(); ++i) {
                const auto a = original.scene.meshes[m].geometry.vertices[i].roadUv;
                const auto b = wrongSideScene.scene.meshes[m].geometry.vertices[i].roadUv;
                preserved &= std::abs((r.connectionOrigins[0].x - b.x) - a.x * original.scene.meshes[0].roadMetersPerUv) < 1e-5f;
            }
        }
        Check(preserved, "右接続後も道路と白線の素材参照位置を保持する");
        Check(rightSurface.geometry.vertices.front().roadUv.x == r.connectionHeightFade.x, "右路肩の内端も共通境界座標に一致する");
    }
    auto axisGraph = sceneGraph;
    std::get<graph::RoadNodeSettings>(axisGraph.FindMutableNode(roadId)->settings).uvAlongU = true;
    auto axisScene = graph::CompileMeshGraph(axisGraph);
    Check(graph::ConnectSurfaceBandMaterials(axisScene, axisGraph, roadside, roadId, lateralBandId, error),
          "道路のUVが長さ方向Uでも横接続を生成できる");
    if (!axisScene.scene.meshes.empty()) {
        const auto& axisRoad = axisScene.scene.meshes[0];
        Check(std::abs(axisRoad.geometry.vertices.back().roadUv.x - bandRoad.settings.widthMeters) < 1e-5f &&
              std::abs(axisRoad.geometry.vertices.back().roadUv.y - 50) < 1e-5f && !axisRoad.roadUvAlongU,
              "元のUV軸に依存せず横距離・進行距離の共通座標へ変換する");
        Check(axisScene.meshSources.size() == axisScene.scene.meshes.size(), "追加後も描画メッシュと由来の配列が対応する");
    }

    tests::Section("左右同時接続 — 独立した区間と共通座標");
    auto bothDocument = roadside;
    Check(graph::CreateRoadsideExample(bothDocument, sceneGraph, roadId, graph::SurfaceSide::Right, error),
          "既存の左沿道を保って右沿道を追加する");
    auto& rightBand = bothDocument.layouts[0].bands.back();
    rightBand.spans[0].endMeters = rightBand.spans[1].startMeters = 18;
    auto bothScene = graph::CompileMeshGraph(sceneGraph);
    Check(graph::ConnectBothSurfaceBands(bothScene, sceneGraph, bothDocument, roadId, lateralBandId, rightBand.id, error, true),
          "左右の異なる区切りを持つ沿道を同時に接続する");
    if (bothScene.scene.meshes.size() == originalMeshes + 7) {
        const auto& bothRoad = bothScene.scene.meshes[0];
        const auto& leftSurface = bothScene.scene.meshes[originalMeshes + 3];
        const auto& rightSurface = bothScene.scene.meshes.back();
        Check(renderer::ValidateMeshScene(bothScene.scene) && bothRoad.connectionExtraSources[0] >= 0,
              "左右同時接続の5材質参照が有効");
        Check(bothRoad.connectionSources == rightSurface.connectionSources &&
              bothRoad.connectionExtraSources == leftSurface.connectionExtraSources &&
              bothRoad.roadWidthMeters == rightSurface.roadWidthMeters,
              "道路と両沿道が同じ材質参照とマスク縮尺を共有する");
        Check(std::abs(leftSurface.geometry.vertices.front().roadUv.x - bothRoad.connectionHeightFade.x) < 1e-5f &&
              std::abs(rightSurface.geometry.vertices.front().roadUv.x - bothRoad.connectionSecondHeightFade.x) < 1e-5f,
              "左右の境界がそれぞれ共通の変位減衰位置に一致する");
        bool sharedGround = true;
        for (size_t i = 0; i < size_t(bothRoad.roadMask.width) * 4; ++i)
            sharedGround &= bothRoad.roadMask.rgba[i] == leftSurface.roadMask.rgba[i] &&
                            bothRoad.roadMask.rgba[i] == rightSurface.roadMask.rgba[i];
        Check(sharedGround, "両側が路肩の区間は3面の混合率が一致する");
        const auto original = graph::CompileMeshGraph(sceneGraph);
        bool whiteUv = true;
        for (size_t i = 0; i < original.scene.meshes[1].geometry.vertices.size(); ++i)
            whiteUv &= std::abs(bothScene.scene.meshes[1].geometry.vertices[i].roadUv.x - bothRoad.connectionOrigins[0].x -
                original.scene.meshes[1].geometry.vertices[i].roadUv.x * original.scene.meshes[0].roadMetersPerUv) < 1e-5f;
        Check(whiteUv, "左右同時接続後も白線が元の道路位置を参照する");
        auto invalidBoth = bothScene.scene;
        invalidBoth.meshes[0].connectionExtraSources[1] = -1;
        Check(!renderer::ValidateMeshScene(invalidBoth), "片方だけ欠けた追加材質参照を拒否する");
    }
    auto failedBoth = graph::CompileMeshGraph(sceneGraph);
    const auto beforeBoth = failedBoth.scene.meshes.size();
    Check(!graph::ConnectBothSurfaceBands(failedBoth, sceneGraph, bothDocument, roadId, lateralBandId, lateralBandId, error, true) &&
          failedBoth.scene.meshes.size() == beforeBoth && failedBoth.scene.meshes[0].connectionSources[0] == -1,
          "左右の指定が不正なら途中の接続を残さない");

    tests::Section("横接続の変位 — 共通の高さ基準と白線UV");
    auto displacedScene = graph::CompileMeshGraph(axisGraph);
    const auto oldRoad = displacedScene.scene.meshes[0];
    const auto oldMarking = displacedScene.scene.meshes[1];
    Check(graph::ConnectSurfaceBandMaterials(displacedScene, axisGraph, roadside, roadId, lateralBandId, error, true),
          "変位を有効にした横接続を生成できる");
    if (displacedScene.scene.meshes.size() == originalMeshes + 4) {
        const auto& roadMesh = displacedScene.scene.meshes[0];
        const auto& sideMesh = displacedScene.scene.meshes.back();
        Check(roadMesh.displacementMeters == 1 && sideMesh.displacementMeters == 1 &&
              roadMesh.connectionPrototype && sideMesh.connectionPrototype,
              "道路・沿道の変位方向を世界Yに揃える");
        Check(roadMesh.connectionHeightFade.x == bandRoad.settings.widthMeters &&
              roadMesh.connectionHeightFade.x == sideMesh.connectionHeightFade.x &&
              roadMesh.connectionHeightFade.y == sideMesh.connectionHeightFade.y && roadMesh.connectionHeightFade.y > 0,
              "両面に同じ境界位置と変位抑制幅を渡す");
        Check(displacedScene.scene.meshes[originalMeshes + 2].displacementMeters == roadside.presets[1].displacementMeters,
              "境界から離れた歩道の変位量はプリセットから取得する");
        bool uvMatches = true;
        for (size_t i = 0; i < oldMarking.geometry.vertices.size(); ++i) {
            const auto oldUv = oldMarking.geometry.vertices[i].roadUv;
            const auto uv = displacedScene.scene.meshes[1].geometry.vertices[i].roadUv;
            uvMatches &= std::abs(uv.x - oldUv.y * oldRoad.roadMetersPerUv) < 1e-5f &&
                         std::abs(uv.y - oldUv.x * oldRoad.roadMetersPerUv) < 1e-5f;
        }
        Check(uvMatches, "白線も道路と同じ実距離UVから変位を評価する");
        auto invalidScene = displacedScene.scene;
        invalidScene.meshes[0].connectionHeightFade.y = std::numeric_limits<float>::quiet_NaN();
        Check(!renderer::ValidateMeshScene(invalidScene), "不正な変位抑制幅をGPUへ渡さない");
    }

}
