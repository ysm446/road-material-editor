#pragma once

#include "compositor/MaterialStack.h"
#include <optional>
#include <DirectXMath.h>
#include <cstdint>
#include <vector>

namespace tg::renderer {

// CPU 側の生成結果。座標は右手系 Y-up、メートル。GPU の所有権を持たない。
struct MeshVertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT4 tangent;
    DirectX::XMFLOAT2 uv;
};

struct MeshData {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
};

struct MaterialSettings {
    DirectX::XMFLOAT3 baseColor = {0.18f, 0.18f, 0.18f};
    float roughness = 0.35f;
    float metallic = 0.0f;
};

struct SceneMesh {
    MeshData geometry;
    MaterialSettings material;
    // 道路の表示用メタデータ。0は道路以外。生成時に再構築する。
    float roadMetersPerUv = 0.0f;
    // 道路の1 mグリッドを重ねるか。白線などの帯は道路面の目盛りを持たない。
    bool roadGridOverlay = true;
    // 接続から導出した材質。GPU参照や保存対象ではない。
    std::optional<compositor::MaterialStack> materialStack;
};

struct MeshScene {
    std::vector<SceneMesh> meshes;
};

// 不正なインデックス、非有限値、縮退した接空間は GPU へ渡さない。
bool ValidateMeshScene(const MeshScene& scene);
// 原点中心の包囲球。カメラとシャドウの既存規約に合わせる。
float MeshSceneRadius(const MeshScene& scene);

}  // namespace tg::renderer
