#pragma once

#include "io/ProjectWorkspace.h"

namespace tg::io {

// アセットファイルを退避（削除）する前の参照関係の検査結果。
struct AssetRelations {
    std::filesystem::path target;
    // 対象を参照している文書（シーン・共有アセット・旧プロジェクト）。
    std::vector<std::filesystem::path> referencers;
    // 対象が参照している素材と、シーンのプレビュー画像。削除はしない。
    std::vector<std::filesystem::path> related;
    // 一緒に退避するファイル（`.meta`）。
    std::vector<std::filesystem::path> companions;
    std::vector<std::string> companionVersions;
    std::filesystem::file_time_type modified{};
    uintmax_t size = 0;
    // 走査をすべて読めたか。偽なら削除を許さない。
    bool complete = false;
};

AssetRelations InspectAssetRelations(ProjectWorkspace& workspace, const std::filesystem::path& target);
// 確認時から変わっていない場合だけ、元ファイルと .meta をルート内の退避フォルダへ移す。
bool RetireAsset(ProjectWorkspace& workspace, const AssetRelations& approved);

}  // namespace tg::io
