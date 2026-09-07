#pragma once
#include <cstdint>
#include <string>

namespace tg::compositor {
// 境界帯の共通設定。テクスチャはTextureLibraryの安定IDで参照する。
struct BoundaryMaterial {
    uint32_t id = 0;
    std::string name;
    uint32_t mask = 0, height = 0;
    float widthMeters = 0.5f;
    float repeatMeters = 2.0f;
    float depthMeters = 0.03f;
    float heightCenter = 0.5f;
    bool alongU = false;
    bool invertMask = false;
};
}
