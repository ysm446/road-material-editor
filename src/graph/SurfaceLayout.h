#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace tg::graph {

// 文書内の安定ID。グラフのノードID・マテリアルIDとは別の名前空間。
using SurfaceId = uint32_t;
enum class SurfaceRole : uint32_t { Road, Ground, Sidewalk };
enum class BoundaryMode : uint32_t { Blend, KeepStep, Fixed };
enum class SurfaceSide : uint32_t { Road, Left, Right };

struct BoundaryContract {
    BoundaryMode mode = BoundaryMode::Blend;
    float transitionMeters = 0.5f;
    float maxHeightAdjustment = 0.15f;
    bool preserveOutline = false;
};
struct CrossSectionPoint {
    SurfaceId id = 0;
    float across = 0, height = 0;
};
struct PresetParameter {
    SurfaceId id = 0;
    std::string name;
    float minimum = 0, maximum = 1, defaultValue = 0;
};
struct PresetMaterial {
    uint32_t material = 0; // プロジェクトに埋め込まれるPBR素材への参照。0は定数材質。
    float uvRepeatMeters = 2;
    bool worldUv = false;
    std::array<float, 3> baseColor{0.42f, 0.4f, 0.36f};
    float roughness = 0.8f;
};
struct SurfacePreset {
    SurfaceId id = 0;
    uint32_t version = 1;
    std::string name;
    SurfaceRole role = SurfaceRole::Ground;
    float displacementMeters = 0;
    std::vector<CrossSectionPoint> section;
    // 内端・外端・始端・終端。接続相手別の定義は持たない。
    std::array<BoundaryContract, 4> boundaries;
    std::vector<PresetMaterial> materials;
    std::vector<PresetParameter> parameters;
};
struct SpanParameter {
    SurfaceId parameter = 0;
    float startValue = 0, endValue = 0;
};
struct SurfaceSpan {
    SurfaceId id = 0, preset = 0;
    float startMeters = 0, endMeters = 1;
    float blendInMeters = 0, blendOutMeters = 0;
    uint32_t seed = 1;
    std::vector<SpanParameter> parameters;
};
struct SurfaceBand {
    SurfaceId id = 0;
    SurfaceSide side = SurfaceSide::Road;
    // 同じ側はRoadに近い帯から順に格納。区間の切れ目は帯ごとに独立。
    std::vector<SurfaceSpan> spans;
};
struct RoadLayout {
    SurfaceId id = 0;
    int32_t roadNode = 0;
    std::vector<SurfaceBand> bands;
};
struct SurfaceLayoutDocument {
    SurfaceId nextId = 1;
    std::vector<SurfacePreset> presets;
    std::vector<RoadLayout> layouts;
    SurfaceId AllocateId();
};

// 参照・区間・寸法を検査する。シーングラフやGPUには依存しない。
bool ValidateSurfaceLayouts(const SurfaceLayoutDocument& document, std::string& error);
class NodeGraph;
bool ValidateSurfaceLayoutRoads(const SurfaceLayoutDocument& document, const NodeGraph& graph, std::string& error);

}  // namespace tg::graph
