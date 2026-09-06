#pragma once

#include "graph/NodeGraph.h"

#include <cstdint>
#include <vector>

// 道路空間マスク。横位置（m。正が Left 側）と実距離（m）から 0〜1 を返す純粋な関数と、
// それを道路 1 本ぶんの低解像度画像（横 256 px、長さ 1 m あたり 16 px）へ焼く処理。
// 設計は docs/design/road-material-layers.md。
namespace tg::graph {

// 1 点の評価。halfWidth は道路幅の半分、length は道路の全長（m）。
float EvaluateRoadMask(const RoadMaskNodeSettings& settings, float lateralMeters, float distanceMeters,
                       float halfWidthMeters, float lengthMeters);

// 道路 1 本ぶんのマスク画像（RGBA8）。R / G / B がスロット 2〜4、A は予約（255）。
struct RoadMaskImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
    bool IsValid() const { return width > 0 && height > 0 && rgba.size() == size_t(width) * height * 4; }
};
// channels[i] が nullptr のスロットは 0。列 0 が Right 端（u = 0）、行 0 が始点。
RoadMaskImage BakeRoadMask(const RoadMaskNodeSettings* const channels[3], float widthMeters,
                           float lengthMeters);

}  // namespace tg::graph
