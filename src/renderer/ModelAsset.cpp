#include "renderer/ModelAsset.h"

#include "core/PathUtf8.h"

#include <ufbx.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <unordered_map>

namespace tg::renderer {
namespace {

using namespace DirectX;
namespace fs = std::filesystem;

std::string ToString(const ufbx_string& value) { return std::string(value.data, value.length); }

// FBX のテクスチャの実ファイルを探す。書かれた絶対パスは作者の PC のものであることが多いので、
// FBX からの相対パス → 絶対パス → FBX と同じフォルダの同名の順に試す。
fs::path FindTexture(const ufbx_texture* texture, const fs::path& modelDirectory) {
    if (texture == nullptr) return {};
    std::vector<fs::path> candidates;
    if (texture->relative_filename.length > 0) {
        const fs::path relative = FromUtf8(ToString(texture->relative_filename));
        candidates.push_back(relative.is_absolute() ? relative : modelDirectory / relative);
    }
    for (const ufbx_string* text : {&texture->absolute_filename, &texture->filename}) {
        if (text->length == 0) continue;
        const fs::path path = FromUtf8(ToString(*text));
        candidates.push_back(path);
        candidates.push_back(modelDirectory / path.filename());
    }
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (fs::is_regular_file(candidate, error)) return candidate.lexically_normal();
    }
    return {};
}

ModelSlotSource ReadSlotSource(const ufbx_material* material, const fs::path& modelDirectory) {
    ModelSlotSource slot;
    if (material == nullptr) {
        slot.name = "Default";
        return slot;
    }
    slot.name = ToString(material->name);
    const ufbx_material_map& diffuse = material->fbx.diffuse_color;
    slot.baseColorTexture = FindTexture(diffuse.texture, modelDirectory);
    if (slot.baseColorTexture.empty()) {
        slot.baseColorTexture = FindTexture(material->pbr.base_color.texture, modelDirectory);
    }
    if (diffuse.has_value) {
        slot.baseColor = {float(diffuse.value_vec3.x), float(diffuse.value_vec3.y), float(diffuse.value_vec3.z)};
    }
    if (material->pbr.opacity.has_value) {
        slot.opacity = std::clamp(float(material->pbr.opacity.value_real), 0.0f, 1.0f);
    }
    return slot;
}

}  // namespace

XMMATRIX ModelPivotMatrix(const ModelAsset& model) {
    if (!model.geometry) return XMMatrixIdentity();
    const auto& lo = model.geometry->minimum;
    const auto& hi = model.geometry->maximum;
    return XMMatrixTranslation(-(lo.x + hi.x) * 0.5f, -lo.y, -(lo.z + hi.z) * 0.5f) *
           XMMatrixScaling(model.scale, model.scale, model.scale);
}

XMMATRIX NodeTransformMatrix(const float position[3], const float rotationDegrees[3], float scale) {
    return XMMatrixScaling(scale, scale, scale) *
           XMMatrixRotationRollPitchYaw(XMConvertToRadians(rotationDegrees[0]), XMConvertToRadians(rotationDegrees[1]),
                                        XMConvertToRadians(rotationDegrees[2])) *
           XMMatrixTranslation(position[0], position[1], position[2]);
}

void RotationToDegrees(FXMMATRIX rotation, float degrees[3]) {
    // RollPitchYaw は Rz * Rx * Ry（行ベクトル）。3 行目が Rx * Ry の 3 行目そのものなので、
    // そこから X（pitch）と Y（yaw）、1・2 行目の 2 列目から Z（roll）を読む。
    XMFLOAT3X3 m;
    XMStoreFloat3x3(&m, rotation);
    const float pitch = std::asin(std::clamp(-m._32, -1.0f, 1.0f));
    float yaw = 0.0f, roll = 0.0f;
    if (std::abs(std::cos(pitch)) > 1e-4f) {
        yaw = std::atan2(m._31, m._33);
        roll = std::atan2(m._12, m._22);
    } else {
        // 真上・真下を向いたとき（ジンバルロック）は Z を 0 にして Y へ寄せる。
        yaw = std::atan2(-m._13, m._11);
    }
    degrees[0] = XMConvertToDegrees(pitch);
    degrees[1] = XMConvertToDegrees(yaw);
    degrees[2] = XMConvertToDegrees(roll);
}

bool ModelWorldBounds(const ModelAsset& model, FXMMATRIX world, BoundingBox& bounds) {
    if (!model.geometry) return false;
    BoundingBox local;
    BoundingBox::CreateFromPoints(local, XMLoadFloat3(&model.geometry->minimum), XMLoadFloat3(&model.geometry->maximum));
    local.Transform(bounds, world);
    return true;
}

bool LoadModel(const fs::path& path, ModelAsset& asset) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        asset.error = "モデルファイルを開けません";
        return false;
    }
    const auto size = stream.tellg();
    if (size <= 0) {
        asset.error = "モデルファイルが空です";
        return false;
    }
    std::vector<char> bytes(static_cast<size_t>(size));
    stream.seekg(0);
    if (!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        asset.error = "モデルファイルを読み取れません";
        return false;
    }
    ufbx_load_opts opts{};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0;
    opts.generate_missing_normals = true;
    opts.ignore_animation = true;
    opts.ignore_embedded = true;
    ufbx_error error{};
    std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> scene(
        ufbx_load_memory(bytes.data(), bytes.size(), &opts, &error), &ufbx_free_scene);
    if (!scene) {
        asset.error.assign(error.description.data, error.description.length);
        return false;
    }
    const fs::path modelDirectory = path.parent_path();
    auto geometry = std::make_shared<ModelGeometry>();
    const float limit = std::numeric_limits<float>::max();
    geometry->minimum = {limit, limit, limit};
    geometry->maximum = {-limit, -limit, -limit};
    // FBX のマテリアル（typed_id）→ スロット番号。同じ名前でも別のマテリアルは別のスロットにする。
    std::unordered_map<uint32_t, uint32_t> slotIds;
    for (ufbx_node* node : scene->nodes) {
        if (!node->mesh) continue;
        size_t lod = 0;
        for (ufbx_node* child = node; child->parent; child = child->parent) {
            if (child->parent->attrib_type == UFBX_ELEMENT_LOD_GROUP) {
                auto children = child->parent->children;
                for (size_t i = 0; i < children.count; ++i)
                    if (children.data[i] == child) lod = i;
                break;
            }
        }
        geometry->lods.resize(std::max(geometry->lods.size(), lod + 1));
        auto& level = geometry->lods[lod];
        const ufbx_mesh& mesh = *node->mesh;
        const auto normalMatrix = ufbx_matrix_for_normals(&node->geometry_to_world);
        std::vector<uint32_t> indices(mesh.max_face_triangles * 3);
        std::unordered_map<uint32_t, size_t> parts;
        for (size_t faceIndex = 0; faceIndex < mesh.faces.count; ++faceIndex) {
            const auto face = mesh.faces.data[faceIndex];
            const uint32_t count = ufbx_triangulate_face(indices.data(), indices.size(), &mesh, face);
            if (!count) continue;
            const uint32_t mat = mesh.face_material.count ? mesh.face_material.data[faceIndex] : 0;
            const ufbx_material* material = mat < node->materials.count ? node->materials.data[mat] : nullptr;
            const uint32_t materialId = material ? material->typed_id : std::numeric_limits<uint32_t>::max();
            auto [slot, added] = slotIds.emplace(materialId, static_cast<uint32_t>(geometry->slots.size()));
            if (added) geometry->slots.push_back(ReadSlotSource(material, modelDirectory));
            auto [part, newPart] = parts.emplace(slot->second, level.parts.size());
            if (newPart) level.parts.push_back({{}, slot->second});
            auto& dst = level.parts[part->second].mesh;
            for (uint32_t t = 0; t < count; ++t) {
                MeshVertex vertices[3]{};
                for (uint32_t j = 0; j < 3; ++j) {
                    const uint32_t index = indices[t * 3 + j];
                    const auto p = ufbx_transform_position(&node->geometry_to_world,
                                                           ufbx_get_vertex_vec3(&mesh.vertex_position, index));
                    const auto n = ufbx_transform_direction(&normalMatrix,
                                                            ufbx_get_vertex_vec3(&mesh.vertex_normal, index));
                    auto& v = vertices[j];
                    v.position = {float(p.x), float(p.y), float(p.z)};
                    v.normal = {float(n.x), float(n.y), float(n.z)};
                    XMStoreFloat3(&v.normal, XMVector3Normalize(XMLoadFloat3(&v.normal)));
                    if (mesh.vertex_uv.exists) {
                        // FBX の V は上向き。画像の行（下向き）へ揃える。
                        const auto uv = ufbx_get_vertex_vec2(&mesh.vertex_uv, index);
                        v.uv = {float(uv.x), float(1.0 - uv.y)};
                    }
                    // 道路 UV の枠には 2 つ目の UV（ライトマップ用など）を入れる。無ければ uv を写す（MeshVertex の規約）。
                    // マテリアルのマップごとの UV の選択（MaterialAsset::mapUvSets）で読み分ける。
                    if (mesh.uv_sets.count > 1 && mesh.uv_sets.data[1].vertex_uv.exists) {
                        const auto uv2 = ufbx_get_vertex_vec2(&mesh.uv_sets.data[1].vertex_uv, index);
                        v.roadUv = {float(uv2.x), float(1.0 - uv2.y)};
                    } else {
                        v.roadUv = v.uv;
                    }
                    for (int k = 0; k < 3; ++k) {
                        const float value = (&v.position.x)[k];
                        if (!std::isfinite(value)) {
                            asset.error = "頂点座標が不正です";
                            return false;
                        }
                        (&geometry->minimum.x)[k] = std::min((&geometry->minimum.x)[k], value);
                        (&geometry->maximum.x)[k] = std::max((&geometry->maximum.x)[k], value);
                    }
                }
                // 負のスケールも含め、面の向きを変換後の法線へ合わせる。
                auto e1 = XMVectorSubtract(XMLoadFloat3(&vertices[1].position), XMLoadFloat3(&vertices[0].position));
                auto e2 = XMVectorSubtract(XMLoadFloat3(&vertices[2].position), XMLoadFloat3(&vertices[0].position));
                if (XMVectorGetX(XMVector3Dot(XMVector3Cross(e1, e2), XMLoadFloat3(&vertices[0].normal))) < 0) {
                    std::swap(vertices[1], vertices[2]);
                    std::swap(e1, e2);
                }
                const float du1 = vertices[1].uv.x - vertices[0].uv.x, dv1 = vertices[1].uv.y - vertices[0].uv.y;
                const float du2 = vertices[2].uv.x - vertices[0].uv.x, dv2 = vertices[2].uv.y - vertices[0].uv.y;
                const float det = du1 * dv2 - du2 * dv1;
                auto tangent = e1, bitangent = e2;
                if (std::abs(det) > 1e-12f) {
                    tangent = XMVectorScale(XMVectorSubtract(XMVectorScale(e1, dv2), XMVectorScale(e2, dv1)), 1 / det);
                    bitangent = XMVectorScale(XMVectorSubtract(XMVectorScale(e2, du1), XMVectorScale(e1, du2)), 1 / det);
                }
                for (auto& v : vertices) {
                    auto n = XMLoadFloat3(&v.normal);
                    auto tx = XMVectorSubtract(tangent, XMVectorMultiply(n, XMVector3Dot(n, tangent)));
                    if (XMVectorGetX(XMVector3LengthSq(tx)) < 1e-20f) {
                        const auto axis = std::abs(v.normal.y) < 0.9f ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(1, 0, 0, 0);
                        tx = XMVector3Cross(axis, n);
                    }
                    tx = XMVector3Normalize(tx);
                    XMStoreFloat4(&v.tangent, tx);
                    v.tangent.w = XMVectorGetX(XMVector3Dot(XMVector3Cross(n, tx), bitangent)) < 0 ? -1.0f : 1.0f;
                    dst.indices.push_back(static_cast<uint32_t>(dst.vertices.size()));
                    dst.vertices.push_back(v);
                }
            }
            level.triangles += count;
        }
    }
    if (geometry->lods.empty() || !geometry->lods[0].triangles) {
        asset.error = "表示できるメッシュがありません";
        return false;
    }
    asset.path = path;
    asset.geometry = std::move(geometry);
    asset.materials.resize(asset.geometry->slots.size(), compositor::kNoMaterialAsset);
    asset.error.clear();
    return true;
}

}  // namespace tg::renderer
