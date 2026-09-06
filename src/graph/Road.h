#pragma once

#include "graph/NodeGraph.h"
#include "renderer/MeshData.h"

namespace tg::graph {

// GPUに依存しない道路面と左右境界。左右はパスの進行方向を基準にする。
struct RoadGeometry {
    renderer::MeshData surface;
    PathSettings left;
    PathSettings right;
    // surface は行×列の格子。1行の頂点数（列数+1）と生成時の設定。白線などの部品が行を参照する。
    uint32_t stride = 0;
    RoadNodeSettings settings;
};
bool BuildRoad(const PathSettings& path, const RoadNodeSettings& settings,
               RoadGeometry& result, std::string& error);
// RoadのLeft/Rightも実寸Pathとして解決できる。循環と過大な依存は拒否する。
bool EvaluateRoad(const NodeGraph& graph, GraphId nodeId, RoadGeometry& result,
                  std::string& error);
// 道路面の行ごとの左右端から横位置を補間し、白線の帯ポリゴンを1メッシュにまとめる。
// 中央線は横位置0、外側線は道路端から edgeInsetMeters 内側。UVは幅方向0〜1、長さ方向は実距離÷UV反復長。
bool BuildRoadMarkings(const RoadGeometry& road, const RoadMarkingNodeSettings& settings,
                       renderer::MeshData& result, std::string& error);
struct CompiledMeshGraph {
    bool active = false;
    renderer::MeshScene scene;
    std::string error;
};
// Mesh Outputへ接続された道路と、その上の白線をシーンへ追加する。
CompiledMeshGraph CompileMeshGraph(const NodeGraph& graph);

}  // namespace tg::graph
