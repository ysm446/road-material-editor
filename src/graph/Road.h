#pragma once

#include "graph/NodeGraph.h"
#include "renderer/MeshData.h"

namespace tg::graph {

// GPUに依存しない道路面と左右境界。左右は進行方向に向かっての左右（右手系 Y-up）。
// 格子の列 0 が Right、列末尾が Left。
struct RoadGeometry {
    renderer::MeshData surface;
    PathSettings left;
    PathSettings right;
    // surface は行×列の格子。1行の頂点数（列数+1）と生成時の設定。白線などの部品が行を参照する。
    uint32_t stride = 0;
    RoadNodeSettings settings;
    // 行ごとの始点からの実距離（m）。UV の向きに依存しない。
    std::vector<float> rowDistances;
};
bool BuildRoad(const PathSettings& path, const RoadNodeSettings& settings,
               RoadGeometry& result, std::string& error);
// RoadのLeft/Rightも実寸Pathとして解決できる。循環と過大な依存は拒否する。
bool EvaluateRoad(const NodeGraph& graph, GraphId nodeId, RoadGeometry& result,
                  std::string& error);
// 道路面の行ごとの左右端から横位置を補間し、白線の帯ポリゴンを1メッシュにまとめる。
// 中央線は横位置0、外側線は道路端から edgeInsetMeters 内側。UVは幅方向0〜1、長さ方向は実距離÷UV反復長。
// 進行方向の矢印は左右の車線の中央に置き、走行側の車線は線形の向き、対向車線は逆向きにする。
bool BuildRoadMarkings(const RoadGeometry& road, const RoadMarkingNodeSettings& settings,
                       bool leftHandTraffic, renderer::MeshData& result, std::string& error);
// --- 面上の座標 -------------------------------------------------------------
// 道路面上の点。distance は始点からの実距離、lateral は横位置（m、正が Left = 列末尾側）。
struct RoadSurfacePoint {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
};
RoadSurfacePoint RoadSurfacePointAt(const RoadGeometry& road, float distanceMeters, float lateralMeters);
// 世界座標を道路座標へ。最寄りの行の間で補間する。道路面が無ければ偽。
bool RoadSurfaceCoordinates(const RoadGeometry& road, const DirectX::XMFLOAT3& world,
                            float& outDistanceMeters, float& outLateralMeters);
// レイと道路面の最初の交点。
bool RayHitsRoad(const RoadGeometry& road, const DirectX::XMFLOAT3& origin, const DirectX::XMFLOAT3& direction,
                 DirectX::XMFLOAT3& outHit);
// Path の Surface 入力から上流をたどり、面を作っている Road ノードを返す。無ければ nullptr。
const Node* FindSurfaceRoad(const NodeGraph& graph, const Node& pathNode);
// 路肩。source の格子の列 edgeColumn（境界）を内側の列としてそのまま使い、隣の列 innerColumn から
// 外向きを決めて widthMeters 押し出す。結果も行×列の格子（列 0 が境界、列末尾が Outer）で、
// left に Outer、right に境界の点列を実寸 Path として持つ。rowDistances は source を写す。
bool BuildShoulder(const RoadGeometry& source, uint32_t edgeColumn, uint32_t innerColumn,
                   const ShoulderNodeSettings& settings, RoadGeometry& result, std::string& error);
// Shoulder ノードを評価する。Path 入力の上流（Road の Left / Right、Shoulder の Outer）をたどる。
bool EvaluateShoulder(const NodeGraph& graph, GraphId nodeId, RoadGeometry& result, std::string& error);
// 面上のパス（surfaceSpace）に沿った帯。uv は幅方向 0〜1、長さ方向はパスに沿った実距離÷UV反復長。
bool BuildDecal(const RoadGeometry& road, const PathSettings& surfacePath, const DecalNodeSettings& settings,
                renderer::MeshData& result, std::string& error);

struct CompiledMeshGraph {
    bool active = false;
    renderer::MeshScene scene;
    std::string error;
};
// Mesh Outputへ接続された道路と、その上の白線・Decalをシーンへ追加する。
// 部品（白線・Decal）が失敗しても道路面までは積み、理由を error に「 / 」区切りで残す。
// previewNodeId が Road / Lane Marking / Decal を指すときは、そのノードまでの鎖だけを出す
// （途中経過の確認。Mesh Output は使わない）。
CompiledMeshGraph CompileMeshGraph(const NodeGraph& graph, GraphId previewNodeId = 0);

}  // namespace tg::graph
