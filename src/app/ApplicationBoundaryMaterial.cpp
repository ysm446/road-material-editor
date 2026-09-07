#include "app/Application.h"
#include "app/ApplicationUiHelpers.h"
#include "ui/UiStyle.h"
#include "core/Log.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cstdio>

namespace tg {
void Application::DrawBoundaryMaterialLibrary() {
    if (const auto* window = ImGui::FindWindowByName("レイヤーマテリアル"); window && window->DockId)
        ImGui::SetNextWindowDockID(window->DockId, ImGuiCond_FirstUseEver);
    bool create = false, remove = false;
    if (ImGui::Begin("境界マテリアル")) {
        const auto menu = [&](bool target) {
            if (ImGui::MenuItem("新規作成")) create = true;
            if (ImGui::MenuItem("編集", nullptr, false, target)) m_editBoundaryMaterial = m_selectedBoundaryMaterial;
            if (ImGui::MenuItem("削除", "DEL", false, target)) remove = true;
        };
        const float size = ui::Scaled(84);
        const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (size + ImGui::GetStyle().ItemSpacing.x)));
        int index = 0;
        for (const auto& material : m_surfaceLayouts.boundaryMaterials) {
            ImGui::PushID(static_cast<int>(material.id)); ImGui::BeginGroup();
            const auto* texture = m_textureLibrary.Find(material.mask ? material.mask : material.height);
            const auto clicked = ui::ThumbnailButton("boundary", texture ? static_cast<ImTextureID>(texture->ChannelHandle(0).ptr) : 0,
                size, m_selectedBoundaryMaterial == material.id);
            if (clicked.clicked) m_selectedBoundaryMaterial = material.id;
            if (clicked.doubleClicked) m_editBoundaryMaterial = material.id;
            if (clicked.hovered) ImGui::SetTooltip("%s\nダブルクリックで編集", material.name.c_str());
            if (ImGui::BeginPopupContextItem("boundaryMenu")) {
                m_selectedBoundaryMaterial = material.id; menu(true); ImGui::EndPopup();
            }
            ui::GridCaption(material.name.c_str(), size);
            ImGui::EndGroup(); ImGui::PopID();
            if (++index % columns && index < static_cast<int>(m_surfaceLayouts.boundaryMaterials.size())) ImGui::SameLine();
        }
        if (ImGui::BeginPopupContextWindow("boundaryBackground", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
            menu(false); ImGui::EndPopup();
        }
        remove |= ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
            ImGui::IsKeyPressed(ImGuiKey_Delete, false);
        if (remove && m_selectedBoundaryMaterial) {
            bool used = false;
            for (const auto& layout : m_surfaceLayouts.layouts) for (const auto& band : layout.bands)
                for (const auto& span : band.spans) used |= span.boundaryMaterial == m_selectedBoundaryMaterial;
            if (used) ImGui::OpenPopup("使用中の境界");
            else {
                std::erase_if(m_surfaceLayouts.boundaryMaterials, [&](const auto& m) { return m.id == m_selectedBoundaryMaterial; });
                if (m_editBoundaryMaterial == m_selectedBoundaryMaterial) m_editBoundaryMaterial = 0;
                m_selectedBoundaryMaterial = 0; MarkDocumentChanged();
            }
        }
        if (ImGui::IsPopupOpen("使用中の境界")) ImGui::SetNextWindowSize(ImVec2(ui::Scaled(400), 0));
        if (ImGui::BeginPopup("使用中の境界")) {
            ui::HintText("沿道で使用中です。割り当てを解除してから削除してください。"); ImGui::EndPopup();
        }
    }
    ImGui::End();
    if (create) {
        compositor::BoundaryMaterial material;
        material.id = m_surfaceLayouts.AllocateId(); material.name = "新しい境界マテリアル";
        if (material.id) {
            m_selectedBoundaryMaterial = m_editBoundaryMaterial = material.id;
            m_surfaceLayouts.boundaryMaterials.push_back(material); MarkDocumentChanged();
        }
    }
    const auto found = std::find_if(m_surfaceLayouts.boundaryMaterials.begin(), m_surfaceLayouts.boundaryMaterials.end(),
        [&](const auto& m) { return m.id == m_editBoundaryMaterial; });
    if (!m_editBoundaryMaterial || found == m_surfaceLayouts.boundaryMaterials.end()) return;
    auto edited = *found;
    bool open = true, changed = false;
    ImGui::SetNextWindowSize(ImVec2(ui::Scaled(540), ui::Scaled(600)), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("境界マテリアル編集", &open)) {
        ui::HintText("道路端のマスクと溝を共有します。形状は道路ビューポートで確認できます。");
        if (ui::BeginPropertyTable("boundaryProperties")) {
            const compositor::BoundaryMaterial defaults;
            char name[128]; std::snprintf(name, sizeof(name), "%s", edited.name.c_str());
            if (ui::PropertyTextInput("名前", name, sizeof(name), "境界の一覧に表示する名前") && name[0]) { edited.name = name; changed = true; }
            changed |= DrawTextureSlotRow("境界マスク", edited.mask, m_textureLibrary);
            changed |= DrawTextureSlotRow("ハイト", edited.height, m_textureLibrary);
            changed |= ui::PropertyFloat("境界幅", &edited.widthMeters, 0.02f, 2, defaults.widthMeters, "道路端を中心とする帯の幅", "%.2f m");
            changed |= ui::PropertyFloat("繰り返し長", &edited.repeatMeters, 0.05f, 50, defaults.repeatMeters, "道路に沿う画像の繰り返し間隔", "%.2f m");
            changed |= ui::PropertyFloat("溝の深さ", &edited.depthMeters, 0, 0.5f, defaults.depthMeters, "基準0.5、黒0のときの深さ。凹凸接続オンで形状へ反映", "%.3f m");
            changed |= ui::PropertyFloat("ハイト基準", &edited.heightCenter, 0, 1, defaults.heightCenter, "この値を変位ゼロとする");
            const char* axes[] = {"V方向", "U方向"}; int axis = edited.alongU ? 1 : 0;
            if (ui::PropertyCombo("反復方向", &axis, axes, 2, 0, "画像内で道路に沿って繰り返す軸")) { edited.alongU = axis == 1; changed = true; }
            changed |= ui::PropertyBool("マスクを反転", &edited.invertMask, defaults.invertMask, "通常は白が路面、黒が沿道");
            ui::EndPropertyTable();
        }
        ui::HintText("画像はRチャンネルを使用。段差保持の区間には適用しません。");
        for (const auto id : {edited.mask, edited.height}) {
            if (const auto* texture = m_textureLibrary.Find(id)) {
                ImGui::Image(static_cast<ImTextureID>(texture->ChannelHandle(0).ptr), ImVec2(ui::Scaled(180), ui::Scaled(180)));
                ImGui::SameLine();
            }
        }
        ImGui::NewLine();
    }
    ImGui::End();
    if (!open) m_editBoundaryMaterial = 0;
    if (changed) {
        auto proposed = m_surfaceLayouts;
        const auto target = std::find_if(proposed.boundaryMaterials.begin(), proposed.boundaryMaterials.end(),
            [&](const auto& m) { return m.id == edited.id; });
        *target = edited;
        std::string error;
        if (graph::ValidateSurfaceLayouts(proposed, error)) {
            m_surfaceLayouts = std::move(proposed); m_graph.MarkDirty(); MarkDocumentChanged();
        } else TG_LOG_ERROR("境界マテリアル: %s", error.c_str());
    }
}
}
