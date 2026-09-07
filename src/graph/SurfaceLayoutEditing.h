#pragma once
#include "graph/SurfaceLayout.h"

namespace tg::graph {
struct SimpleRoadsideDefaults {
    float width = 2;
    float groundHeight = -0.06f;
    float sidewalkHeight = 0.15f;
};
// 道路端を固定した2点の路肩、または垂直段差と水平面を持つ3点の歩道。
bool GetSimpleRoadsideDimensions(const SurfacePreset& preset, float& width, float& height);
bool SetSimpleRoadsideDimensions(SurfacePreset& preset, float width, float height);
SurfaceBand* FindRoadBand(SurfaceLayoutDocument& document, GraphId roadId);
bool CreateRoadLayout(SurfaceLayoutDocument& document, const NodeGraph& graph, GraphId roadId, std::string& error);
bool SplitSurfaceSpan(SurfaceLayoutDocument& document, SurfaceBand& band, size_t index);
bool RemoveSurfaceSpan(SurfaceBand& band, size_t index);
bool ResizeSurfaceBand(SurfaceBand& band, float length);
bool DuplicateSurfacePreset(SurfaceLayoutDocument& document, SurfaceBand& band, size_t index);
void ClampSpanBlends(SurfaceSpan& span);
}  // namespace tg::graph
