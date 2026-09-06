#pragma once

#include "renderer/MeshData.h"
#include <nlohmann/json.hpp>

namespace tg::io {

// 不正な入力は false。失敗時に出力を変更しない。
bool ReadMeshScene(const nlohmann::json& node, renderer::MeshScene& scene);
nlohmann::json WriteMeshScene(const renderer::MeshScene& scene);

}  // namespace tg::io
