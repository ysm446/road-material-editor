// アセットの帯。プロジェクトのルートフォルダの階層と、フォルダの中身（画像・マテリアル・
// 天球・シーン）をサムネイルの格子で出す。旧「テクスチャ / マテリアル / 天球」のタブの代わり。
//
// 一覧はファイルそのもの。**シーンへ読み込んでいないものも見える。** ダブルクリックで
// 読み込み（画像 → テクスチャ、.tgmat → マテリアル、.tgsky → 天球、.tgscene → シーン）。
// 読み込み済みのものはライブラリのサムネイルとドラッグ元（TG_TEXTURE / TG_MATERIAL）を使い、
// 未読み込みのものは AssetThumbnailCache が別領域で作ったサムネイルを出す。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "core/FileDialog.h"
#include "core/Log.h"
#include "core/Shell.h"
#include "io/AssetRelations.h"
#include "io/ProjectIo.h"
#include "io/ThumbnailStore.h"
#include "ui/UiStyle.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <unordered_map>
#include <functional>

namespace tg {
namespace fs = std::filesystem;
namespace {

std::string Extension(const fs::path& path) {
    auto ext = ToUtf8Portable(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext;
}

bool IsImage(const std::string& ext) {
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".exr" || ext == ".tga" || ext == ".bmp";
}

// 比較用のキー。**ファイルシステムには触らない。** 帯は毎フレーム全ファイル × 読み込み済み全件を
// 突き合わせるので、weakly_canonical のような問い合わせを挟むとシーンを開いた途端に描画が落ちる。
// ライブラリとルートのパスはどちらも絶対パスなので、正規化と大文字小文字の同一視で足りる。
std::wstring PathKey(const fs::path& path) {
    std::wstring key = path.lexically_normal().wstring();
    for (wchar_t& c : key) {
        if (c == L'/') c = L'\\';
        else c = static_cast<wchar_t>(std::towlower(c));
    }
    return key;
}

bool SameFile(const fs::path& a, const fs::path& b) {
    if (a.empty() || b.empty()) return false;
    return PathKey(a) == PathKey(b);
}

// サムネイル枠の中央へ、タブ付きフォルダの輪郭だけを描く（字形ではなく図形で描く）。
void DrawFolderIcon(const ImVec2& min, const ImVec2& max) {
    const float size = std::min(max.x - min.x, max.y - min.y);
    const float left = (min.x + max.x) * 0.5f - size * 0.34f;
    const float top = (min.y + max.y) * 0.5f - size * 0.23f;
    const auto point = [&](float x, float y) { return ImVec2(left + size * x, top + size * y); };
    auto* draw = ImGui::GetWindowDrawList();
    // 閉じる位置は上辺の途中に置き、終点と始点の重複による線の歪みを避ける。
    draw->PathLineTo(point(0.12f, 0.0f));
    draw->PathLineTo(point(0.23f, 0.0f));
    draw->PathLineTo(point(0.31f, 0.08f));
    draw->PathLineTo(point(0.64f, 0.08f));
    draw->PathBezierCubicCurveTo(point(0.67f, 0.08f), point(0.68f, 0.09f), point(0.68f, 0.12f));
    draw->PathLineTo(point(0.68f, 0.44f));
    draw->PathBezierCubicCurveTo(point(0.68f, 0.47f), point(0.67f, 0.48f), point(0.64f, 0.48f));
    draw->PathLineTo(point(0.04f, 0.48f));
    draw->PathBezierCubicCurveTo(point(0.01f, 0.48f), point(0.0f, 0.47f), point(0.0f, 0.44f));
    draw->PathLineTo(point(0.0f, 0.04f));
    draw->PathBezierCubicCurveTo(point(0.0f, 0.01f), point(0.01f, 0.0f), point(0.04f, 0.0f));
    const auto color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const float thickness = size / 42.0f;
    draw->PathStroke(color, ImDrawFlags_Closed, thickness);
    draw->AddLine(point(0.0f, 0.15f), point(0.68f, 0.15f), color, thickness);
}

}  // namespace

bool Application::IsAssetLoaded(const fs::path& path) const {
    if (SameFile(m_projectPath, path)) return true;
    for (const auto& a : m_textureLibrary.Entries()) if (SameFile(a.path, path)) return true;
    for (const auto& a : m_materialLibrary.Entries()) if (SameFile(a.assetPath, path)) return true;
    for (const auto& a : m_skyLibrary.Entries()) if (SameFile(a.assetPath, path) || SameFile(a.sky.hdriPath, path)) return true;
    return false;
}

void Application::DrawAssetDeleteDialog() {
    if (m_assetDeleteDialog && !ImGui::IsPopupOpen("アセットファイルの削除")) ImGui::OpenPopup("アセットファイルの削除");
    if (!ImGui::BeginPopupModal("アセットファイルの削除", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const auto& report = m_assetDeleteRelations;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ui::Scaled(520));
    ImGui::TextUnformatted(ToUtf8Display(report.target).c_str());
    ImGui::PopTextWrapPos();
    ui::HintText("元ファイルと付随する .meta を、ルート内の退避フォルダ（.terrain-graph/trash）へ移します。");
    const auto list = [&](const char* title, const std::vector<fs::path>& paths) {
        if (paths.empty()) return;
        ImGui::Separator();
        ImGui::TextUnformatted(title);
        const float height = std::min(float(paths.size()), 5.0f) * ImGui::GetTextLineHeightWithSpacing() + ui::Scaled(12);
        if (ImGui::BeginChild(title, ImVec2(ui::Scaled(520), height), ImGuiChildFlags_Borders))
            for (const auto& path : paths)
                ImGui::TextUnformatted(ToUtf8Display(path.lexically_relative(m_workspace.Root())).c_str());
        ImGui::EndChild();
    };
    list("一緒に退避するファイル", report.companions);
    list("直接の参照元（削除すると参照切れになります）", report.referencers);
    list("関連ファイル（削除せず残します）", report.related);
    const bool loaded = IsAssetLoaded(report.target);
    if (!report.complete) ui::HintText("参照関係をすべて確認できませんでした。読めないファイルやリンクを確認してください。");
    if (loaded) ui::HintText("現在のシーンに読み込まれています。新規シーンなどへ切り替えてから削除してください。");
    ImGui::Separator();
    ImGui::BeginDisabled(!report.complete || loaded);
    if (ImGui::Button("削除する")) {
        m_pendingAssetDelete = true;
        m_assetDeleteDialog = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("キャンセル") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_assetDeleteDialog = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Application::ResumeSceneSwitch() {
    m_pendingRoot = std::move(m_deferredRoot);
    m_deferredRoot.clear();
    m_pendingProjectOpen = std::move(m_deferredScene);
    m_deferredScene.clear();
    m_pendingProjectNew = m_deferredNew;
    m_deferredNew = false;
    m_allowSceneSwitch = true;
    m_sceneSwitchDialog = false;
}

void Application::DrawSceneSwitchDialog() {
    if (m_sceneSwitchDialog && !ImGui::IsPopupOpen("シーンの切り替え")) ImGui::OpenPopup("シーンの切り替え");
    if (!ImGui::BeginPopupModal("シーンの切り替え", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ui::HintText("現在のシーンと共有アセットを保存してから切り替えますか？");
    if (ImGui::Button("保存して切り替え")) {
        RequestSaveProject(false);
        if (!m_pendingProjectSave.empty()) {
            m_saveThenSwitch = true;
            m_sceneSwitchDialog = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("保存せず切り替え")) {
        ResumeSceneSwitch();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("キャンセル")) {
        m_sceneSwitchDialog = false;
        m_deferredRoot.clear();
        m_deferredScene.clear();
        m_deferredNew = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Application::RefreshAssetBrowser() {
    m_assetEntries.clear();
    std::error_code error;
    if (!m_workspace.Contains(m_assetDirectory) || !fs::is_directory(m_assetDirectory, error)) {
        m_assetDirectory = m_workspace.Root();
    }
    fs::directory_iterator it(m_assetDirectory, fs::directory_options::skip_permission_denied, error), end;
    for (; it != end && !error; it.increment(error)) {
        const auto& entry = *it;
        // .meta と内部フォルダ（.terrain-graph）は出さない。
        if (entry.is_symlink(error) || entry.path().filename().wstring().starts_with(L".")) continue;
        const auto ext = Extension(entry.path());
        if (!entry.is_directory(error) && !IsImage(ext) && ext != ".hdr" && ext != ".tgmat" &&
            ext != ".tgsky" && ext != ".tgscene" && ext != ".tgproj" && ext != ".mmproj") continue;
        m_assetEntries.push_back(entry);
    }
    std::sort(m_assetEntries.begin(), m_assetEntries.end(), [](const auto& a, const auto& b) {
        std::error_code errorA, errorB;
        const bool directoryA = a.is_directory(errorA), directoryB = b.is_directory(errorB);
        if (directoryA != directoryB) return directoryA;
        return a.path().filename() < b.path().filename();
    });
    // フォルダ階層。ドット始まりと symlink は出さない。深さは 32 まで。
    m_assetFolders.clear();
    const auto collect = [&](auto&& self, const fs::path& directory, int depth) -> void {
        if (depth > 32) return;
        auto& children = m_assetFolders[directory.wstring()];
        std::error_code scanError;
        fs::directory_iterator child(directory, fs::directory_options::skip_permission_denied, scanError), childEnd;
        for (; child != childEnd && !scanError; child.increment(scanError))
            if (child->is_directory(scanError) && !child->is_symlink(scanError) &&
                !child->path().filename().wstring().starts_with(L".")) children.push_back(child->path());
        std::sort(children.begin(), children.end());
        for (const auto& path : children) self(self, path, depth + 1);
    };
    collect(collect, m_workspace.Root(), 0);
    m_assetRefresh = false;
}

// フレームの外で処理するアセット関連の作業（ルートの切り替え、共有アセットの保存、
// ファイルを開く、削除、サムネイルの生成）。ProcessPendingFileWork から呼ぶ。
void Application::ProcessAssetWork() {
    if (m_pendingAssetDelete) {
        m_pendingAssetDelete = false;
        const auto path = m_assetDeleteRelations.target;
        if (!IsAssetLoaded(path) && io::RetireAsset(m_workspace, m_assetDeleteRelations)) {
            m_recentProjects.Remove(m_workspace.Root(), path);
            m_selectedAssetPath.clear();
            m_assetRefresh = true;
            m_assetThumbnails.Invalidate();
        } else {
            TG_LOG_WARN("削除できませんでした。対象と参照関係を再確認してください");
            m_pendingAssetDeleteInspect = path;
        }
    }
    if (!m_pendingAssetDeleteInspect.empty()) {
        m_assetDeleteRelations = io::InspectAssetRelations(m_workspace, m_pendingAssetDeleteInspect);
        m_pendingAssetDeleteInspect.clear();
        m_assetDeleteDialog = true;
    }
    // --project にはシーンだけでなく、ルートのフォルダや目印ファイル（project.tgproj）も渡せる。
    if (!m_pendingProjectOpen.empty()) {
        std::error_code error;
        if (fs::is_directory(m_pendingProjectOpen, error)) {
            m_pendingRoot = m_pendingProjectOpen;
            m_pendingProjectOpen.clear();
        } else if (io::ProjectWorkspace::IsWorkspaceFile(m_pendingProjectOpen)) {
            m_pendingRoot = m_pendingProjectOpen.parent_path();
            m_pendingProjectOpen.clear();
        }
    }
    if (!m_pendingRoot.empty()) {
        const auto root = m_pendingRoot;
        m_pendingRoot.clear();
        io::ProjectWorkspace next;
        if (next.Open(root)) {
            // ルートの選択とシーンの選択を分ける。履歴から指定されたシーンだけ開く。
            if (!m_pendingProjectOpen.empty() && Extension(m_pendingProjectOpen) == ".tgscene") {
                nlohmann::json validation;
                if (!next.ReadScene(m_pendingProjectOpen, validation)) {
                    TG_LOG_ERROR("シーンを読み込めません。現在のプロジェクトを保持します");
                    m_pendingProjectOpen.clear();
                    return;
                }
            }
            m_workspace = std::move(next);
            m_assetDirectory = m_workspace.Root();
            m_selectedAssetPath.clear();
            m_assetRefresh = true;
            m_assetThumbnails.Invalidate();
            if (!Headless()) m_recentProjects.AddRoot(m_workspace.Root());
            ResetProject();
            m_projectPath.clear();
            UpdateWindowTitle();
            TG_LOG_INFO("ルートフォルダを開きました: %s", ToUtf8Display(m_workspace.Root()).c_str());
        } else {
            m_pendingProjectOpen.clear();
        }
    }
    if (m_pendingAssetsSave) {
        m_pendingAssetsSave = false;
        io::ProjectRefs refs{m_textureLibrary, m_materialLibrary, m_skyLibrary, m_renderer, m_graph, m_surfaceLayouts,
                             m_previewSurfaceBands, m_connectSurfaceBands, m_displaceConnectedBands};
        if (io::SaveSharedAssets(m_workspace, refs)) TG_LOG_INFO("共有アセットを保存しました");
        else TG_LOG_ERROR("共有アセットを保存できませんでした");
        m_assetRefresh = true;
        m_assetThumbnails.Invalidate();
    }
    if (!m_pendingAssetOpen.empty()) {
        const auto path = m_pendingAssetOpen;
        m_pendingAssetOpen.clear();
        const auto ext = Extension(path);
        if (ext == ".tgmat" || ext == ".tgsky") {
            nlohmann::json header;
            if (ext == ".tgmat" && io::ProjectWorkspace::ReadJson(path, header) &&
                io::ProjectWorkspace::String(header, "format") != "terrain-graph.material-asset") {
                // 持ち出し用の旧 .tgmat は従来の読み込み（ライブラリへ 1 つ足す）。
                m_pendingMaterialImport = path;
                return;
            }
            if (io::LoadSharedAsset(m_workspace, path, m_device, m_pipelineCache,
                                    m_textureLibrary, m_materialLibrary, m_skyLibrary)) {
                if (ext == ".tgmat") {
                    const auto& entries = m_materialLibrary.Entries();
                    for (size_t i = 0; i < entries.size(); ++i)
                        if (SameFile(entries[i].assetPath, path)) m_selectedMaterial = static_cast<int>(i);
                    m_showMaterialSphere = true;
                    m_scrollToSelectedMaterial = true;
                } else {
                    m_showSkyPreview = true;
                    m_scrollToSelectedSky = true;
                }
                m_renderer.InvalidateSceneMaterials();
                MarkDocumentChanged();
            } else {
                TG_LOG_ERROR("アセットを開けません: %s", ToUtf8Display(path).c_str());
            }
        } else if (IsImage(ext)) {
            const compositor::TextureId id = m_textureLibrary.Load(m_device, m_pipelineCache, path);
            if (id != compositor::kNoTexture) {
                const auto& entries = m_textureLibrary.Entries();
                for (size_t i = 0; i < entries.size(); ++i)
                    if (entries[i].id == id) m_selectedTexture = static_cast<int>(i);
                m_showTexturePreview = true;
                m_scrollToSelectedTexture = true;
            }
        } else {
            HandleDroppedFiles({path});
        }
        m_assetRefresh = true;
    }
    if (m_assetRefresh) RefreshAssetBrowser();
    m_assetThumbnails.Process(m_device, m_pipelineCache, m_workspace, m_assetDirectory);
}

void Application::DrawAssetBrowser() {
    if (!ImGui::Begin("アセット")) {
        ImGui::End();
        return;
    }
    if (ImGui::Button("ルートを開く…")) RequestOpenProject();
    ImGui::SameLine();
    if (ImGui::Button("更新")) {
        m_assetRefresh = true;
        m_workspace.Scan();
        m_assetThumbnails.Invalidate();
    }
    ImGui::SameLine();
    if (ImGui::Button("アセットを保存")) m_pendingAssetsSave = true;
    ImGui::SameLine();
    ImGui::TextDisabled("%s", ToUtf8Display(m_assetDirectory).c_str());

    const auto available = ImGui::GetContentRegionAvail();
    const float margin = ui::Scaled(ui::kSplitterMargin);
    const float usable = std::max(2.0f, available.x - margin * 2.0f - ui::Scaled(ui::kSplitterGrabWidth));
    const float minimum = std::min(ui::Scaled(120.0f), usable * 0.5f);
    float folderWidth = std::clamp(ui::Scaled(m_settings.Ui().assetFolderWidth), minimum, usable - minimum);
    // --- 左: フォルダ階層 ------------------------------------------------
    if (ImGui::BeginChild("folders", ImVec2(folderWidth, 0), ImGuiChildFlags_Borders)) {
        const auto tree = [&](auto&& self, const fs::path& directory, int depth) -> void {
            if (depth > 32) return;
            const auto label = ToUtf8Display(directory.filename());
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (directory == m_workspace.Root()) flags |= ImGuiTreeNodeFlags_DefaultOpen;
            if (directory == m_assetDirectory) flags |= ImGuiTreeNodeFlags_Selected;
            ImGui::PushID(ToUtf8Portable(directory).c_str());
            const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                m_assetDirectory = directory;
                m_assetRefresh = true;
            }
            if (open) {
                if (const auto found = m_assetFolders.find(directory.wstring()); found != m_assetFolders.end())
                    for (const auto& child : found->second) self(self, child, depth + 1);
                ImGui::TreePop();
            }
            ImGui::PopID();
        };
        tree(tree, m_workspace.Root(), 0);
    }
    ImGui::EndChild();
    ImGui::SameLine(0.0f, margin);
    const float previousWidth = folderWidth;
    const bool released = ui::VerticalSplitter("assetFolderSplitter", &folderWidth, minimum, usable - minimum, available.y);
    if (folderWidth != previousWidth) m_settings.Ui().assetFolderWidth = folderWidth / ui::Scaled(1.0f);
    if (released) m_settings.Save();
    ImGui::SameLine(0.0f, margin);
    // --- 右: フォルダの中身 ----------------------------------------------
    if (ImGui::BeginChild("contents", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        if (m_assetDirectory != m_workspace.Root() && ImGui::Button("上のフォルダ")) {
            m_assetDirectory = m_assetDirectory.parent_path();
            m_assetRefresh = true;
        }
        if (m_assetEntries.empty()) ui::HintText("右クリックでアセットを作成、またはファイルを読み込みます");
        const float size = ui::Scaled(84);
        const int columns = std::max(1, int(ImGui::GetContentRegionAvail().x / (size + ImGui::GetStyle().ItemSpacing.x)));
        // 読み込み済みのものをパスで引く表。ファイルごとにライブラリを総なめしない。
        struct Loaded {
            ImTextureID handle = 0;
            compositor::TextureId texture = compositor::kNoTexture;
            compositor::MaterialAssetId material = compositor::kNoMaterialAsset;
            bool missing = false;
        };
        std::unordered_map<std::wstring, Loaded> loaded;
        for (const auto& a : m_textureLibrary.Entries()) if (!a.path.empty())
            loaded[PathKey(a.path)] = {static_cast<ImTextureID>(a.PreviewHandle().ptr), a.id, compositor::kNoMaterialAsset, a.missing};
        for (const auto& a : m_materialLibrary.Entries()) if (!a.assetPath.empty())
            loaded[PathKey(a.assetPath)] = {static_cast<ImTextureID>(a.thumbnail.srv.gpu.ptr), compositor::kNoTexture, a.id, MaterialHasMissingTexture(a)};
        for (const auto& a : m_skyLibrary.Entries()) if (!a.assetPath.empty())
            loaded[PathKey(a.assetPath)] = {static_cast<ImTextureID>(a.thumbnail.srv.gpu.ptr)};
        int index = 0;
        for (const auto& entry : m_assetEntries) {
            const auto path = entry.path();
            const auto ext = Extension(path);
            std::error_code error;
            const bool folder = entry.is_directory(error);
            ImTextureID handle = 0;
            compositor::TextureId textureId = compositor::kNoTexture;
            compositor::MaterialAssetId materialId = compositor::kNoMaterialAsset;
            bool missing = false;
            if (const auto found = loaded.find(PathKey(path)); found != loaded.end()) {
                handle = found->second.handle;
                textureId = found->second.texture;
                materialId = found->second.material;
                missing = found->second.missing;
            }
            if (!handle && !folder && ImGui::IsRectVisible(ImVec2(size, size)))
                handle = static_cast<ImTextureID>(m_assetThumbnails.Request(path).ptr);
            ImGui::PushID(ToUtf8Portable(path).c_str());
            ImGui::BeginGroup();
            const auto thumb = ui::ThumbnailButton("##asset", handle, size, m_selectedAssetPath == path);
            if (folder) {
                DrawFolderIcon(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            } else if (!handle) {
                const char* type = ext == ".tgscene" ? "シーン" : ext == ".tgmat" ? "マテリアル" :
                    ext == ".tgsky" ? "天球" : (ext == ".tgproj" || ext == ".mmproj") ? "旧形式" :
                    IsImage(ext) || ext == ".hdr" ? "画像" : "ファイル";
                const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                const auto text = ImGui::CalcTextSize(type);
                ImGui::GetWindowDrawList()->AddText(ImVec2((min.x + max.x - text.x) * 0.5f, (min.y + max.y - text.y) * 0.5f),
                                                    ImGui::GetColorU32(ImGuiCol_TextDisabled), type);
            }
            if (missing || (!handle && m_assetThumbnails.Failed(path)))
                ui::MissingBadge(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            if (thumb.clicked) {
                m_selectedAssetPath = path;
                // 読み込み済みのものは、一覧での選択もそれへ合わせる（プレビューの窓が追従する）。
                if (textureId != compositor::kNoTexture) {
                    const auto& entries = m_textureLibrary.Entries();
                    for (size_t i = 0; i < entries.size(); ++i) if (entries[i].id == textureId) m_selectedTexture = int(i);
                }
                if (materialId != compositor::kNoMaterialAsset) {
                    const auto& entries = m_materialLibrary.Entries();
                    for (size_t i = 0; i < entries.size(); ++i) if (entries[i].id == materialId) m_selectedMaterial = int(i);
                }
            }
            if (thumb.doubleClicked) {
                if (folder) {
                    m_assetDirectory = path;
                    m_assetRefresh = true;
                } else if (textureId != compositor::kNoTexture) {
                    m_showTexturePreview = true;
                } else if (materialId != compositor::kNoMaterialAsset) {
                    m_showMaterialSphere = true;
                } else {
                    m_pendingAssetOpen = path;
                }
            }
            // 読み込み済みの画像とマテリアルは従来と同じペイロードでドラッグできる。
            // テクスチャは hold-to-switch を残すため SourceNoHoldToOpenOthers を付けない。
            if ((textureId != compositor::kNoTexture || materialId != compositor::kNoMaterialAsset) &&
                ImGui::BeginDragDropSource()) {
                if (materialId != compositor::kNoMaterialAsset)
                    ImGui::SetDragDropPayload(kMaterialDragDropType, &materialId, sizeof(materialId));
                else
                    ImGui::SetDragDropPayload(kTextureDragDropType, &textureId, sizeof(textureId));
                ImGui::TextUnformatted(ToUtf8Display(path.filename()).c_str());
                ImGui::EndDragDropSource();
            }
            if (thumb.hovered) ImGui::SetTooltip("%s\nダブルクリックで開く", ToUtf8Display(path).c_str());
            if (ImGui::BeginPopupContextItem("assetMenu")) {
                m_selectedAssetPath = path;
                if (ImGui::MenuItem("開く")) {
                    if (folder) { m_assetDirectory = path; m_assetRefresh = true; }
                    else m_pendingAssetOpen = path;
                }
                if (ImGui::MenuItem("エクスプローラで表示")) RevealFileInExplorer(path);
                if (textureId != compositor::kNoTexture) {
                    ImGui::Separator();
                    DrawTextureContextMenu(textureId);
                } else if (materialId != compositor::kNoMaterialAsset) {
                    ImGui::Separator();
                    DrawMaterialContextMenu(materialId);
                }
                if (!folder && path.filename() != L"project.tgproj") {
                    ImGui::Separator();
                    if (ImGui::MenuItem("ファイルを削除…")) m_pendingAssetDeleteInspect = path;
                }
                ImGui::EndPopup();
            }
            ui::GridCaption(ToUtf8Display(path.filename()).c_str(), size);
            ImGui::EndGroup();
            ImGui::PopID();
            if (++index % columns && index < int(m_assetEntries.size())) ImGui::SameLine();
        }
        if (ImGui::BeginPopupContextWindow("createAsset", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
            if (ImGui::MenuItem("フォルダを作成")) {
                const auto path = m_workspace.UniquePath(m_assetDirectory, "NewFolder", "");
                std::error_code error;
                if (!path.empty()) fs::create_directory(path, error);
                if (path.empty() || error) TG_LOG_ERROR("フォルダを作成できませんでした");
                m_assetRefresh = true;
            }
            if (ImGui::MenuItem("マテリアルを作成")) {
                const auto id = m_materialLibrary.Add("新規マテリアル");
                auto* asset = m_materialLibrary.FindMutable(id);
                asset->assetPath = m_workspace.UniquePath(m_assetDirectory, asset->name, ".tgmat");
                m_selectedMaterial = static_cast<int>(m_materialLibrary.Entries().size()) - 1;
                m_showMaterialSphere = true;
                m_pendingAssetsSave = true;
                MarkDocumentChanged();
            }
            if (ImGui::MenuItem("天球を作成")) {
                const auto id = m_skyLibrary.Add("新規天球");
                auto* asset = m_skyLibrary.FindMutable(id);
                asset->assetPath = m_workspace.UniquePath(m_assetDirectory, asset->name, ".tgsky");
                m_skyLibrary.SetActive(id);
                m_showSkyPreview = true;
                m_pendingAssetsSave = true;
            }
            if (ImGui::MenuItem("ファイルを読み込む…")) {
                const auto paths = ShowOpenFilesDialog(
                    L"アセットを読み込む", {{L"画像 / HDRI / マテリアル", L"*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.exr;*.hdr;*.tgmat"}});
                HandleDroppedFiles(paths);
            }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

}  // namespace tg
