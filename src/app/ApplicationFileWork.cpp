// ファイルメニュー、ショートカット、ドロップの受け付けと、
// フレームの外で処理する保留ファイル作業（開く / 保存 / 削除）。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "core/FileDialog.h"
#include "core/Log.h"
#include "io/ProjectIo.h"
#include "ui/UiStyle.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace tg {

// ファイルメニュー。ここでは要求を積むだけで、実際の読み書きは
// ProcessPendingFileWork がフレームの外で行う（GPU 待機を伴うため）。
void Application::RequestOpenProject() {
    const std::filesystem::path path =
        ShowOpenFileDialog(L"プロジェクトを開く", ProjectFileFilters());
    if (!path.empty()) {
        m_pendingProjectOpen = path;
    }
}

// saveAs が偽でも、まだ一度も保存していなければ保存先を聞く。
void Application::RequestSaveProject(bool saveAs) {
    if (!saveAs && !m_projectPath.empty()) {
        m_pendingProjectSave = m_projectPath;
        return;
    }
    const std::filesystem::path path = ShowSaveFileDialog(
        L"プロジェクトを保存", ProjectFileFilters(), L"tgproj", m_projectPath);
    if (!path.empty()) {
        m_pendingProjectSave = path;
    }
}

// キーボードショートカット。メニューと同じ入口（Request*）を通す。
//
// テキスト入力中でも効かせる（Ctrl + S は入力欄が食う操作ではない）。
// 実際の読み書きはどれも保留されるので、押された時点では要求が積まれるだけ。
void Application::HandleShortcuts() {
    const ImGuiIO& io = ImGui::GetIO();

    // F12 はコンテンツ領域のスクリーンショット。修飾キーは要らないので先に見る。
    // 実際の撮影はフレームの終わり（EndFrame）で行う。
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F12, false)) {
        m_screenshotPending = true;
    }

    if (!io.KeyCtrl || io.KeyAlt) {
        return;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_N, false)) {
        m_pendingProjectNew = true;
    } else if (ImGui::IsKeyPressed(ImGuiKey_B, false) && !io.WantTextInput) {
        // アセットの帯を畳む / 戻す（ウィンドウメニューと同じ）。
        m_settings.Display().showAssetBand = !m_settings.Display().showAssetBand;
        m_settings.Save();
    } else if (ImGui::IsKeyPressed(ImGuiKey_O, false)) {
        RequestOpenProject();
    } else if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        // Ctrl + Shift + S は「名前を付けて保存」。
        RequestSaveProject(io.KeyShift);
    } else if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        // テキスト入力中は InputText 内部のアンドゥに任せる。
        // 文書のアンドゥまで同時に走ると、無関係な編集が巻き戻る。
        if (io.WantTextInput) {
            return;
        }
        // Ctrl + Shift + Z も「やり直す」。Ctrl + Y と同じ。
        if (io.KeyShift) {
            if (m_undoHistory.CanRedo()) {
                m_pendingHistoryStep = 1;
            }
        } else if (m_undoHistory.CanUndo()) {
            m_pendingHistoryStep = -1;
        }
    } else if (ImGui::IsKeyPressed(ImGuiKey_Y, false) && !io.WantTextInput &&
               m_undoHistory.CanRedo()) {
        m_pendingHistoryStep = 1;
    }
}

// 最近使ったプロジェクト。名前を項目に、置き場所を右の列に出す。
// 同じ名前のプロジェクトが別の場所にあっても見分けられるようにするため。
void Application::DrawRecentMenu() {
    const std::vector<std::filesystem::path>& entries = m_recentProjects.Entries();
    if (!ImGui::BeginMenu("最近使ったプロジェクト", !entries.empty())) {
        return;
    }

    for (size_t i = 0; i < entries.size(); ++i) {
        const std::filesystem::path& path = entries[i];
        ImGui::PushID(static_cast<int>(i));

        const std::string name = ToUtf8Display(path.filename());
        const std::string directory = ToUtf8Display(path.parent_path());
        if (ImGui::MenuItem(name.c_str(), directory.c_str())) {
            m_pendingProjectOpen = path;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", ToUtf8Display(path).c_str());
        }

        ImGui::PopID();
    }

    ImGui::Separator();
    if (ImGui::MenuItem("履歴を消す")) {
        m_recentProjects.Clear();
    }
    ImGui::EndMenu();
}

void Application::DrawFileMenu() {
    if (!ImGui::BeginMenu("ファイル")) {
        return;
    }

    if (ImGui::MenuItem("新規", "Ctrl+N")) {
        m_pendingProjectNew = true;
    }
    if (ImGui::MenuItem("開く…", "Ctrl+O")) {
        RequestOpenProject();
    }
    DrawRecentMenu();
    if (ImGui::MenuItem("保存", "Ctrl+S")) {
        RequestSaveProject(false);
    }
    if (ImGui::MenuItem("名前を付けて保存…", "Ctrl+Shift+S")) {
        RequestSaveProject(true);
    }

    ImGui::Separator();
    if (ImGui::MenuItem("終了")) {
        m_window.RequestClose();
    }
    ImGui::EndMenu();
}

void Application::HandleDroppedFiles(const std::vector<std::filesystem::path>& paths) {
    size_t images = 0;
    for (const std::filesystem::path& path : paths) {
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        // 拡張子で行き先を決める。読み込み自体はどれも保留し、フレームの外で処理する。
        // 旧拡張子 (.mmproj / .mmmat) は material-mixer 時代のファイル。読み込みだけ受け付ける。
        if (extension == ".tgproj" || extension == ".mmproj") {
            m_pendingProjectOpen = path;
        } else if (extension == ".tgmat" || extension == ".mmmat") {
            m_pendingMaterialImport = path;
        } else if (extension == ".hdr") {
            // 選択中の天球へ入れる。天球は必ず 1 つあるので、行き先は常に決まる。
            m_skyLibrary.EnsureDefault();
            if (renderer::SkyAsset* sky = m_skyLibrary.ActiveMutable(); sky != nullptr) {
                sky->sky.source = renderer::SkySource::Hdri;
                sky->sky.hdriPath = path;
                m_skyLibrary.MarkThumbnailDirty(sky->id);
                TG_LOG_INFO("天球「%s」に %s を割り当てました", sky->name.c_str(),
                            ToUtf8Display(path.filename()).c_str());
            }
        } else if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
                   extension == ".tga" || extension == ".bmp" || extension == ".exr") {
            m_pendingTexturePaths.push_back(path);
            ++images;
        } else {
            TG_LOG_WARN("扱えない形式です: %s", ToUtf8Display(path.filename()).c_str());
        }
    }
    if (images > 0) {
        TG_LOG_INFO("%zu 枚の画像を読み込みます", images);
    }
}

void Application::ResetProject() {
    // どれも GPU 待機を伴う。フレームの外から呼ぶこと。
    m_materialLibrary.Clear(m_device);
    m_skyLibrary.Clear(m_device);
    m_skyLibrary.EnsureDefault();
    m_textureLibrary.Clear(m_device);

    // グラフを既定へ戻す。位置はエディタへ流し込み直す。
    // メッシュシーンは次のフレームの SyncMeshGraph が作り直す（改版を 0 に戻す）。
    m_graph = graph::NodeGraph::CreateDefault();
    m_surfaceLayouts = {};
    m_layerThumbnailsDirty = true; ++m_layerThumbnailTextureRevision;
    m_previewSurfaceBands = m_connectSurfaceBands = m_displaceConnectedBands = false;
    m_editSurfacePreset = 0;
    m_selectedBoundaryMaterial = m_editBoundaryMaterial = 0;
    m_surfacePresetError.clear();
    m_selectedGraphNode = 0;
    m_previewGraphNode = 0;
    m_previewGraphPin = 0;
    m_meshGraphRevision = 0;
    m_meshGraphActive = false;
    m_meshGraphError.clear();
    RequestGraphNodePlacement();

    // **プレビュー設定も既定へ戻す。** テセレーション・カメラ・ライト・露出・
    // 被写界深度はプロジェクトが持つ値なので、戻さないと前の中身が残る。
    m_renderer.ResetSettings();

    m_selectedMaterial = 0;
    m_selectedTexture = 0;
    m_ordTexture = compositor::kNoTexture;

    // 別の文書になるので履歴は捨てる。戻せてしまうと中身が混ざる。
    m_undoHistory.Clear();
    m_documentDirty = false;
    m_pendingHistoryStep = 0;
    m_committed = CaptureDocument();
}

void Application::UpdateWindowTitle() {
    std::wstring title;
    if (!m_projectPath.empty()) {
        title = m_projectPath.filename().wstring() + L" - ";
    }
    title += L"Road Editor";
    m_window.SetTitle(title.c_str());
}

void Application::ProcessPendingFileWork() {
    // どれもリソースの生成・破棄と GPU 待機を伴う。フレームの外で処理すること。

    // アンドゥ / リドゥ。マテリアルの破棄を伴うのでここで処理する。
    if (m_pendingHistoryStep != 0) {
        const int step = m_pendingHistoryStep;
        m_pendingHistoryStep = 0;

        const DocumentSnapshot current = CaptureDocument();
        if (step < 0 && m_undoHistory.CanUndo()) {
            ApplyDocument(m_undoHistory.Undo(current));
        } else if (step > 0 && m_undoHistory.CanRedo()) {
            ApplyDocument(m_undoHistory.Redo(current));
        }
        m_committed = CaptureDocument();
    }

    if (m_pendingProjectNew) {
        m_pendingProjectNew = false;
        ResetProject();
        m_projectPath.clear();
        UpdateWindowTitle();
    }

    if (!m_pendingProjectOpen.empty()) {
        const std::filesystem::path path = m_pendingProjectOpen;
        m_pendingProjectOpen.clear();

        io::ProjectRefs refs{m_textureLibrary, m_materialLibrary, m_skyLibrary,
                             m_renderer, m_graph, m_surfaceLayouts,
                             m_previewSurfaceBands, m_connectSurfaceBands, m_displaceConnectedBands};
        if (io::LoadProject(path, m_device, m_pipelineCache, refs)) {
            m_layerThumbnailsDirty = true; ++m_layerThumbnailTextureRevision;
            m_recentProjects.Add(path);
            m_projectPath = path;
            m_selectedGraphNode = m_graph.FindNode(m_options.selectNode) ? m_options.selectNode : 0;
            m_options.selectNode = 0;
            m_editSurfacePreset = graph::PresetLayerMaterial(m_surfaceLayouts, m_options.editPreset);
            if (!m_editSurfacePreset) m_editSurfacePreset = m_options.editPreset;
            m_options.editPreset = 0;
            m_selectedBoundaryMaterial = m_editBoundaryMaterial = m_options.editBoundary;
            m_options.editBoundary = 0;
            m_surfacePresetError.clear();
            m_pathEdit = PathEditState{};
            m_pathEdit.nodeId = m_selectedGraphNode;
            if (m_options.selectPathPoint != 0) m_pathEdit.selected = {m_options.selectPathPoint};
            if (!m_options.selectPathPoints.empty()) m_pathEdit.selected = std::move(m_options.selectPathPoints);
            if (m_options.profileMode != 0) {
                m_pathEdit.profileMode = std::clamp(m_options.profileMode, 0, 2);
                m_pathEdit.selectedProfile = m_options.selectPathPoint;
                m_pathEdit.selected.clear();
            }
            m_options.selectPathPoint = 0;
            m_options.profileMode = 0;
            m_previewGraphNode = 0;
            m_previewGraphPin = 0;
            m_meshGraphRevision = 0;
            m_meshGraphActive = false;
            m_meshGraphError.clear();
            RequestGraphNodePlacement();
            m_selectedMaterial = 0;
            m_selectedTexture = 0;
            m_ordTexture = compositor::kNoTexture;
            // 読み込んだ文書が新しい起点になる。前の文書の履歴は捨てる。
            m_undoHistory.Clear();
            m_documentDirty = false;
            m_pendingHistoryStep = 0;
            m_committed = CaptureDocument();
            UpdateWindowTitle();
        } else {
            // 消えた / 壊れたプロジェクトを履歴に残しても、選べるだけで意味がない。
            m_recentProjects.Remove(path);
        }
    }

    if (!m_pendingProjectSave.empty()) {
        const std::filesystem::path path = m_pendingProjectSave;
        m_pendingProjectSave.clear();

        io::ProjectRefs refs{m_textureLibrary, m_materialLibrary, m_skyLibrary,
                             m_renderer, m_graph, m_surfaceLayouts,
                             m_previewSurfaceBands, m_connectSurfaceBands, m_displaceConnectedBands};
        if (io::SaveProject(path, refs)) {
            m_recentProjects.Add(path);
            m_projectPath = path;
            UpdateWindowTitle();
        }
    }

    if (!m_pendingMaterialExport.empty()) {
        const std::filesystem::path path = m_pendingMaterialExport;
        const compositor::MaterialAssetId id = m_pendingExportMaterial;
        m_pendingMaterialExport.clear();
        m_pendingExportMaterial = compositor::kNoMaterialAsset;

        if (const compositor::MaterialAsset* asset = m_materialLibrary.Find(id);
            asset != nullptr) {
            io::SaveMaterial(path, *asset, m_textureLibrary);
        }
    }

    if (!m_pendingMaterialImport.empty()) {
        const std::filesystem::path path = m_pendingMaterialImport;
        m_pendingMaterialImport.clear();

        const compositor::MaterialAssetId id = io::LoadMaterial(
            path, m_device, m_pipelineCache, m_textureLibrary, m_materialLibrary);
        if (id != compositor::kNoMaterialAsset) {
            m_selectedMaterial = static_cast<int>(m_materialLibrary.Entries().size()) - 1;
        }
    }

    if (m_pendingTextureRemove != compositor::kNoTexture) {
        const compositor::TextureId removed = m_pendingTextureRemove;
        m_pendingTextureRemove = compositor::kNoTexture;

        // 参照を先に外す。無効な ID を残すと、次に同じ番号が払い出されたときに
        // 別の画像が割り当たってしまう。
        const auto clearSlot = [removed](compositor::TextureId& slot) {
            const bool hit = (slot == removed);
            if (hit) {
                slot = compositor::kNoTexture;
            }
            return hit;
        };
        const auto clearMap = [removed](compositor::MapSlot& slot) {
            const bool hit = (slot.texture == removed);
            if (hit) {
                slot = compositor::MapSlot{};
            }
            return hit;
        };

        for (const compositor::MaterialAsset& entry : m_materialLibrary.Entries()) {
            compositor::MaterialAsset* asset = m_materialLibrary.FindMutable(entry.id);
            bool hit = clearSlot(asset->baseColor);
            hit |= clearSlot(asset->normal);
            hit |= clearMap(asset->roughness);
            hit |= clearMap(asset->metallic);
            hit |= clearMap(asset->ambientOcclusion);
            hit |= clearMap(asset->height);
            if (hit) {
                asset->thumbnailDirty = true;
            }
        }
        clearSlot(m_ordTexture);
        bool boundaryChanged = false;
        for (auto& boundary : m_surfaceLayouts.boundaryMaterials) {
            boundaryChanged |= clearSlot(boundary.mask); boundaryChanged |= clearSlot(boundary.height);
        }
        if (boundaryChanged) m_graph.MarkDirty();

        // 解放は DeferRelease でフレーム同期後に行われるため、GPU 待機は不要。
        m_textureLibrary.Remove(m_device, removed);
        m_renderer.InvalidateSceneMaterials();
    }

    if (m_pendingMaterialRemove != compositor::kNoMaterialAsset) {
        const compositor::MaterialAssetId removed = m_pendingMaterialRemove;
        m_pendingMaterialRemove = compositor::kNoMaterialAsset;

        if (m_materialLibrary.Find(removed) != nullptr) {
            m_materialLibrary.Remove(m_device, removed);
            // 参照していたノードは「なし」へ戻す。無効な ID を残さない。
            bool graphChanged = false;
            for (graph::Node& node : m_graph.MutableNodes()) {
                graph::VisitNodeMaterialLayers(node, [&](compositor::MaterialLayer& layer) {
                    if (layer.material == removed) {
                        layer.material = compositor::kNoMaterialAsset;
                        graphChanged = true;
                    }
                });
            }
            if (graphChanged) {
                m_graph.MarkDirty();
            }
            m_selectedMaterial = std::max(0, m_selectedMaterial - 1);
            MarkDocumentChanged();
        }
    }

    if (m_pendingSkyRemove != renderer::kNoSkyAsset) {
        const renderer::SkyAssetId removed = m_pendingSkyRemove;
        m_pendingSkyRemove = renderer::kNoSkyAsset;
        // 消したのが適用中の天球なら、SkyLibrary が隣へ移す。
        // 環境の作り直しは、次のフレームの SetActiveSky が判断する。
        m_skyLibrary.Remove(m_device, removed);
    }
}

}  // namespace tg
