#include "app/Application.h"
#include "graph/SurfaceBandGeometry.h"
#include "app/RoadMaskUi.h"
#include "app/ApplicationUiHelpers.h"
#include "graph/SurfaceLayoutEditing.h"
#include "graph/SurfaceLayoutEvaluation.h"
#include "ui/UiStyle.h"
#include "core/Log.h"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <unordered_set>

namespace tg {
bool Application::DrawSurfaceLayoutSettings(graph::GraphId roadId) {
    ui::SectionHeader("沿道の配置");
    if (!m_surfacePresetError.empty()) ui::HintText(m_surfacePresetError.c_str());
    if (ui::BeginPropertyTable("surfaceBandPreviewRows", 150)) {
        if (ui::PropertyBool("形状を表示", &m_previewSurfaceBands, false,
            "道路に沿って路肩や歩道を表示する。素材のハイトによる凹凸は「凹凸をなじませる」で有効にする")) m_graph.MarkDirty();
        const char* sides[] = {"左沿道", "右沿道"};
        if (ui::PropertyCombo("編集する沿道", &m_surfaceBandSide, sides, 2, 0, "編集する沿道の左右。横接続は左右両方へ適用する")) m_graph.MarkDirty();
        if (ui::PropertyBool("材質をなじませる", &m_connectSurfaceBands, false,
            "道路と左右の沿道の境界で材質を滑らかに混ぜる。形状表示もオンにする。道路は最大3種類、沿道は左右それぞれ最大2種類のプリセットに対応")) {
            if (m_connectSurfaceBands) m_previewSurfaceBands = true;
            m_graph.MarkDirty();
        }
        if (ui::PropertyBool("凹凸をなじませる", &m_displaceConnectedBands, false,
            "素材のハイトによる凹凸を有効にし、境界付近で滑らかに抑える。歩道の段差は保つ。形状表示と材質のなじませもオンにする")) {
            if (m_displaceConnectedBands) { m_connectSurfaceBands = true; m_previewSurfaceBands = true; }
            m_graph.MarkDirty();
        }
        ui::EndPropertyTable();
    }
    const auto side = m_surfaceBandSide == 0 ? graph::SurfaceSide::Left : graph::SurfaceSide::Right;
    if (m_previewSurfaceBands && m_connectSurfaceBands) ui::HintText(m_displaceConnectedBands
        ? "境界付近の凹凸を滑らかに抑え、段差の基準高さへ接続します"
        : "材質の境界をなじませています。素材の凹凸も使う場合は「凹凸をなじませる」をオンにします");
    bool exists = false;
    for (const auto& layout : m_surfaceLayouts.layouts) if (layout.roadNode == roadId)
        for (const auto& candidate : layout.bands) if (candidate.side == side) exists = true;
    if (!exists) {
        if (ui::BeginPropertyTable("roadsideCreateRows")) {
            const char* kinds[] = {"路肩", "歩道"};
            ui::PropertyCombo("作る沿道", &m_surfaceBandCreateRole, kinds, 2, 0, "選んだ側の全長を同じ種類で作成。途中の変更は作成後に区間を分割する");
            ui::EndPropertyTable();
        }
        if (ui::Button("全長に作成", ui::kWideButtonWidth)) {
            std::string error;
            if (graph::CreateUniformRoadside(m_surfaceLayouts, m_graph, roadId, side,
                m_surfaceBandCreateRole == 0 ? graph::SurfaceRole::Ground : graph::SurfaceRole::Sidewalk, error)) {
                m_previewSurfaceBands = true;
                return true;
            }
            TG_LOG_ERROR("沿道形状: %s", error.c_str());
        }
    } else {
        auto roadsideEdit = m_surfaceLayouts;
        for (auto& layout : roadsideEdit.layouts) if (layout.roadNode == roadId) {
            for (auto& candidate : layout.bands) if (candidate.side == side && !candidate.spans.empty()) {
                graph::RoadGeometry road; std::string error;
                if (!graph::EvaluateRoad(m_graph, roadId, road, error)) continue;
                const float length = road.rowDistances.back();
                bool changed = false;
                m_surfaceBandSpan = std::clamp(m_surfaceBandSpan, 0, static_cast<int>(candidate.spans.size()) - 1);
                if (std::abs(candidate.spans.back().endMeters - length) > 0.001f) {
                    ui::HintText("沿道の範囲と道路長が異なります。区間の割合を保って合わせます");
                    if (ui::Button("道路長に合わせる##roadside", ui::kWideButtonWidth)) changed = graph::ResizeSurfaceBand(candidate, length);
                }
                std::vector<std::string> labels;
                for (const auto& span : candidate.spans) {
                    const auto labelPreset = std::find_if(m_surfaceLayouts.layerMaterials.begin(), m_surfaceLayouts.layerMaterials.end(), [&](const auto& p) { return p.id == graph::PresetLayerMaterial(m_surfaceLayouts, span.preset); });
                    char label[192]; std::snprintf(label, sizeof(label), "%.2f～%.2f m / %s", span.startMeters, span.endMeters,
                        labelPreset == m_surfaceLayouts.layerMaterials.end() ? "未設定" : labelPreset->name.c_str());
                    labels.emplace_back(label);
                }
                std::vector<const char*> items;
                for (const auto& label : labels) items.push_back(label.c_str());
                if (ui::BeginPropertyTable("roadsideSpanRows")) {
                    ui::PropertyCombo("編集する区間", &m_surfaceBandSpan, items.data(), static_cast<int>(items.size()), 0, "分割・削除・プリセット編集の対象区間");
                    auto& current = candidate.spans[m_surfaceBandSpan];
                    ui::PropertyValue("始点", "%.2f m", current.startMeters);
                    if (static_cast<size_t>(m_surfaceBandSpan + 1) < candidate.spans.size()) {
                        auto& next = candidate.spans[m_surfaceBandSpan + 1];
                        if (next.endMeters - current.startMeters > 0.1f && ui::PropertyFloat("切替位置", &current.endMeters,
                            current.startMeters + 0.05f, next.endMeters - 0.05f, (current.startMeters + next.endMeters) * 0.5f,
                            "次の沿道区間との境界。両区間を隙間なく動かす", "%.2f m")) {
                            next.startMeters = current.endMeters; graph::ClampSpanBlends(current); graph::ClampSpanBlends(next); changed = true;
                        }
                        float transition = std::min(current.blendOutMeters, next.blendInMeters);
                        const float limit = std::min(current.endMeters - current.startMeters, next.endMeters - next.startMeters) * 0.5f;
                        if (limit >= 0.01f && ui::PropertyFloat("移行距離", &transition, 0.01f, limit, std::min(2.0f, limit),
                            "次の区間との境界の前後で形状と材質を変える距離", "%.2f m")) {
                            current.blendOutMeters = next.blendInMeters = transition; changed = true;
                        }
                    } else ui::PropertyValue("終点", "%.2f m", current.endMeters);
                    ui::EndPropertyTable();
                }
                if (ui::Button("沿道区間を分割", ui::kWideButtonWidth)) changed |= graph::SplitSurfaceSpan(roadsideEdit, candidate, static_cast<size_t>(m_surfaceBandSpan));
                ImGui::SameLine();
                ImGui::BeginDisabled(candidate.spans.size() < 2);
                if (ui::Button("沿道区間を削除", ui::kWideButtonWidth)) {
                    changed |= graph::RemoveSurfaceSpan(candidate, static_cast<size_t>(m_surfaceBandSpan));
                    m_surfaceBandSpan = std::clamp(m_surfaceBandSpan, 0, static_cast<int>(candidate.spans.size()) - 1);
                }
                ImGui::EndDisabled();
                if (changed) {
                    graph::EnsureRoadsideTransitions(candidate);
                    renderer::MeshData mesh;
                    if (graph::BuildSurfaceBandGeometry(road, roadsideEdit, candidate, mesh, error)) {
                        m_surfaceLayouts = std::move(roadsideEdit); m_previewSurfaceBands = true; return true;
                    }
                    TG_LOG_ERROR("沿道区間: %s", error.c_str());
                }
            }
        }
        for (auto& layout : roadsideEdit.layouts) if (layout.roadNode == roadId) {
            for (auto& candidate : layout.bands) if (candidate.side == side && !candidate.spans.empty()) {
                m_surfaceBandSpan = std::clamp(m_surfaceBandSpan, 0, static_cast<int>(candidate.spans.size()) - 1);
                bool changed = false;
                ui::SectionHeader("沿道の材質と形状");
                if (ui::BeginPropertyTable("roadsideMaterialRows")) {
                    auto& span = candidate.spans[m_surfaceBandSpan];
                    std::vector<graph::SurfaceId> presetIds;
                    std::vector<const char*> presetNames;
                    std::vector<ImTextureID> presetThumbnails;
                    int presetIndex = 0;
                    for (const auto& p : roadsideEdit.layerMaterials) {
                        if (p.id == graph::PresetLayerMaterial(roadsideEdit, span.preset)) presetIndex = static_cast<int>(presetIds.size());
                        presetIds.push_back(p.id); presetNames.push_back(p.name.c_str());
                        const auto thumbnail = std::find_if(m_layerThumbnails.begin(), m_layerThumbnails.end(),
                            [&](const auto& entry) { return entry.id == p.id; });
                        presetThumbnails.push_back(thumbnail != m_layerThumbnails.end() && thumbnail->ready
                            ? static_cast<ImTextureID>(thumbnail->texture.srv.gpu.ptr) : 0);
                    }
                    if (!presetIds.empty() && ui::PropertyCombo("材質", &presetIndex, presetNames.data(), static_cast<int>(presetNames.size()), presetIndex,
                        "この区間のレイヤーマテリアル。幅と高さは維持する", presetThumbnails.data())) {
                        changed |= graph::AssignLayerMaterial(roadsideEdit, span, presetIds[presetIndex]);
                    }
                    auto shape = std::find_if(roadsideEdit.presets.begin(), roadsideEdit.presets.end(), [&](const auto& p) { return p.id == span.preset; });
                    float width = 0, height = 0;
                    if (shape != roadsideEdit.presets.end() && graph::GetSimpleRoadsideDimensions(*shape, width, height)) {
                        ui::PropertyValue("形状", "%s", shape->role == graph::SurfaceRole::Sidewalk ? "歩道" : "路肩");
                        const graph::SimpleRoadsideDefaults defaults;
                        bool dimensionsChanged = ui::PropertyFloat("幅", &width, 0.1f, 10, defaults.width,
                            "選択した区間だけの幅", "%.2f m");
                        dimensionsChanged |= ui::PropertyFloat(shape->section.size() == 3 ? "段差の高さ" : "外端の高さ", &height,
                            shape->section.size() == 3 ? 0.01f : -2.0f, 2,
                            shape->role == graph::SurfaceRole::Sidewalk ? defaults.sidewalkHeight : defaults.groundHeight,
                            "路面からの高さ。材質を共有する他の区間には影響しない", "%.2f m");
                        if (dimensionsChanged) {
                            size_t uses = 0;
                            for (const auto& l : roadsideEdit.layouts) for (const auto& b : l.bands)
                                for (const auto& s : b.spans) uses += s.preset == span.preset;
                            if (uses <= 1 || graph::DuplicateSurfacePreset(roadsideEdit, candidate, static_cast<size_t>(m_surfaceBandSpan))) {
                                shape = std::find_if(roadsideEdit.presets.begin(), roadsideEdit.presets.end(), [&](const auto& p) { return p.id == span.preset; });
                                changed |= graph::SetSimpleRoadsideDimensions(*shape, width, height);
                            }
                        }
                    }
                    ui::EndPropertyTable();
                }
                if (ui::Button("材質を編集", ui::kWideButtonWidth)) {
                    m_editSurfacePreset = graph::PresetLayerMaterial(roadsideEdit, candidate.spans[m_surfaceBandSpan].preset); m_surfacePresetError.clear();
                }
                ImGui::SameLine();
                if (ui::Button("複製して編集", ui::kWideButtonWidth)) {
                    changed |= graph::DuplicateLayerMaterial(roadsideEdit, candidate.spans[m_surfaceBandSpan]);
                    if (changed) { m_editSurfacePreset = graph::PresetLayerMaterial(roadsideEdit, candidate.spans[m_surfaceBandSpan].preset); m_surfacePresetError.clear(); }
                }
                if (changed) {
                    graph::EnsureRoadsideTransitions(candidate);
                    // 共有プリセットの変更が別の道路・沿道を壊さないことも確認する。
                    std::string error;
                    for (const auto& checkLayout : roadsideEdit.layouts) for (const auto& checkBand : checkLayout.bands) {
                        if (checkBand.side == graph::SurfaceSide::Road || checkBand.spans.empty() || !error.empty()) continue;
                        if (std::none_of(checkBand.spans.begin(), checkBand.spans.end(), [&](const auto& checkSpan) {
                            return checkSpan.preset == candidate.spans[m_surfaceBandSpan].preset;
                        })) continue;
                        const auto preview = graph::CompileSurfaceBandPreview(m_graph, roadsideEdit, checkLayout.roadNode, checkBand.id);
                        error = preview.error;
                        std::unordered_set<graph::SurfaceId> unique;
                        for (const auto& checkSpan : checkBand.spans) unique.insert(checkSpan.preset);
                        if (error.empty() && m_connectSurfaceBands && unique.size() > 2) error = "横接続中は沿道各2種類までです";
                    }
                    if (error.empty()) { m_surfaceLayouts = std::move(roadsideEdit); m_previewSurfaceBands = true; return true; }
                    m_editSurfacePreset = 0; m_surfacePresetError = error;
                    TG_LOG_ERROR("沿道の材質と形状: %s", error.c_str());
                }
                break;
            }
        }
        ui::HintText("材質のレイヤー合成は「材質を編集」で設定します");
    }
    ui::SectionHeader("路面区間");
    auto edited = m_surfaceLayouts;
    auto* band = graph::FindRoadBand(edited, roadId);
    bool changed = false;
    if (!band || band->spans.empty()) {
        ui::HintText("道路材質を保存して、区間ごとに切り替える");
        if (ui::Button("区間編集を始める", ui::kWideButtonWidth)) {
            std::string error;
            if (graph::CreateRoadLayout(edited, m_graph, roadId, error)) changed = true;
            else TG_LOG_ERROR("路面区間: %s", error.c_str());
        }
    } else {
        ui::HintText("材質とマスクは区間プリセットで編集します");
        if (ui::BeginPropertyTable("legacyRoadInputs")) {
            ui::PropertyBool("旧入力を表示", &m_showLegacyRoadInputs, false,
                "Roadの取込元のピンと接続線を表示する。区間編集中の路面には反映されず、区間編集を解除すると使用する");
            ui::EndPropertyTable();
        }
        if (m_showLegacyRoadInputs) ui::HintText("旧入力は区間編集を解除したときに使用します");
        graph::RoadGeometry road;
        std::string error;
        if (!graph::EvaluateRoad(m_graph, roadId, road, error)) { ui::HintText("先に道路の形状を修正してください"); return false; }
        const float length = road.rowDistances.back();
        if (band->spans.empty() || std::abs(band->spans.back().endMeters - length) > 0.001f) {
            ui::HintText("区間の範囲と道路長が異なります");
            if (ui::Button("道路長に合わせる", ui::kWideButtonWidth)) changed |= graph::ResizeSurfaceBand(*band, length);
        }
        if (!band->spans.empty()) {
            auto found = std::find_if(band->spans.begin(), band->spans.end(), [&](const auto& span) { return span.id == m_surfaceLayoutSpan; });
            int selected = found == band->spans.end() ? 0 : static_cast<int>(found - band->spans.begin());
            std::vector<std::string> labels;
            for (const auto& span : band->spans) {
                const auto labelPreset = std::find_if(m_surfaceLayouts.layerMaterials.begin(), m_surfaceLayouts.layerMaterials.end(), [&](const auto& p) { return p.id == graph::PresetLayerMaterial(m_surfaceLayouts, span.preset); });
                    char label[192]; std::snprintf(label, sizeof(label), "%.2f～%.2f m / %s", span.startMeters, span.endMeters,
                        labelPreset == m_surfaceLayouts.layerMaterials.end() ? "未設定" : labelPreset->name.c_str());
                labels.emplace_back(label);
            }
            std::vector<const char*> items;
            for (const auto& label : labels) items.push_back(label.c_str());
            if (ui::BeginPropertyTable("surfaceSpanRows")) {
                ui::PropertyCombo("区間", &selected, items.data(), static_cast<int>(items.size()), 0, "編集する区間を選ぶ。始点からの実距離で表示する");
                m_surfaceLayoutSpan = band->spans[selected].id;
                auto& span = band->spans[selected];
                std::vector<graph::SurfaceId> presetIds;
                std::vector<const char*> presetNames;
                std::vector<ImTextureID> presetThumbnails;
                int presetIndex = 0;
                for (const auto& preset : edited.layerMaterials) {
                    if (preset.id == graph::PresetLayerMaterial(edited, span.preset)) presetIndex = static_cast<int>(presetIds.size());
                    presetIds.push_back(preset.id); presetNames.push_back(preset.name.c_str());
                    const auto thumbnail = std::find_if(m_layerThumbnails.begin(), m_layerThumbnails.end(),
                        [&](const auto& entry) { return entry.id == preset.id; });
                    presetThumbnails.push_back(thumbnail != m_layerThumbnails.end() && thumbnail->ready
                        ? static_cast<ImTextureID>(thumbnail->texture.srv.gpu.ptr) : 0);
                }
                if (ui::PropertyCombo("材質", &presetIndex, presetNames.data(), static_cast<int>(presetNames.size()), presetIndex,
                                      "この区間へ割り当てる材質。1本の道路で同時に3種類まで使用できる", presetThumbnails.data())) {
                    changed |= graph::AssignLayerMaterial(edited, span, presetIds[presetIndex]);
                }
                ui::PropertyValue("始点", "%.2f m", span.startMeters);
                if (selected + 1 < static_cast<int>(band->spans.size()) && band->spans[selected + 1].endMeters - span.startMeters > 0.02f) {
                    auto& next = band->spans[selected + 1];
                    if (ui::PropertyFloat("終点", &span.endMeters, span.startMeters + 0.01f, next.endMeters - 0.01f,
                                          (span.startMeters + next.endMeters) * 0.5f, "次の区間との境界。隙間を作らず一緒に動かす", "%.2f m")) {
                        next.startMeters = span.endMeters; graph::ClampSpanBlends(span); graph::ClampSpanBlends(next); changed = true;
                    }
                } else ui::PropertyValue("終点", "%.2f m", span.endMeters);
                const graph::SurfaceSpan defaults;
                if (selected > 0) changed |= ui::PropertyFloat("始端の移行", &span.blendInMeters, 0, span.endMeters - span.startMeters,
                    defaults.blendInMeters, "前の材質から移る距離。実際の移行は区間長の半分まで", "%.2f m");
                if (selected + 1 < static_cast<int>(band->spans.size())) changed |= ui::PropertyFloat("終端の移行", &span.blendOutMeters, 0,
                    span.endMeters - span.startMeters, defaults.blendOutMeters, "次の材質へ移る距離。大きいほど緩やかに変わる", "%.2f m");
                ui::EndPropertyTable();
            }
            if (ui::Button("分割")) changed |= graph::SplitSurfaceSpan(edited, *band, selected);
            ImGui::SameLine();
            ImGui::BeginDisabled(band->spans.size() < 2);
            if (ui::Button("削除")) { changed |= graph::RemoveSurfaceSpan(*band, selected); m_surfaceLayoutSpan = 0; }
            ImGui::EndDisabled();
            // 構造を変更したフレームは、選択中の参照を次フレームに取り直す。
            if (!changed) {
                auto& span = band->spans[selected];
                auto preset = std::find_if(edited.presets.begin(), edited.presets.end(), [&](const auto& p) { return p.id == span.preset; });
                if (preset != edited.presets.end()) {
                    if (ui::Button("材質を編集##road", ui::kWideButtonWidth)) {
                        m_editSurfacePreset = preset->layerMaterial; m_surfacePresetError.clear();
                    }
                    ImGui::SameLine();
                    if (ui::Button("複製して編集##road", ui::kWideButtonWidth)) {
                        changed |= graph::DuplicateLayerMaterial(edited, band->spans[selected]);
                        if (changed) { m_editSurfacePreset = graph::PresetLayerMaterial(edited, band->spans[selected].preset); m_surfacePresetError.clear(); }
                    }
                }
            }
        }
        if (ui::Button("区間編集を解除", ui::kWideButtonWidth)) {
            band->spans.clear(); // 沿道の記述とプリセットは保持する。Roadの元の材質へ戻す。
            changed = true;
        }
    }
    if (changed) {
        std::string error;
        if (graph::ValidateSurfaceLayouts(edited, error)) {
            if (const auto* active = graph::FindRoadBand(edited, roadId); active && !active->spans.empty())
                error = graph::CompileSurfaceLayoutPreview(m_graph, edited, roadId).error;
            if (error.empty()) { m_surfaceLayouts = std::move(edited); return true; }
        }
        m_editSurfacePreset = 0; m_surfacePresetError = error;
        TG_LOG_ERROR("路面区間: %s", error.c_str());
    }
    return false;
}
}  // namespace tg
