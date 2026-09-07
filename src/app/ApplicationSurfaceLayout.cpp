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
    ui::SectionHeader("沿道形状（試作）");
    if (ui::BeginPropertyTable("surfaceBandPreviewRows")) {
        if (ui::PropertyBool("形状を表示", &m_previewSurfaceBands, false,
            "沿道の形状と下地材質を確認する。ハイト変位は横接続の「変位もつなぐ」で確認する")) m_graph.MarkDirty();
        const char* sides[] = {"左", "右"};
        if (ui::PropertyCombo("配置する側", &m_surfaceBandSide, sides, 2, 0, "道路の進行方向に向かっての左右。横接続を試す側も切り替える")) m_graph.MarkDirty();
        if (ui::PropertyBool("横接続を試す", &m_connectSurfaceBands, false,
            "道路1種類と片側の沿道の最大2種類を境界で混ぜる。両側にある場合は選択側、片側だけならその側を接続する。形状表示もオンにする。凹凸は「変位もつなぐ」で有効にする")) {
            if (m_connectSurfaceBands) m_previewSurfaceBands = true;
            m_graph.MarkDirty();
        }
        if (ui::PropertyBool("変位もつなぐ", &m_displaceConnectedBands, false,
            "境界付近の変位を共通の基準高さへ戻し、離れた所では素材の凹凸を残す。変位方向は世界Y")) {
            if (m_displaceConnectedBands) { m_connectSurfaceBands = true; m_previewSurfaceBands = true; }
            m_graph.MarkDirty();
        }
        ui::EndPropertyTable();
    }
    const auto side = m_surfaceBandSide == 0 ? graph::SurfaceSide::Left : graph::SurfaceSide::Right;
    if (m_previewSurfaceBands && m_connectSurfaceBands) ui::HintText(m_displaceConnectedBands
        ? "境界付近の凹凸を滑らかに抑え、段差の基準高さへ接続します"
        : "片側の横接続を試作中。接続した道路と沿道の変位は停止します");
    bool exists = false;
    for (const auto& layout : m_surfaceLayouts.layouts) if (layout.roadNode == roadId)
        for (const auto& candidate : layout.bands) if (candidate.side == side) exists = true;
    if (!exists) {
        if (ui::Button("路肩→歩道を作る", ui::kWideButtonWidth)) {
            std::string error;
            if (graph::CreateRoadsideExample(m_surfaceLayouts, m_graph, roadId, side, error)) {
                m_previewSurfaceBands = true;
                return true;
            }
            TG_LOG_ERROR("沿道形状: %s", error.c_str());
        }
    } else {
        auto roadsideEdit = m_surfaceLayouts;
        for (auto& layout : roadsideEdit.layouts) if (layout.roadNode == roadId) {
            for (auto& candidate : layout.bands) if (candidate.side == side && candidate.spans.size() == 2) {
                auto& first = candidate.spans[0]; auto& second = candidate.spans[1];
                const float length = second.endMeters;
                bool changed = false;
                if (length > 0.1f && ui::BeginPropertyTable("roadsideSpanRows")) {
                    if (ui::PropertyFloat("切替位置", &first.endMeters, 0.05f, length - 0.05f, length * 0.5f,
                        "道路の始点から、路肩と歩道が切り替わる位置", "%.2f m")) {
                        second.startMeters = first.endMeters;
                        graph::ClampSpanBlends(first); graph::ClampSpanBlends(second); changed = true;
                    }
                    float transition = std::min(first.blendOutMeters, second.blendInMeters);
                    const float limit = std::min(first.endMeters - first.startMeters, second.endMeters - second.startMeters) * 0.5f;
                    if (limit >= 0.01f && ui::PropertyFloat("移行距離", &transition, 0.01f, limit, std::min(2.0f, limit),
                        "切替位置の前後それぞれで断面を変える距離", "%.2f m")) {
                        first.blendOutMeters = second.blendInMeters = transition; changed = true;
                    }
                    ui::EndPropertyTable();
                }
                if (changed) {
                    graph::RoadGeometry road; renderer::MeshData mesh; std::string error;
                    if (graph::EvaluateRoad(m_graph, roadId, road, error) &&
                        graph::BuildSurfaceBandGeometry(road, roadsideEdit, candidate, mesh, error)) {
                        m_surfaceLayouts = std::move(roadsideEdit); return true;
                    }
                    TG_LOG_ERROR("沿道形状: %s", error.c_str());
                }
            }
        }
        for (auto& layout : roadsideEdit.layouts) if (layout.roadNode == roadId) {
            for (auto& candidate : layout.bands) if (candidate.side == side && !candidate.spans.empty()) {
                m_surfaceBandSpan = std::clamp(m_surfaceBandSpan, 0, static_cast<int>(candidate.spans.size()) - 1);
                std::vector<std::string> labels;
                for (const auto& span : candidate.spans) {
                    char label[96]; std::snprintf(label, sizeof(label), "%.2f ～ %.2f m", span.startMeters, span.endMeters);
                    labels.emplace_back(label);
                }
                std::vector<const char*> items;
                for (const auto& label : labels) items.push_back(label.c_str());
                bool changed = false;
                ui::SectionHeader("沿道の下地材質");
                if (ui::BeginPropertyTable("roadsideMaterialRows")) {
                    ui::PropertyCombo("編集する区間", &m_surfaceBandSpan, items.data(), static_cast<int>(items.size()), 0, "材質を編集する沿道の区間");
                    auto& span = candidate.spans[m_surfaceBandSpan];
                    auto preset = std::find_if(roadsideEdit.presets.begin(), roadsideEdit.presets.end(), [&](const auto& p) { return p.id == span.preset; });
                    if (preset != roadsideEdit.presets.end()) {
                        ui::PropertyValue("プリセット", "%s", preset->name.c_str());
                        auto& material = preset->materials.front();
                        const graph::PresetMaterial defaults;
                        const graph::SurfacePreset presetDefaults;
                        changed |= ui::PropertyFloat("凹凸の高さ", &preset->displacementMeters, 0, 1, presetDefaults.displacementMeters,
                            "横接続で変位もつなぐときの押し出し量。境界では自動で抑える", "%.3f m");
                        changed |= DrawMaterialSlotRow("素材", material.material, m_materialLibrary);
                        changed |= ui::PropertyFloat("反復長", &material.uvRepeatMeters, 0.01f, 100, defaults.uvRepeatMeters,
                            "素材が繰り返す実距離。断面の垂直面も距離に含む", "%.2f m");
                        const char* spaces[] = {"面に沿う", "ワールド XZ"};
                        int space = material.worldUv ? 1 : 0;
                        if (ui::PropertyCombo("座標", &space, spaces, 2, defaults.worldUv ? 1 : 0, "ワールド XZは垂直面では模様が伸びるため、歩道の段差には面に沿うを推奨")) {
                            material.worldUv = space == 1; changed = true;
                        }
                        if (!material.material) {
                            changed |= ui::PropertyColorLinear("色", material.baseColor.data(), defaults.baseColor.data(), "素材未指定時の色");
                            changed |= ui::PropertyFloat("粗さ", &material.roughness, 0, 1, defaults.roughness, "素材未指定時の反射のぼけ");
                        }
                    }
                    ui::EndPropertyTable();
                }
                ui::HintText("同じプリセットを使う沿道区間すべてに反映します");
                if (changed) {
                    const auto preview = graph::CompileSurfaceBandPreview(m_graph, roadsideEdit, roadId, candidate.id);
                    if (preview.error.empty()) { m_surfaceLayouts = std::move(roadsideEdit); return true; }
                    TG_LOG_ERROR("沿道材質: %s", preview.error.c_str());
                }
                break;
            }
        }
        ui::HintText("下地1層の試作。凹凸は横接続の「変位もつなぐ」で確認");
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
                char label[96]; std::snprintf(label, sizeof(label), "%.2f ～ %.2f m", span.startMeters, span.endMeters);
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
                int presetIndex = 0;
                for (const auto& preset : edited.presets) if (preset.role == graph::SurfaceRole::Road) {
                    if (preset.id == span.preset) presetIndex = static_cast<int>(presetIds.size());
                    presetIds.push_back(preset.id); presetNames.push_back(preset.name.c_str());
                }
                if (ui::PropertyCombo("プリセット", &presetIndex, presetNames.data(), static_cast<int>(presetNames.size()), presetIndex,
                                      "この区間へ割り当てる材質。1本の道路で同時に3種類まで使用できる")) {
                    span.preset = presetIds[presetIndex]; span.parameters.clear(); changed = true;
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
                    ui::SectionHeader("プリセットの材質");
                    ui::HintText("同じプリセットを使う全区間へ反映。個別に変える場合は複製");
                    if (ui::Button("複製して編集", ui::kWideButtonWidth)) changed |= graph::DuplicateSurfacePreset(edited, *band, selected);
                    if (!changed && ui::BeginPropertyTable("surfacePresetRows")) {
                        char name[128]; std::snprintf(name, sizeof(name), "%s", preset->name.c_str());
                        if (ui::PropertyTextInput("名前", name, sizeof(name), "プリセット一覧に表示する名前") && name[0]) { preset->name = name; changed = true; }
                        const graph::SurfacePreset defaults;
                        changed |= ui::PropertyFloat("凹凸の高さ", &preset->displacementMeters, 0, 10, defaults.displacementMeters,
                            "素材のハイトで押し出す量。0なら形状を変えない", "%.3f m");
                        const char* layers[] = {"下地", "層 2", "層 3", "層 4"};
                        m_surfaceLayoutLayer = std::clamp(m_surfaceLayoutLayer, 0, static_cast<int>(preset->materials.size()) - 1);
                        ui::PropertyCombo("編集する層", &m_surfaceLayoutLayer, layers, static_cast<int>(preset->materials.size()), 0, "プリセット内部で編集する材質層");
                        auto& material = preset->materials[m_surfaceLayoutLayer];
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
                        changed |= ui::PropertyFloat("ブレンド幅", &preset->layerBlendRange, 0, 1, defaults.layerBlendRange,
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
                        ui::EndPropertyTable();
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
        TG_LOG_ERROR("路面区間: %s", error.c_str());
    }
    return false;
}
}  // namespace tg
