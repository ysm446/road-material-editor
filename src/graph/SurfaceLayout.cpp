#include "graph/SurfaceLayout.h"
#include "graph/SurfacePresetGraph.h"
#include "graph/NodeGraph.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace tg::graph {
bool ValidateSurfaceLayoutRoads(const SurfaceLayoutDocument& document, const NodeGraph& graph, std::string& error) {
    for (const auto& layout : document.layouts) {
        const auto* road = graph.FindNode(layout.roadNode);
        if (!road || road->kind != NodeKind::Road) { error = "配置先のRoadが存在しません"; return false; }
    }
    error.clear();
    return true;
}

SurfaceId SurfaceLayoutDocument::AllocateId() {
    if (nextId == 0 || nextId == std::numeric_limits<SurfaceId>::max()) return 0;
    return nextId++;
}

bool ValidatePresetMaterial(const PresetMaterial& material, std::string& error) {
    const auto fail = [&](const char* message) { error = message; return false; };
    const auto range = [](float x, float lo, float hi) { return std::isfinite(x) && x >= lo && x <= hi; };
    if (!range(material.uvRepeatMeters, 0.01f, 100) || !range(material.roughness, 0, 1) ||
        !range(material.metallic, 0, 1) || !range(material.ambientOcclusion, 0, 1) ||
        material.blendMode > 1 || material.heightGate > 2 || !range(material.heightGateThreshold, 0, 1) ||
        !range(material.heightGateSoftness, 0.001f, 1)) return fail("材質の寸法・PBR値・合成条件が不正です");
    for (float channel : material.baseColor) if (!range(channel, 0, 1)) return fail("材質色が不正です");
    if (material.mask) {
        const auto& mask = *material.mask;
        if (static_cast<uint32_t>(mask.shape) > 4 || static_cast<uint32_t>(mask.edgeSide) > 2 ||
            !range(mask.laneOffsetMeters, -100, 100) || !range(mask.trackSpacingMeters, 0, 100) ||
            !range(mask.trackWidthMeters, 0, 100) || !range(mask.featherMeters, 0, 100) ||
            !range(mask.edgeWidthMeters, 0, 100) || !range(mask.noiseScaleMeters, 0.05f, 100) ||
            !range(mask.threshold, 0, 1) || !range(mask.softness, 0.0001f, 1) ||
            !range(mask.breakupAmount, 0, 1) || !range(mask.breakupScaleMeters, 0.05f, 100) ||
            !range(mask.strength, 0, 1)) return fail("プリセットの道路マスクが不正です");
    }
    return true;
}

bool ValidateSurfaceLayouts(const SurfaceLayoutDocument& document, std::string& error) {
    error.clear();
    const auto fail = [&](const char* message) { error = message; return false; };
    const auto finite = [](float x) { return std::isfinite(x); };
    const auto range = [&](float x, float lo, float hi) { return finite(x) && x >= lo && x <= hi; };
    std::unordered_set<SurfaceId> ids;
    const auto idValid = [&](SurfaceId id) { return id > 0 && id < document.nextId && ids.insert(id).second; };
    if (document.nextId == 0) return fail("配置データの次IDが不正です");
    std::unordered_map<SurfaceId, const SurfacePreset*> presets;
    for (const auto& preset : document.presets) {
        if (!idValid(preset.id) || preset.version != 1 || preset.name.empty() || static_cast<uint32_t>(preset.role) > 2)
            return fail("プリセットのID・版・名前・役割が不正です");
        if (!range(preset.displacementMeters, 0, 10) || preset.section.size() < 2 || preset.materials.empty() || preset.materials.size() > 4)
            return fail("プリセットの断面・材質数・変位量が不正です");
        for (size_t i = 0; i < preset.section.size(); ++i) {
            const auto& point = preset.section[i];
            if (!idValid(point.id) || !range(point.across, 0, 100) || !range(point.height, -100, 100))
                return fail("断面点が不正です");
            if (i && (point.across < preset.section[i-1].across ||
                (point.across == preset.section[i-1].across && point.height == preset.section[i-1].height)))
                return fail("断面は横方向の順に並べ、同じ点を連続させないでください");
        }
        for (const auto& boundary : preset.boundaries)
            if (static_cast<uint32_t>(boundary.mode) > 2 || !range(boundary.transitionMeters, 0, 50) ||
                !range(boundary.maxHeightAdjustment, 0, 100)) return fail("境界条件が不正です");
        if (!range(preset.layerBlendRange, 0, 1)) return fail("材質のハイト合成幅が不正です");
        for (const auto& material : preset.materials)
            if (!ValidatePresetMaterial(material, error)) return false;
        if (preset.materialGraph && !ValidatePresetGraph(*preset.materialGraph, error)) return false;
        if (preset.materials.front().mask || preset.materials.front().heightGate || preset.materials.front().blendMode)
            return fail("下地スロットにはマスク・高さ条件・混ぜ方を指定できません");
        for (const auto& parameter : preset.parameters)
            if (!idValid(parameter.id) || parameter.name.empty() || !finite(parameter.minimum) || !finite(parameter.maximum) ||
                parameter.minimum > parameter.maximum || !range(parameter.defaultValue, parameter.minimum, parameter.maximum))
                return fail("公開パラメータが不正です");
        presets.emplace(preset.id, &preset);
    }
    std::unordered_set<int32_t> roads;
    for (const auto& layout : document.layouts) {
        if (!idValid(layout.id) || layout.roadNode <= 0 || !roads.insert(layout.roadNode).second) return fail("道路配置のID・接続先が不正または重複しています");
        size_t roadBands = 0;
        for (const auto& band : layout.bands) {
            if (!idValid(band.id) || static_cast<uint32_t>(band.side) > 2) return fail("帯のID・側が不正です");
            roadBands += band.side == SurfaceSide::Road ? 1 : 0;
            float previousEnd = 0;
            for (const auto& span : band.spans) {
                const auto found = presets.find(span.preset);
                if (!idValid(span.id) || found == presets.end()) return fail("区間のID・プリセット参照が不正です");
                if (!range(span.startMeters, 0, 50) || !range(span.endMeters, 0, 50) || span.startMeters >= span.endMeters ||
                    span.startMeters < previousEnd || !range(span.blendInMeters, 0, span.endMeters - span.startMeters) ||
                    !range(span.blendOutMeters, 0, span.endMeters - span.startMeters)) return fail("区間の範囲・並び・移行距離が不正です");
                previousEnd = span.endMeters;
                if ((band.side == SurfaceSide::Road) != (found->second->role == SurfaceRole::Road)) return fail("区間とプリセットの役割が一致しません");
                std::unordered_set<SurfaceId> used;
                for (const auto& value : span.parameters) {
                    const auto& definitions = found->second->parameters;
                    const auto parameter = std::find_if(definitions.begin(), definitions.end(), [&](const auto& p) { return p.id == value.parameter; });
                    if (parameter == definitions.end() || !used.insert(value.parameter).second ||
                        !range(value.startValue, parameter->minimum, parameter->maximum) || !range(value.endValue, parameter->minimum, parameter->maximum))
                        return fail("区間の公開値参照・始終端値が不正です");
                }
            }
        }
        if (roadBands != 1) return fail("道路配置には道路本体の帯が一つ必要です");
    }
    return true;
}
}  // namespace tg::graph
