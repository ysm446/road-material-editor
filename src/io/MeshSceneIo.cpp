#include "io/MeshSceneIo.h"

#include <cmath>
#include <limits>

namespace tg::io {
namespace {
bool ReadFloats(const nlohmann::json& value, float* output, size_t count) {
    if (!value.is_array() || value.size() != count) return false;
    for (size_t i = 0; i < count; ++i) {
        if (!value[i].is_number()) return false;
        const double number = value[i].get<double>();
        if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max()) return false;
        output[i] = static_cast<float>(number);
    }
    return true;
}
}  // namespace

bool ReadMeshScene(const nlohmann::json& node, renderer::MeshScene& scene) {
    if (!node.is_object() || !node.contains("meshes") || !node["meshes"].is_array()) return false;
    renderer::MeshScene result;
    for (const auto& entry : node["meshes"]) {
        if (!entry.is_object() || !entry.contains("vertices") || !entry["vertices"].is_array() ||
            !entry.contains("indices") || !entry["indices"].is_array() ||
            !entry.contains("material")) return false;
        renderer::SceneMesh mesh;
        float material[5];
        if (!ReadFloats(entry["material"], material, 5)) return false;
        mesh.material.baseColor = {material[0], material[1], material[2]};
        mesh.material.roughness = material[3];
        mesh.material.metallic = material[4];
        for (const auto& vertex : entry["vertices"]) {
            float values[12];
            if (!ReadFloats(vertex, values, 12)) return false;
            // 保存形式は 12 値のまま。道路 UV は持たないので uv を写す。
            mesh.geometry.vertices.push_back({{values[0], values[1], values[2]},
                {values[3], values[4], values[5]}, {values[6], values[7], values[8], values[9]},
                {values[10], values[11]}, {values[10], values[11]}});
        }
        for (const auto& index : entry["indices"]) {
            if (!index.is_number_integer()) return false;
            if (index.is_number_unsigned()) {
                if (index.get<uint64_t>() > std::numeric_limits<uint32_t>::max()) return false;
            } else if (index.get<int64_t>() < 0 ||
                       index.get<int64_t>() > std::numeric_limits<uint32_t>::max()) return false;
            mesh.geometry.indices.push_back(index.get<uint32_t>());
        }
        result.meshes.push_back(std::move(mesh));
    }
    if (!renderer::ValidateMeshScene(result)) return false;
    scene = std::move(result);
    return true;
}

nlohmann::json WriteMeshScene(const renderer::MeshScene& scene) {
    auto entries = nlohmann::json::array();
    for (const auto& mesh : scene.meshes) {
        auto vertices = nlohmann::json::array();
        for (const auto& v : mesh.geometry.vertices) {
            vertices.push_back({v.position.x, v.position.y, v.position.z,
                v.normal.x, v.normal.y, v.normal.z, v.tangent.x, v.tangent.y,
                v.tangent.z, v.tangent.w, v.uv.x, v.uv.y});
        }
        const auto& m = mesh.material;
        entries.push_back({{"vertices", std::move(vertices)}, {"indices", mesh.geometry.indices},
            {"material", {m.baseColor.x, m.baseColor.y, m.baseColor.z, m.roughness, m.metallic}}});
    }
    return {{"meshes", std::move(entries)}};
}

}  // namespace tg::io
