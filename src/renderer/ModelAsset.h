#pragma once

#include "compositor/MaterialLayer.h"
#include "renderer/MeshData.h"

#include <DirectXCollision.h>
#include <DirectXMath.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// FBX から読んだ 3D モデル。terrain-graph の ModelAsset を移植したもの（配置ノードは持たない）。
// 仕様は docs/reference/model-assets.md。
namespace tg::renderer {

// マテリアルスロット 1 つぶんの、FBX のマテリアルに書かれていた情報。
// 「FBX のマテリアルから作成」でマテリアルアセットの初期値に使う。
struct ModelSlotSource {
    std::string name;
    // 拡散色のテクスチャ。FBX の相対パス → 絶対パス → FBX と同じフォルダの同名の順に探し、
    // 見つかったものだけを入れる。無ければ空。
    std::filesystem::path baseColorTexture;
    DirectX::XMFLOAT3 baseColor{1.0f, 1.0f, 1.0f};
    float opacity = 1.0f;
};

struct ModelPart {
    MeshData mesh;
    uint32_t slot = 0;
};

struct ModelLod {
    std::vector<ModelPart> parts;
    uint32_t triangles = 0;
};

struct ModelGeometry {
    std::vector<ModelLod> lods;
    std::vector<ModelSlotSource> slots;
    DirectX::XMFLOAT3 minimum{}, maximum{};
};

// CPU 形状は不変・共有。履歴へ頂点配列を複製しない。
// GPU リソースを持たないので、アンドゥのスナップショットへそのまま複製できる。
struct ModelAsset {
    uint64_t id = 0;
    // 共有アセット（.tgmodel）の置き場所と永続 ID。未保存なら空。
    std::filesystem::path assetPath;
    std::string assetUid;
    std::string name;
    // 元の FBX。
    std::filesystem::path path;
    std::shared_ptr<const ModelGeometry> geometry;
    // スロットごとのマテリアル。未割り当ては kNoMaterialAsset（灰色で描く）。
    std::vector<compositor::MaterialAssetId> materials;
    // FBX の座標に掛ける倍率。単位の宣言と中身が食い違う FBX（cm と宣言して m で作ったもの）を
    // 実寸へ直すためのもの。形状（geometry）には焼き込まず、寸法の表示とシーンへの配置で掛ける。
    float scale = 1.0f;
    std::string error;
};

// シーン（ビューポート）に置いたモデル 1 つ。position はモデルの底面の中心（形状の境界ボックスの
// X・Z の中央、Y の最小）を置く位置（m）。回転は Y 軸まわり（度）、倍率はモデルの倍率に掛ける。
struct ModelInstance {
    uint64_t id = 0;
    uint64_t model = 0;
    DirectX::XMFLOAT3 position{};
    float rotationDegrees = 0.0f;
    float scale = 1.0f;
};

// 置いたモデルのワールド行列（DirectXMath の行ベクトル規約）。形状が無ければ単位行列。
DirectX::XMMATRIX ModelInstanceWorld(const ModelAsset& model, const ModelInstance& instance);
// 置いたモデルのワールド空間の境界ボックス。形状が無ければ偽。
bool ModelInstanceBounds(const ModelAsset& model, const ModelInstance& instance, DirectX::BoundingBox& bounds);

// FBX を読み、右手系 Y-up・メートルへ変換する。失敗したら asset.error に理由を入れて偽を返す
// （geometry と materials は変えない）。
bool LoadModel(const std::filesystem::path& path, ModelAsset& asset);

}  // namespace tg::renderer
