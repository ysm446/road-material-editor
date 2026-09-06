#pragma once

#include "graph/NodeGraph.h"
#include "renderer/MeshData.h"

namespace tg::graph {

// GPUに依存しない道路面と左右境界。左右はパスの進行方向を基準にする。
struct RoadGeometry {
    renderer::MeshData surface;
    PathSettings left;
    PathSettings right;
};
bool BuildRoad(const PathSettings& path, const RoadNodeSettings& settings,
               RoadGeometry& result, std::string& error);
// RoadのLeft/Rightも実寸Pathとして解決できる。循環と過大な依存は拒否する。
bool EvaluateRoad(const NodeGraph& graph, GraphId nodeId, RoadGeometry& result,
                  std::string& error);
struct CompiledMeshGraph {
    bool active = false;
    renderer::MeshScene scene;
    std::string error;
};
// Mesh Outputへ接続された道路だけをシーンへ追加する。
CompiledMeshGraph CompileMeshGraph(const NodeGraph& graph);

}  // namespace tg::graph
