#include "app/Application.h"
#include "app/ApplicationUiHelpers.h"
#include "app/RoadMaskUi.h"
#include "graph/SurfaceLayoutEditing.h"
#include "graph/SurfaceLayoutEvaluation.h"
#include "graph/SurfaceBandGeometry.h"
#include "ui/UiStyle.h"
#include "graph/SurfacePresetGraph.h"
#include <imgui-node-editor/imgui_node_editor.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tg {
namespace ed = ax::NodeEditor;
namespace {
const char* PresetNodeLabel(graph::PresetNodeKind kind) {
    switch (kind) {
        case graph::PresetNodeKind::Material: return "素材";
        case graph::PresetNodeKind::Mask: return "マスク";
        case graph::PresetNodeKind::Blend: return "合成";
        default: return "出力";
    }
}
void PresetPin(uint32_t id, const char* label, bool output, bool mask) {
    ed::BeginPin(ed::PinId(id), output ? ed::PinKind::Output : ed::PinKind::Input);
    const auto color = ImGui::GetStyleColorVec4(mask ? ImGuiCol_PlotHistogramHovered : ImGuiCol_Text);
    if (output) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui::TextScaled(130) - ImGui::CalcTextSize(label).x);
    ImGui::TextColored(color, "%s", label);
    const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const ImVec2 pivot(output ? max.x + 8 : min.x - 8, (min.y + max.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddCircleFilled(pivot, 4, ImGui::ColorConvertFloat4ToU32(color));
    ed::PinPivotRect(pivot, pivot);
    ed::PinRect(ImVec2(min.x - 14, min.y), ImVec2(max.x + 14, max.y));
    ed::EndPin();
}
}
bool Application::DrawSurfacePresetGraph(graph::SurfacePreset& preset) {
    auto& graph = *preset.materialGraph;
    bool changed = false;
    bool frameAll = false;
    if (!m_presetNodeEditor || m_presetEditorId != preset.id) {
        if (m_presetNodeEditor) ed::DestroyEditor(m_presetNodeEditor);
        ed::Config config{}; config.SettingsFile = nullptr; config.NavigateButtonIndex = 2;
        m_presetNodeEditor = ed::CreateEditor(&config);
        m_presetEditorId = preset.id;
        m_selectedPresetNode = graph.nodes.front().id;
        frameAll = true;
    }
    ed::SetCurrentEditor(m_presetNodeEditor);
    if (frameAll) { ed::ClearSelection(); ed::SelectNode(ed::NodeId(m_selectedPresetNode)); }
    if (ui::Button("層を追加", 100)) {
        if (graph::AppendPresetLayer(graph, m_surfacePresetError)) {
            changed = true; frameAll = true;
            m_selectedPresetNode = graph.nodes.back().id;
            ed::ClearSelection(); ed::SelectNode(ed::NodeId(m_selectedPresetNode));
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(graph.nodes.size() >= 32);
    for (const auto kind : {graph::PresetNodeKind::Material, graph::PresetNodeKind::Mask, graph::PresetNodeKind::Blend}) {
        if (kind != graph::PresetNodeKind::Material) ImGui::SameLine();
        const std::string label = std::string(PresetNodeLabel(kind)) + "を追加";
        if (ui::Button(label.c_str(), 100)) {
            const auto position = ed::ScreenToCanvas(ImGui::GetCursorScreenPos());
            m_selectedPresetNode = graph::AddPresetNode(graph, kind, {position.x + 40, position.y + 100});
            ed::SetNodePosition(ed::NodeId(m_selectedPresetNode), ImVec2(position.x + 40, position.y + 100));
            ed::ClearSelection(); ed::SelectNode(ed::NodeId(m_selectedPresetNode));
            changed = true;
        }
    }
    ImGui::EndDisabled();
    if (ui::Button("全体を表示", 100)) frameAll = true;
    ImGui::SameLine();
    if (ui::Button("選択を削除", 100)) changed |= graph::DeletePresetNode(graph, m_selectedPresetNode);
    ui::HintText("丸をドラッグして接続。合成の上層には素材を接続します（最大4層）");
    const float height = std::clamp(ImGui::GetContentRegionAvail().y * 0.48f, ui::Scaled(180), ui::Scaled(370));
    const auto screenMin = ImGui::GetCursorScreenPos();
    const ImVec2 screenMax(screenMin.x + ImGui::GetContentRegionAvail().x, screenMin.y + height);
    auto gridColor = ImGui::GetStyleColorVec4(ImGuiCol_Border); gridColor.w = 0;
    ed::PushStyleColor(ed::StyleColor_Bg, gridColor);
    ed::PushStyleColor(ed::StyleColor_Grid, gridColor);
    ed::Begin("presetMaterialGraph", ImVec2(0, height));
    DrawGraphBackground(screenMin, screenMax);
    const bool moving = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    for (const auto& node : graph.nodes) {
        const ed::NodeId id(node.id);
        if (!moving || frameAll) ed::SetNodePosition(id, ImVec2(node.position[0], node.position[1]));
        ed::PushStyleColor(ed::StyleColor_NodeBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        ed::PushStyleColor(ed::StyleColor_NodeBorder, ImGui::GetStyleColorVec4(ImGuiCol_Border));
        ed::BeginNode(id);
        ImGui::TextUnformatted(PresetNodeLabel(node.kind));
        ImGui::Dummy(ImVec2(ui::TextScaled(130), 3));
        if (node.kind == graph::PresetNodeKind::Material) {
            const auto* asset = m_materialLibrary.Find(node.settings.material);
            ui::ThumbnailImage(static_cast<ImTextureID>(m_materialLibrary.ThumbnailHandle(node.settings.material).ptr),
                ui::Scaled(ui::kNodeThumbnail));
            const char* name = asset ? asset->name.c_str() : "定数材質";
            std::string label = name;
            while (!label.empty() && ImGui::CalcTextSize(label.c_str()).x > ui::TextScaled(130)) {
                size_t end = label.size() - 1;
                while (end && (static_cast<unsigned char>(label[end]) & 0xC0) == 0x80) --end;
                label.resize(end);
            }
            ImGui::TextUnformatted(label.c_str());
        }
        if (node.kind == graph::PresetNodeKind::Blend || node.kind == graph::PresetNodeKind::Output)
            PresetPin(node.id * 8 + 1, node.kind == graph::PresetNodeKind::Output ? "面" : "下地", false, false);
        if (node.kind == graph::PresetNodeKind::Blend) {
            PresetPin(node.id * 8 + 2, "上層素材", false, false);
            PresetPin(node.id * 8 + 3, "マスク", false, true);
            ImGui::TextUnformatted(node.settings.blendMode ? "ハイトで競合" : "マスクどおり");
        }
        if (node.kind != graph::PresetNodeKind::Output)
            PresetPin(node.id * 8 + 4, node.kind == graph::PresetNodeKind::Mask ? "マスク出力" : "材質出力", true,
                node.kind == graph::PresetNodeKind::Mask);
        ed::EndNode(); ed::PopStyleColor(2);
    }
    if (frameAll || changed) ed::SelectNode(ed::NodeId(m_selectedPresetNode));
    for (const auto& node : graph.nodes) for (uint32_t input = 0; input < 3; ++input) if (node.inputs[input]) {
        const auto color = ImGui::GetStyleColorVec4(input == 2 ? ImGuiCol_PlotHistogramHovered : ImGuiCol_Text);
        ed::Link(ed::LinkId(node.id * 8 + input + 1), ed::PinId(node.inputs[input] * 8 + 4),
            ed::PinId(node.id * 8 + input + 1), color, 2);
    }
    if (ed::BeginCreate()) {
        ed::PinId a, b;
        if (ed::QueryNewLink(&a, &b) && a && b) {
            uint32_t source = static_cast<uint32_t>(a.Get()), target = static_cast<uint32_t>(b.Get());
            if (target % 8 == 4) std::swap(source, target);
            auto candidate = graph;
            std::string error;
            if (source % 8 == 4 && target % 8 >= 1 && target % 8 <= 3 &&
                graph::ConnectPresetNodes(candidate, source / 8, target / 8, target % 8 - 1, error)) {
                if (ed::AcceptNewItem()) { graph = std::move(candidate); changed = true; m_surfacePresetError.clear(); }
            } else {
                ed::RejectNewItem();
                m_surfacePresetError = error.empty() ? "出力と入力の丸を接続してください" : error;
            }
        }
    }
    ed::EndCreate();
    if (ed::BeginDelete()) {
        ed::LinkId link;
        while (ed::QueryDeletedLink(&link)) if (ed::AcceptDeletedItem()) {
            const auto id = static_cast<uint32_t>(link.Get());
            std::string error;
            changed |= graph::ConnectPresetNodes(graph, 0, id / 8, id % 8 - 1, error);
        }
        ed::NodeId node;
        while (ed::QueryDeletedNode(&node)) {
            const auto id = static_cast<uint32_t>(node.Get());
            const auto found = std::find_if(graph.nodes.begin(), graph.nodes.end(), [id](const auto& n) { return n.id == id; });
            if (found != graph.nodes.end() && found->kind != graph::PresetNodeKind::Output) {
                if (ed::AcceptDeletedItem()) changed |= graph::DeletePresetNode(graph, id);
            } else ed::RejectDeletedItem();
        }
    }
    ed::EndDelete();
    ed::NodeId selected;
    if (ed::GetSelectedNodes(&selected, 1)) m_selectedPresetNode = static_cast<uint32_t>(selected.Get());
    for (auto& node : graph.nodes) {
        const auto position = ed::GetNodePosition(ed::NodeId(node.id));
        if (moving && (std::abs(position.x - node.position[0]) > 0.1f || std::abs(position.y - node.position[1]) > 0.1f)) {
            node.position = {position.x, position.y}; changed = true;
        }
    }
    if (frameAll) ed::NavigateToContent(0);
    ed::End(); ed::PopStyleColor(2); ed::SetCurrentEditor(nullptr);
    return changed;
}
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
    if (!preset.materialGraph) preset.materialGraph = graph::MakePresetGraph(preset.materials);
    bool changed = DrawSurfacePresetGraph(preset);
    ImGui::BeginChild("presetProperties", ImVec2(0, 0));
    if (ui::BeginPropertyTable("presetEditorRows")) {
        char name[128]; std::snprintf(name, sizeof(name), "%s", preset.name.c_str());
        if (ui::PropertyTextInput("名前", name, sizeof(name), "プリセット一覧に表示する名前") && name[0]) { preset.name = name; changed = true; }
        const graph::SurfacePreset defaults;
        changed |= ui::PropertyFloat("凹凸の高さ", &preset.displacementMeters, 0, 10, defaults.displacementMeters,
            "素材のハイトで押し出す量。0なら形状を変えない", "%.3f m");
        auto selected = std::find_if(preset.materialGraph->nodes.begin(), preset.materialGraph->nodes.end(),
            [&](const auto& n) { return n.id == m_selectedPresetNode; });
        if (selected != preset.materialGraph->nodes.end()) {
            ui::PropertyValue("編集対象", "%s", PresetNodeLabel(selected->kind));
            auto& material = selected->settings;
            const auto kind = selected->kind;
            const graph::PresetMaterial materialDefaults;
            if (kind == graph::PresetNodeKind::Material) {
                changed |= DrawMaterialSlotRow("素材", material.material, m_materialLibrary, true);
                changed |= ui::PropertyFloat("反復長", &material.uvRepeatMeters, 0.01f, 100, materialDefaults.uvRepeatMeters,
                "素材が繰り返す実距離。大きくすると模様が大きくなる", "%.2f m");
                const char* spaces[] = {"面に沿う", "ワールド XZ"};
                int space = material.worldUv ? 1 : 0;
                if (ui::PropertyCombo("座標", &space, spaces, 2, materialDefaults.worldUv ? 1 : 0,
                "面に沿う: 道路の曲がりに追従。ワールド XZ: 地面や隣の面と同じ座標で素材を配置")) {
                    material.worldUv = space == 1; changed = true;
                }
                if (!material.material) {
                    changed |= ui::PropertyColorLinear("色", material.baseColor.data(), materialDefaults.baseColor.data(), "素材未指定時の路面色");
                    changed |= ui::PropertyFloat("粗さ", &material.roughness, 0, 1, materialDefaults.roughness, "大きいほど反射がぼける");
                    changed |= ui::PropertyFloat("金属度", &material.metallic, 0, 1, materialDefaults.metallic, "素材未指定時の金属の割合");
                    changed |= ui::PropertyFloat("AO", &material.ambientOcclusion, 0, 1, materialDefaults.ambientOcclusion, "素材未指定時の環境光の遮蔽。1で遮蔽なし");
                }
            }
            if (kind == graph::PresetNodeKind::Blend) {
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
            }
            if (kind == graph::PresetNodeKind::Mask && material.mask) {
                ImGui::PushID("presetMask");
                changed |= DrawRoadMaskPropertyRows(*material.mask);
                ImGui::PopID();
            }
        }
        changed |= ui::PropertyFloat("ブレンド幅", &preset.layerBlendRange, 0, 1, defaults.layerBlendRange,
            "プリセット内の全層に共通する、ハイトによる境界の柔らかさ");
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
    ImGui::EndChild();
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
