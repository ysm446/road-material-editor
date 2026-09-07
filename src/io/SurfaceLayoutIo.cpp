#include "io/SurfaceLayoutIo.h"
#include <nlohmann/json.hpp>
#include <limits>

namespace tg::io {
using nlohmann::json;
namespace {
struct Reader {
    bool valid = true;
    const json& Field(const json& object, const char* key) {
        static const json missing;
        if (!object.is_object() || !object.contains(key)) { valid = false; return missing; }
        return object[key];
    }
    const json& Array(const json& object, const char* key) {
        static const json empty = json::array();
        const auto& value = Field(object, key);
        if (!value.is_array()) { valid = false; return empty; }
        return value;
    }
    uint32_t UInt(const json& object, const char* key) {
        const auto& value = Field(object, key);
        if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0)) { valid = false; return 0; }
        const auto number = value.get<uint64_t>();
        if (number > std::numeric_limits<uint32_t>::max()) { valid = false; return 0; }
        return static_cast<uint32_t>(number);
    }
    float Float(const json& object, const char* key) {
        const auto& value = Field(object, key);
        if (!value.is_number()) { valid = false; return 0; }
        return value.get<float>();
    }
    bool Bool(const json& object, const char* key) {
        const auto& value = Field(object, key);
        if (!value.is_boolean()) { valid = false; return false; }
        return value.get<bool>();
    }
    std::string String(const json& object, const char* key) {
        const auto& value = Field(object, key);
        if (!value.is_string()) { valid = false; return {}; }
        return value.get<std::string>();
    }
};
json WritePresetMaterial(const graph::PresetMaterial& material) {
    json m = {{"material", material.material}, {"uvRepeat", material.uvRepeatMeters},
        {"worldUv", material.worldUv}, {"baseColor", material.baseColor}, {"roughness", material.roughness},
        {"metallic", material.metallic}, {"ambientOcclusion", material.ambientOcclusion},
        {"blendMode", material.blendMode}, {"heightGate", material.heightGate},
        {"heightGateThreshold", material.heightGateThreshold}, {"heightGateSoftness", material.heightGateSoftness}, {"mask", nullptr}};
    if (material.mask) {
        const auto& mask = *material.mask;
        m["mask"] = {{"shape", static_cast<uint32_t>(mask.shape)}, {"edgeSide", static_cast<uint32_t>(mask.edgeSide)},
            {"laneOffset", mask.laneOffsetMeters}, {"trackSpacing", mask.trackSpacingMeters}, {"trackWidth", mask.trackWidthMeters},
            {"feather", mask.featherMeters}, {"tracksFromLanes", mask.tracksFromLanes}, {"bothLanes", mask.bothLanes},
            {"edgeWidth", mask.edgeWidthMeters}, {"noiseScale", mask.noiseScaleMeters}, {"threshold", mask.threshold},
            {"softness", mask.softness}, {"seed", mask.seed}, {"breakupAmount", mask.breakupAmount},
            {"breakupScale", mask.breakupScaleMeters}, {"strength", mask.strength}, {"invert", mask.invert}};
    }
    if (!material.enabled) m["enabled"] = false;
    return m;
}

graph::PresetMaterial ReadPresetMaterial(Reader& r, const json& m, uint32_t version) {
    graph::PresetMaterial material;
    if (m.contains("enabled")) material.enabled = r.Bool(m, "enabled");
    material.material = r.UInt(m, "material"); material.uvRepeatMeters = r.Float(m, "uvRepeat"); material.worldUv = r.Bool(m, "worldUv");
    material.roughness = r.Float(m, "roughness");
    const auto& color = r.Array(m, "baseColor");
    if (color.size() != 3) r.valid = false;
    for (size_t i = 0; i < color.size() && i < 3; ++i) {
        if (!color[i].is_number()) r.valid = false;
        else material.baseColor[i] = color[i].get<float>();
    }
    if (version >= 2) {
        material.metallic = r.Float(m, "metallic"); material.ambientOcclusion = r.Float(m, "ambientOcclusion");
        material.blendMode = r.UInt(m, "blendMode"); material.heightGate = r.UInt(m, "heightGate");
        material.heightGateThreshold = r.Float(m, "heightGateThreshold"); material.heightGateSoftness = r.Float(m, "heightGateSoftness");
        const auto& mask = r.Field(m, "mask");
        if (!mask.is_null()) {
            graph::RoadMaskNodeSettings settings;
            settings.shape = static_cast<graph::RoadMaskShape>(r.UInt(mask, "shape"));
            settings.edgeSide = static_cast<graph::RoadMaskSide>(r.UInt(mask, "edgeSide"));
            settings.laneOffsetMeters = r.Float(mask, "laneOffset"); settings.trackSpacingMeters = r.Float(mask, "trackSpacing");
            settings.trackWidthMeters = r.Float(mask, "trackWidth"); settings.featherMeters = r.Float(mask, "feather");
            settings.tracksFromLanes = r.Bool(mask, "tracksFromLanes"); settings.bothLanes = r.Bool(mask, "bothLanes");
            settings.edgeWidthMeters = r.Float(mask, "edgeWidth"); settings.noiseScaleMeters = r.Float(mask, "noiseScale");
            settings.threshold = r.Float(mask, "threshold"); settings.softness = r.Float(mask, "softness"); settings.seed = r.UInt(mask, "seed");
            settings.breakupAmount = r.Float(mask, "breakupAmount"); settings.breakupScaleMeters = r.Float(mask, "breakupScale");
            settings.strength = r.Float(mask, "strength"); settings.invert = r.Bool(mask, "invert");
            material.mask = settings;
        }
    }
    return material;
}

}

json WriteSurfaceLayouts(const graph::SurfaceLayoutDocument& document) {
    json result = {{"version", 2}, {"nextId", document.nextId}, {"presets", json::array()}, {"layouts", json::array()}};
    for (const auto& preset : document.presets) {
        if (preset.materialGraph) result["version"] = 3;
        json p = {{"id", preset.id}, {"version", preset.version}, {"name", preset.name}, {"role", static_cast<uint32_t>(preset.role)},
                  {"displacement", preset.displacementMeters}, {"layerBlendRange", preset.layerBlendRange}, {"section", json::array()}, {"boundaries", json::array()},
                  {"materials", json::array()}, {"parameters", json::array()}};
        for (const auto& point : preset.section) p["section"].push_back({{"id", point.id}, {"across", point.across}, {"height", point.height}});
        for (const auto& boundary : preset.boundaries) p["boundaries"].push_back({{"mode", static_cast<uint32_t>(boundary.mode)},
            {"transition", boundary.transitionMeters}, {"maxHeightAdjustment", boundary.maxHeightAdjustment}, {"preserveOutline", boundary.preserveOutline}});
        for (const auto& material : preset.materials) {
            p["materials"].push_back(WritePresetMaterial(material));
        }
        if (preset.materialGraph) {
            json g = {{"nextId", preset.materialGraph->nextId}, {"nodes", json::array()}};
            for (const auto& node : preset.materialGraph->nodes)
                g["nodes"].push_back({{"id", node.id}, {"kind", static_cast<uint32_t>(node.kind)},
                    {"inputs", node.inputs}, {"position", node.position}, {"settings", WritePresetMaterial(node.settings)}});
            p["materialGraph"] = std::move(g);
        }
        for (const auto& parameter : preset.parameters) p["parameters"].push_back({{"id", parameter.id}, {"name", parameter.name},
            {"minimum", parameter.minimum}, {"maximum", parameter.maximum}, {"default", parameter.defaultValue}});
        result["presets"].push_back(std::move(p));
    }
    for (const auto& layout : document.layouts) {
        json l = {{"id", layout.id}, {"roadNode", layout.roadNode}, {"bands", json::array()}};
        for (const auto& band : layout.bands) {
            json b = {{"id", band.id}, {"side", static_cast<uint32_t>(band.side)}, {"spans", json::array()}};
            for (const auto& span : band.spans) {
                json s = {{"id", span.id}, {"preset", span.preset}, {"start", span.startMeters}, {"end", span.endMeters},
                    {"blendIn", span.blendInMeters}, {"blendOut", span.blendOutMeters}, {"seed", span.seed}, {"parameters", json::array()}};
                for (const auto& value : span.parameters) s["parameters"].push_back({{"parameter", value.parameter}, {"start", value.startValue}, {"end", value.endValue}});
                b["spans"].push_back(std::move(s));
            }
            l["bands"].push_back(std::move(b));
        }
        result["layouts"].push_back(std::move(l));
    }
    return result;
}

bool ReadSurfaceLayouts(const json& value, graph::SurfaceLayoutDocument& document, std::string& error) {
    Reader r;
    graph::SurfaceLayoutDocument parsed;
    const auto version = r.UInt(value, "version");
    if (version != 1 && version != 2 && version != 3) { error = "未対応の配置データ版です"; return false; }
    parsed.nextId = r.UInt(value, "nextId");
    for (const auto& p : r.Array(value, "presets")) {
        graph::SurfacePreset preset;
        preset.id = r.UInt(p, "id"); preset.version = r.UInt(p, "version"); preset.name = r.String(p, "name");
        preset.role = static_cast<graph::SurfaceRole>(r.UInt(p, "role"));
        preset.displacementMeters = r.Float(p, "displacement");
        if (version >= 2) preset.layerBlendRange = r.Float(p, "layerBlendRange");
        for (const auto& point : r.Array(p, "section")) preset.section.push_back({r.UInt(point, "id"), r.Float(point, "across"), r.Float(point, "height")});
        const auto& boundaries = r.Array(p, "boundaries");
        if (boundaries.size() != 4) r.valid = false;
        for (size_t i = 0; i < boundaries.size() && i < 4; ++i) {
            const auto& b = boundaries[i];
            preset.boundaries[i] = {static_cast<graph::BoundaryMode>(r.UInt(b, "mode")), r.Float(b, "transition"),
                                   r.Float(b, "maxHeightAdjustment"), r.Bool(b, "preserveOutline")};
        }
        for (const auto& m : r.Array(p, "materials")) {
            preset.materials.push_back(ReadPresetMaterial(r, m, version));
        }
        if (p.contains("materialGraph")) {
            if (version < 3) { error = "グラフ付きプリセットには配置データ版3が必要です"; return false; }
            const auto& g = r.Field(p, "materialGraph");
            preset.materialGraph.emplace();
            preset.materialGraph->nextId = r.UInt(g, "nextId");
            for (const auto& n : r.Array(g, "nodes")) {
                graph::PresetNode node; node.id = r.UInt(n, "id");
                node.kind = static_cast<graph::PresetNodeKind>(r.UInt(n, "kind"));
                const auto& inputs = r.Array(n, "inputs");
                const auto& position = r.Array(n, "position");
                if (inputs.size() != 3 || position.size() != 2) r.valid = false;
                for (size_t i = 0; i < inputs.size() && i < 3; ++i) {
                    const json item = {{"input", inputs[i]}};
                    node.inputs[i] = r.UInt(item, "input");
                }
                for (size_t i = 0; i < position.size() && i < 2; ++i) {
                    if (!position[i].is_number()) r.valid = false;
                    else node.position[i] = position[i].get<float>();
                }
                node.settings = ReadPresetMaterial(r, r.Field(n, "settings"), 2);
                preset.materialGraph->nodes.push_back(node);
            }
        }
        for (const auto& parameter : r.Array(p, "parameters")) preset.parameters.push_back({r.UInt(parameter, "id"), r.String(parameter, "name"),
            r.Float(parameter, "minimum"), r.Float(parameter, "maximum"), r.Float(parameter, "default")});
        parsed.presets.push_back(std::move(preset));
    }
    for (const auto& l : r.Array(value, "layouts")) {
        graph::RoadLayout layout;
        layout.id = r.UInt(l, "id");
        const auto roadNode = r.UInt(l, "roadNode");
        if (roadNode > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) r.valid = false;
        else layout.roadNode = static_cast<int32_t>(roadNode);
        for (const auto& b : r.Array(l, "bands")) {
            graph::SurfaceBand band;
            band.id = r.UInt(b, "id"); band.side = static_cast<graph::SurfaceSide>(r.UInt(b, "side"));
            for (const auto& s : r.Array(b, "spans")) {
                graph::SurfaceSpan span;
                span.id = r.UInt(s, "id"); span.preset = r.UInt(s, "preset"); span.startMeters = r.Float(s, "start"); span.endMeters = r.Float(s, "end");
                span.blendInMeters = r.Float(s, "blendIn"); span.blendOutMeters = r.Float(s, "blendOut"); span.seed = r.UInt(s, "seed");
                for (const auto& parameter : r.Array(s, "parameters")) span.parameters.push_back({r.UInt(parameter, "parameter"), r.Float(parameter, "start"), r.Float(parameter, "end")});
                band.spans.push_back(std::move(span));
            }
            layout.bands.push_back(std::move(band));
        }
        parsed.layouts.push_back(std::move(layout));
    }
    if (!r.valid) { error = "配置データの必須項目・型・値の範囲が不正です"; return false; }
    if (!graph::ValidateSurfaceLayouts(parsed, error)) return false;
    document = std::move(parsed);
    return true;
}
}  // namespace tg::io
