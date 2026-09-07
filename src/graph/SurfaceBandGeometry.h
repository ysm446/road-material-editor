#pragma once
#include "graph/SurfaceLayoutEvaluation.h"

namespace tg::graph {
bool CreateRoadsideExample(SurfaceLayoutDocument& document, const NodeGraph& graph, GraphId roadId,
                          SurfaceSide side, std::string& error);
// 道路に隣接する沿道1帯の形状。左・右、全長を覆う区間列に対応。
// 材質変位・複数帯の積み上げ・境界契約の解決は呼び出し側の後続処理。
// 失敗時は出力を保持する。
bool BuildSurfaceBandGeometry(const RoadGeometry& road, const SurfaceLayoutDocument& document,
                              const SurfaceBand& band, renderer::MeshData& result, std::string& error);
}  // namespace tg::graph
