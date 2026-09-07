#include "app/Application.h"
#include "app/ApplicationUiHelpers.h"
#include "app/RoadMaskUi.h"
#include "graph/SurfaceLayoutEditing.h"
#include "graph/SurfaceLayoutEvaluation.h"
#include "graph/SurfaceBandGeometry.h"
#include "ui/UiStyle.h"
#include <algorithm>
#include <cstdio>

namespace tg {
void Application::DrawSurfacePresetEditor() {
    if (ui::Button("配置へ戻る", ui::kWideButtonWidth)) { m_editSurfacePreset = 0; return; }
    auto edited = m_surfaceLayouts;
    auto found = std::find_if(edited.presets.begin(), edited.presets.end(), [&](const auto& p) { return p.id == m_editSurfacePreset; });
    if (found == edited.presets.end()) { ui::HintText("プリセットが削除されました。配置へ戻って選び直してください"); return; }
    auto& preset = *found;
    if (preset.role != graph::SurfaceRole::Road && !m_previewSurfaceBands) {
        m_previewSurfaceBands = true; m_graph.MarkDirty();
    }
    ui::SectionHeader("プリセット編集");
    ui::HintText("ここでの変更は、このプリセットを使うすべての区間へ反映します");
    if (!m_surfacePresetError.empty()) ui::HintText(m_surfacePresetError.c_str());
    bool changed = false;
    ImGui::BeginDisabled(preset.materials.size() >= 4);
    if (ui::Button("レイヤーを追加", ui::kWideButtonWidth)) {
        preset.materials.emplace_back();
        m_surfaceLayoutLayer = static_cast<int>(preset.materials.size()) - 1;
        auto& added = preset.materials.back();
        added.mask.emplace(); added.mask->shape = graph::RoadMaskShape::Constant;
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    m_surfaceLayoutLayer = std::clamp(m_surfaceLayoutLayer, 0, static_cast<int>(preset.materials.size()) - 1);
    ImGui::BeginDisabled(m_surfaceLayoutLayer == 0);
    if (ui::Button("選択層を削除", ui::kWideButtonWidth)) {
        preset.materials.erase(preset.materials.begin() + m_surfaceLayoutLayer);
        m_surfaceLayoutLayer = std::max(0, m_surfaceLayoutLayer - 1); changed = true;
    }
    ImGui::EndDisabled();
    ui::HintText("下地から層4の順に重ねます。上層ごとにマスクとハイトの混ぜ方を設定できます");
    if (ui::BeginPropertyTable("presetEditorRows")) {
        char name[128]; std::snprintf(name, sizeof(name), "%s", preset.name.c_str());
        if (ui::PropertyTextInput("名前", name, sizeof(name), "プリセット一覧に表示する名前") && name[0]) { preset.name = name; changed = true; }
        const graph::SurfacePreset defaults;
        changed |= ui::PropertyFloat("凹凸の高さ", &preset.displacementMeters, 0, 10, defaults.displacementMeters,
            "素材のハイトで押し出す量。0なら形状を変えない", "%.3f m");
        const char* layers[] = {"下地", "層 2", "層 3", "層 4"};
        m_surfaceLayoutLayer = std::clamp(m_surfaceLayoutLayer, 0, static_cast<int>(preset.materials.size()) - 1);
        ui::PropertyCombo("編集する層", &m_surfaceLayoutLayer, layers, static_cast<int>(preset.materials.size()), 0, "プリセット内部で編集する材質層");
        auto& material = preset.materials[m_surfaceLayoutLayer];
        const graph::PresetMaterial materialDefaults;
        changed |= DrawMaterialSlotRow("素材", material.material, m_materialLibrary);
        changed |= ui::PropertyFloat("反復長", &material.uvRepeatMeters, 0.01f, 100, materialDefaults.uvRepeatMeters,
            "素材が繰り返す実距離。大きくすると模様が大きくなる", "%.2f m");
        const char* spaces[] = {"面に沿う", "ワールド XZ"};
        int space = material.worldUv ? 1 : 0;
        if (ui::PropertyCombo("座標", &space, spaces, 2, materialDefaults.worldUv ? 1 : 0,
            "面に沿う: 道路の曲がりに追従。ワールド XZ: 地面や隣の面と同じ座標で素材を配置")) {
            material.worldUv = space == 1; changed = true;
        }
        changed |= ui::PropertyFloat("ブレンド幅", &preset.layerBlendRange, 0, 1, defaults.layerBlendRange,
            "プリセット内の全層に共通する、ハイトによる境界の柔らかさ");
        if (!material.material) {
            changed |= ui::PropertyColorLinear("色", material.baseColor.data(), materialDefaults.baseColor.data(), "素材未指定時の路面色");
            changed |= ui::PropertyFloat("粗さ", &material.roughness, 0, 1, materialDefaults.roughness, "大きいほど反射がぼける");
            changed |= ui::PropertyFloat("金属度", &material.metallic, 0, 1, materialDefaults.metallic, "素材未指定時の金属の割合");
            changed |= ui::PropertyFloat("AO", &material.ambientOcclusion, 0, 1, materialDefaults.ambientOcclusion, "素材未指定時の環境光の遮蔽。1で遮蔽なし");
        }
        if (m_surfaceLayoutLayer > 0) {
            bool enabled = material.mask.has_value();
            if (ui::PropertyBool("層を使用", &enabled, false, "マスクを使って上層を表示する")) {
                if (enabled) { material.mask.emplace(); material.mask->shape = graph::RoadMaskShape::Constant; }
                else material.mask.reset();
                changed = true;
            }
            if (material.mask) {
                const char* modes[] = {"マスクどおり", "ハイトで競合"};
                int mode = static_cast<int>(material.blendMode);
                if (ui::PropertyCombo("混ぜ方", &mode, modes, 2, static_cast<int>(materialDefaults.blendMode),
                    "マスクどおり: 被覆率で混ぜる。ハイトで競合: 素材の高い部分を優先する")) {
                    material.blendMode = static_cast<uint32_t>(mode); changed = true;
                }
                const char* gates[] = {"使わない", "下地の高い所", "下地の低い所"};
                int gate = static_cast<int>(material.heightGate);
                if (ui::PropertyCombo("下地のハイト", &gate, gates, 3, static_cast<int>(materialDefaults.heightGate),
                    "下地の凹凸で上層を絞る。粒の露出や低い所に溜まる土を表現する")) {
                    material.heightGate = static_cast<uint32_t>(gate); changed = true;
                }
                if (material.heightGate) {
                    changed |= ui::PropertyFloat("高さのしきい値", &material.heightGateThreshold, 0, 1,
                        materialDefaults.heightGateThreshold, "下地のハイト0〜1のうち、境界にする高さ");
                    changed |= ui::PropertyFloat("高さの柔らかさ", &material.heightGateSoftness, 0.001f, 1,
                        materialDefaults.heightGateSoftness, "高さ条件の境界をぼかす幅");
                }
                ImGui::PushID("presetMask");
                changed |= DrawRoadMaskPropertyRows(*material.mask);
                ImGui::PopID();
            }
        }
        float width, height;
        if (graph::GetSimpleRoadsideDimensions(preset, width, height)) {
            const graph::SimpleRoadsideDefaults shapeDefaults;
            const bool step = preset.section.size() == 3;
            bool dimensionsChanged = ui::PropertyFloat("幅", &width, 0.1f, 10, shapeDefaults.width,
                "共有断面の幅。使用中の全区間へ反映", "%.2f m");
            dimensionsChanged |= ui::PropertyFloat(step ? "段差の高さ" : "外端の高さ", &height,
                step ? 0.01f : -2.0f, 2, step ? shapeDefaults.sidewalkHeight : shapeDefaults.groundHeight,
                "道路端を固定した断面の高さ。使用中の全区間へ反映", "%.2f m");
            if (dimensionsChanged) changed |= graph::SetSimpleRoadsideDimensions(preset, width, height);
        }
        ui::EndPropertyTable();
    }
    if (!changed) return;
    std::string error;
    if (graph::ValidateSurfaceLayouts(edited, error)) {
        for (const auto& layout : edited.layouts) for (const auto& band : layout.bands) {
            if (!error.empty() || std::none_of(band.spans.begin(), band.spans.end(), [&](const auto& span) { return span.preset == preset.id; })) continue;
            error = band.side == graph::SurfaceSide::Road
                ? graph::CompileSurfaceLayoutPreview(m_graph, edited, layout.roadNode).error
                : graph::CompileSurfaceBandPreview(m_graph, edited, layout.roadNode, band.id).error;
        }
    }
    m_surfacePresetError = error;
    if (error.empty()) {
        m_surfaceLayouts = std::move(edited);
        m_graph.MarkDirty(); MarkDocumentChanged();
    }
}
}  // namespace tg
