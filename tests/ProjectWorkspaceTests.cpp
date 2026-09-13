// プロジェクトルートと共有アセットのテスト。GPU を使わない io 層だけを対象にする。
//
//   - 永続 ID と参照の解決（改名・移動しても追える、ID が無ければ付け替えない）
//   - シーンの分離保存と展開（埋め込み文書 → .tgmat / .tgsky / Imported/ → 埋め込み文書）
//   - リンク切れの画像を保存・読み込みで失わない
//   - ルートごとのシーン履歴と旧履歴の移行
//   - サムネイルのディスクキャッシュの有効判定
//   - ファイルの削除（退避）前の参照検査

#include "TestSupport.h"

#include "core/PathUtf8.h"
#include "io/AssetRelations.h"
#include "io/ProjectWorkspace.h"
#include "io/RecentFiles.h"
#include "io/ThumbnailStore.h"

#include <chrono>
#include <fstream>

namespace fs = std::filesystem;
using nlohmann::json;
using tg::io::ProjectWorkspace;

// AppSettings.cpp はテストに入れない（Windows の設定フォルダへ触らせない）。
namespace tg::io {
fs::path AppDataDirectory() { return {}; }
}  // namespace tg::io

namespace {

using tg::tests::Check;
using tg::tests::Section;

fs::path FreshDirectory(const char* name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path directory = fs::temp_directory_path() / ("road-editor-" + std::string(name) + "-" + std::to_string(stamp));
    std::error_code error;
    fs::create_directories(directory, error);
    return directory;
}

void Touch(const fs::path& path, char content = 'x') {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream(path, std::ios::binary).put(content);
}

void TestWorkspace() {
    Section("ProjectWorkspace: 永続 ID と参照");
    const fs::path root = FreshDirectory("workspace");
    ProjectWorkspace workspace;
    Check(workspace.Open(root), "ルートを開く（project.tgproj を作る）");
    Check(fs::exists(root / "project.tgproj"), "目印ファイルができる");
    Check(ProjectWorkspace::IsWorkspaceFile(root / "project.tgproj"), "目印ファイルの判定");
    Check(!workspace.Contains(root / ".." / "outside"), "ルート外（..）は含まない");
    Check(!workspace.Contains(root.wstring() + L"-sibling/file"), "名前が前方一致するだけの隣は含まない");

    const fs::path image = workspace.UniquePath(root, "source", ".png");
    Touch(image);
    const json source = workspace.Reference(image);
    Check(!source.is_null() && fs::exists(fs::path(image.wstring() + L".meta")), "画像の参照で .meta ができる");
    fs::path materialPath = workspace.UniquePath(root, "material", ".tgmat");
    json material = {{"name", "material"}, {"roughness", 0.25}, {"maps", {{"baseColor", source}}}};
    Check(workspace.SaveAsset(materialPath, "material-asset", material), "マテリアルを保存する");
    const json materialRef = workspace.Reference(materialPath);

    json packed = {{"materials", json::array({{{"id", 7}, {"asset", materialRef}}})}};
    Check(workspace.Expand(packed), "共有マテリアルを展開する");
    Check(packed["textures"].size() == 1 && packed["materials"][0]["maps"]["baseColor"] == 1, "画像の参照をシーンの番号へ写す");

    std::error_code error;
    const fs::path moved = workspace.UniquePath(root / "Moved", "renamed", ".tgmat");
    fs::create_directories(moved.parent_path(), error);
    fs::rename(materialPath, moved, error);
    Check(!error && workspace.Scan() && workspace.Resolve(materialRef) == moved, "改名・移動しても ID で追える");
    const fs::path movedImage = workspace.UniquePath(root / "Moved", "source", ".png");
    fs::rename(image, movedImage, error);
    fs::rename(fs::path(image.wstring() + L".meta"), fs::path(movedImage.wstring() + L".meta"), error);
    Check(!error && workspace.Scan() && workspace.Resolve(source) == movedImage, ".meta と一緒に動かした画像を追える");

    json changed;
    Check(workspace.ReadAsset(moved, "material-asset", changed), "改名したマテリアルを読む");
    changed["roughness"] = 0.75;
    fs::path target = moved;
    Check(workspace.SaveAsset(target, "material-asset", changed), "共有マテリアルを更新する");
    json second = {{"materials", json::array({{{"id", 2}, {"asset", materialRef}}})}};
    Check(workspace.Expand(second) && second["materials"][0]["roughness"] == 0.75, "別のシーンから編集後の値が見える");

    Section("ProjectWorkspace: シーンの分離保存と展開");
    const fs::path outside = FreshDirectory("outside") / "external.png";
    Touch(outside);
    const fs::path missing = root / "gone.png";
    const fs::path scene = workspace.UniquePath(root / "Scenes", "scene", ".tgscene");
    json legacy = {{"version", 26},
                   {"textures", json::array({{{"id", 1}, {"name", "a"}, {"path", tg::ToUtf8Portable(movedImage)}},
                                             {{"id", 2}, {"name", "b"}, {"path", tg::ToUtf8Portable(outside)}},
                                             {{"id", 3}, {"name", "c"}, {"path", tg::ToUtf8Portable(missing)}}})},
                   {"materials", json::array({{{"id", 1}, {"name", "embedded"}, {"maps", {{"baseColor", 2}}}}})},
                   {"skies", json::array({{{"id", 1}, {"name", "sky"}, {"hdri", nullptr}}})},
                   {"graph", {{"nodes", json::array()}}}};
    Check(workspace.SaveScene(scene, legacy), "シーンを保存する");
    Check(fs::exists(root / "Materials" / "embedded.tgmat") && fs::exists(root / "Skies" / "sky.tgsky"), "埋め込みを .tgmat / .tgsky へ分ける");
    Check(fs::exists(root / "Imported" / "external.png"), "ルート外の画像を Imported/ へ取り込む");
    json loaded;
    Check(workspace.ReadScene(scene, loaded), "シーンを読む");
    Check(loaded["version"] == 26, "既存の保存器の版を戻す");
    Check(loaded["textures"][0]["path"] == tg::ToUtf8Portable(movedImage), "ルート内の画像は元の場所のまま");
    Check(loaded["textures"][1]["path"] == tg::ToUtf8Portable(root / "Imported" / "external.png"), "取り込んだ画像を指す");
    Check(loaded["textures"][2]["path"] == tg::ToUtf8Portable(missing), "リンク切れの画像はパスのまま残る");
    Check(loaded["materials"][0]["maps"]["baseColor"] == 2 && loaded["materials"][0]["name"] == "embedded", "マテリアルの画像参照を番号へ戻す");
    Check(workspace.StartupScene() == scene, "開始シーンを覚える");
    const std::string uid = loaded["sceneUid"];
    json again = loaded;
    Check(workspace.SaveScene(scene, again) && again["sceneUid"] == uid, "同じ保存先はシーン ID を保つ");
    json other = loaded;
    Check(workspace.SaveScene(root / "Scenes" / "copy.tgscene", other) && other["sceneUid"] != uid, "名前を付けて保存は別 ID");

    json broken = {{"materials", json::array({{{"id", 1}, {"asset", {{"uid", "missing"}, {"path", "Moved/renamed.tgmat"}}}}})}};
    Check(!workspace.Expand(broken), "ID が見つからなければ同名へ付け替えず失敗する");
    const fs::path duplicate = workspace.UniquePath(root, "duplicate", ".tgmat");
    fs::copy_file(moved, duplicate, error);
    Check(!workspace.Scan(), "ID の重複を検出する");
    fs::remove(duplicate, error);
    Check(workspace.Scan(), "重複を消せば戻る");
    json malformed = {{"materials", json::array({42})}};
    Check(!workspace.Expand(malformed), "壊れたアセット表を拒否する");

    Section("ProjectWorkspace: ルートごと移動");
    fs::path relocated;
    for (unsigned i = 0;; ++i) {
        relocated = root.parent_path() / ("road-editor-relocated-" + std::to_string(i));
        if (!fs::exists(relocated, error)) break;
    }
    fs::rename(root, relocated, error);
    ProjectWorkspace movedWorkspace;
    Check(!error && movedWorkspace.Open(relocated), "移動したルートを開く");
    json movedScene;
    Check(movedWorkspace.ReadScene(movedWorkspace.StartupScene(), movedScene), "移動後にシーンを読む");
    Check(movedScene["textures"][0]["path"] == tg::ToUtf8Portable(relocated / movedImage.lexically_relative(root)),
          "画像の参照が新しいルートの中を指す");
    fs::remove_all(relocated, error);
    fs::remove_all(outside.parent_path(), error);
}

void TestHistoryAndThumbnails() {
    Section("RecentFiles: ルートごとのシーン履歴");
    const fs::path directory = FreshDirectory("history");
    std::error_code error;
    tg::io::RecentFiles history;
    const fs::path storage = directory / "recent.json", a = directory / "A", b = directory / "B";
    history.Load(storage);
    history.Add(a, a / "same.tgscene");
    history.Add(b, b / "same.tgscene");
    Check(history.Entries(a).size() == 1 && history.Entries(b).size() == 1, "ルートごとに分かれる");
    history.Add(a, a / "SAME.tgscene");
    Check(history.Entries(a).size() == 1 && history.Roots().front().path == a, "大文字小文字を同一視し、ルートが先頭へ来る");
    for (int i = 0; i < 12; ++i) history.Add(a, a / (std::to_string(i) + ".tgscene"));
    Check(history.Entries(a).size() == 10 && history.Entries(a).front().filename() == "11.tgscene", "上限 10 件、新しい順");
    tg::io::RecentFiles loaded;
    loaded.Load(storage);
    Check(loaded.Entries(a) == history.Entries(a) && loaded.Entries(b).size() == 1, "保存と読み込み");
    loaded.Clear(a);
    Check(loaded.Entries(a).empty() && loaded.Entries(b).size() == 1, "ルート単位で消す");
    for (int i = 0; i < 12; ++i) loaded.AddRoot(directory / std::to_string(i));
    Check(loaded.Roots().size() == 10, "ルートも上限 10 件");
    ProjectWorkspace::WriteJson(storage, {{"format", "road-editor.recent"}, {"version", 1},
        {"projects", {tg::ToUtf8Portable(a / "legacy.tgproj"), tg::ToUtf8Portable(b / "legacy.tgproj")}}});
    loaded.Load(storage);
    loaded.AddRoot(a);
    Check(loaded.Entries(a).size() == 1 && loaded.Entries(b).empty(), "旧履歴は所属ルートを開いたときだけ移る");
    loaded.Load(storage);
    loaded.AddRoot(b);
    Check(loaded.Entries(a).size() == 1 && loaded.Entries(b).size() == 1, "未移行の旧履歴は残る");
    loaded.ClearRoots();
    loaded.Load(storage);
    Check(loaded.Roots().empty(), "全消去が保存される");
    ProjectWorkspace workspace;
    fs::create_directories(a, error);
    Check(workspace.Open(a), "履歴用のルートを開く");
    ProjectWorkspace::WriteJson(storage, {{"format", "road-editor.recent"}, {"projects", {tg::ToUtf8Portable(a / "nested.tgscene")}}});
    loaded.Load(storage);
    loaded.AddRoot(directory);
    Check(loaded.Entries(directory).empty(), "入れ子のルートの履歴を親へ取り込まない");
    loaded.AddRoot(a);
    Check(loaded.Entries(a).size() == 1, "入れ子のルート自身を開けば移る");

    Section("ThumbnailStore: ディスクキャッシュの有効判定");
    const fs::path image = a / "source.png";
    Touch(image, 'a');
    const json source = workspace.Reference(image);
    fs::path material = a / "material.tgmat";
    json materialBody = {{"maps", {{"baseColor", source}}}};
    Check(workspace.SaveAsset(material, "material-asset", materialBody), "検証用のマテリアル");
    const auto original = tg::io::AssetThumbnailRecord(workspace, material);
    Touch(original.image);
    Check(tg::io::CommitThumbnail(original) && tg::io::ThumbnailIsCurrent(original), "記録した直後は有効");
    Touch(a / "unrelated.png", 'z');
    Check(tg::io::ThumbnailIsCurrent(tg::io::AssetThumbnailRecord(workspace, material)), "無関係なファイルでは無効にならない");
    std::ofstream(image, std::ios::app | std::ios::binary).put('b');
    const auto changed = tg::io::AssetThumbnailRecord(workspace, material);
    Check(original.image == changed.image && !tg::io::ThumbnailIsCurrent(changed), "参照先の画像が変わると同じ枠が無効になる");
    fs::remove(image, error);
    Check(changed.stamp != tg::io::AssetThumbnailRecord(workspace, material).stamp, "参照先が消えても無効になる");
    const fs::path scene = a / "scene.tgscene";
    json sceneDocument = {{"textures", json::array()}, {"materials", json::array()}, {"skies", json::array()}};
    Check(workspace.SaveScene(scene, sceneDocument), "シーン ID 付きで保存する");
    const auto thumbnail = tg::io::SceneThumbnailPath(workspace, scene);
    Check(thumbnail.parent_path() == a / ".terrain-graph" / "scene-thumbnails", "シーンの画像はルート内の一か所へ");
    const fs::path renamedScene = a / "renamed.tgscene";
    fs::rename(scene, renamedScene, error);
    Check(tg::io::SceneThumbnailPath(workspace, renamedScene) == thumbnail, "改名しても同じ画像を指す");

    Section("AssetRelations: 削除前の参照検査");
    ProjectWorkspace deletion;
    const fs::path deleteRoot = directory / "delete-root";
    fs::create_directories(deleteRoot, error);
    Check(deletion.Open(deleteRoot), "削除検査用のルート");
    const fs::path texture = deleteRoot / "image.png";
    Touch(texture, 't');
    const json textureRef = deletion.Reference(texture);
    fs::path materialFile = deleteRoot / "shared.tgmat";
    json dependency = {{"maps", {{"baseColor", textureRef}}}};
    Check(deletion.SaveAsset(materialFile, "material-asset", dependency), "参照元のマテリアル");
    auto report = tg::io::InspectAssetRelations(deletion, texture);
    Check(report.complete && report.referencers.size() == 1 && report.referencers[0] == materialFile && report.companions.size() == 1,
          "参照元と .meta を列挙する");
    std::ofstream(texture.wstring() + L".meta", std::ios::app | std::ios::binary).put(' ');
    Check(!tg::io::RetireAsset(deletion, report) && fs::exists(texture), ".meta が変わっていれば再確認を求める");
    report = tg::io::InspectAssetRelations(deletion, texture);
    const auto materialReport = tg::io::InspectAssetRelations(deletion, materialFile);
    Check(materialReport.related.size() == 1 && materialReport.related[0] == texture, "参照先の素材を関連として出す");
    fs::path secondMaterial = deleteRoot / "second.tgmat";
    dependency.erase("uid");
    Check(deletion.SaveAsset(secondMaterial, "material-asset", dependency), "確認後に参照元が増える");
    Check(!tg::io::RetireAsset(deletion, report) && fs::exists(texture), "参照関係が変わっていれば再確認を求める");
    const auto updated = tg::io::InspectAssetRelations(deletion, texture);
    Check(tg::io::RetireAsset(deletion, updated), "確認どおりなら退避する");
    Check(!fs::exists(texture) && !fs::exists(texture.wstring() + L".meta") && fs::exists(materialFile), "参照元は消さない");
    bool recoverable = false;
    for (const auto& entry : fs::recursive_directory_iterator(deleteRoot / ".terrain-graph" / "trash", error))
        if (entry.path().filename() == "image.png") recoverable = true;
    Check(recoverable, "退避した元ファイルが残る");
    std::ofstream(deleteRoot / "broken.tgmat", std::ios::binary) << "{broken";
    Check(!tg::io::InspectAssetRelations(deletion, materialFile).complete, "読めない文書があれば不完全");
    Check(!tg::io::InspectAssetRelations(deletion, deleteRoot / "project.tgproj").complete, "目印ファイルは削除できない");
    fs::remove_all(directory, error);
}

}  // namespace

void RunProjectWorkspaceTests() {
    TestWorkspace();
    TestHistoryAndThumbnails();
}
