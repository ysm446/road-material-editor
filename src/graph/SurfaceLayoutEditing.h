#pragma once
#include "graph/SurfaceLayout.h"

namespace tg::graph {
SurfaceBand* FindRoadBand(SurfaceLayoutDocument& document, GraphId roadId);
bool CreateRoadLayout(SurfaceLayoutDocument& document, const NodeGraph& graph, GraphId roadId, std::string& error);
bool SplitSurfaceSpan(SurfaceLayoutDocument& document, SurfaceBand& band, size_t index);
bool RemoveSurfaceSpan(SurfaceBand& band, size_t index);
bool ResizeSurfaceBand(SurfaceBand& band, float length);
bool DuplicateSurfacePreset(SurfaceLayoutDocument& document, SurfaceBand& band, size_t index);
void ClampSpanBlends(SurfaceSpan& span);
}  // namespace tg::graph
