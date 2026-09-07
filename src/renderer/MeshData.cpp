#include "renderer/MeshData.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace tg::renderer {

bool ValidateMeshScene(const MeshScene& scene) {
    for (const auto& mesh : scene.meshes) {
        const auto& data = mesh.geometry;
        if (mesh.materialOnly) {
            if (!data.vertices.empty() || !data.indices.empty() || !mesh.materialStack) return false;
            continue;
        }
        for (int source : mesh.connectionSources) {
            if (source != -1 && (source < 0 || static_cast<size_t>(source) >= scene.meshes.size() ||
                !scene.meshes[static_cast<size_t>(source)].materialOnly)) return false;
        }
        const bool hasContexts = mesh.connectionSources[0] >= 0;
        for (size_t i = 0; i < mesh.connectionSources.size(); ++i) {
            if ((mesh.connectionSources[i] >= 0) != hasContexts ||
                !std::isfinite(mesh.connectionOrigins[i].x) || !std::isfinite(mesh.connectionOrigins[i].y)) return false;
        }
        if (data.vertices.empty() || data.indices.empty() || data.indices.size() % 3 != 0 ||
            data.vertices.size() > std::numeric_limits<uint32_t>::max() / sizeof(MeshVertex) ||
            data.indices.size() > std::numeric_limits<uint32_t>::max() / sizeof(uint32_t)) {
            return false;
        }
        const auto unitRange = [](float value) {
            return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
        };
        const auto& material = mesh.material;
        if (!unitRange(material.baseColor.x) || !unitRange(material.baseColor.y) ||
            !unitRange(material.baseColor.z) || !unitRange(material.roughness) ||
            !unitRange(material.metallic)) {
            return false;
        }
        for (const auto& v : data.vertices) {
            const float values[] = {v.position.x, v.position.y, v.position.z,
                v.normal.x, v.normal.y, v.normal.z, v.tangent.x, v.tangent.y,
                v.tangent.z, v.tangent.w, v.uv.x, v.uv.y, v.roadUv.x, v.roadUv.y};
            for (float value : values) {
                if (!std::isfinite(value)) return false;
            }
            using namespace DirectX;
            const float normalLength = XMVectorGetX(XMVector3Length(XMLoadFloat3(&v.normal)));
            const float tangentLength = XMVectorGetX(XMVector3Length(XMLoadFloat4(&v.tangent)));
            const float dot = XMVectorGetX(XMVector3Dot(XMLoadFloat3(&v.normal), XMLoadFloat4(&v.tangent)));
            if (std::abs(normalLength - 1.0f) > 0.01f ||
                std::abs(tangentLength - 1.0f) > 0.01f || std::abs(dot) > 0.01f ||
                std::abs(std::abs(v.tangent.w) - 1.0f) > 0.001f) return false;
        }
        for (uint32_t index : data.indices) {
            if (index >= data.vertices.size()) return false;
        }
        for (const auto& seam : mesh.connectionSeams)
            for (uint32_t index : seam) if (index >= data.vertices.size()) return false;
    }
    return std::isfinite(MeshSceneRadius(scene));
}

float MeshSceneRadius(const MeshScene& scene) {
    float radius = 0.1f;
    for (const auto& mesh : scene.meshes) {
        for (const auto& vertex : mesh.geometry.vertices) {
            const auto& p = vertex.position;
            radius = std::max(radius, std::hypot(p.x, p.y, p.z));
        }
    }
    return radius;
}

}  // namespace tg::renderer
