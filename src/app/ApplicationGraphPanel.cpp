#include "graph/Road.h"
// ノードグラフパネル。imgui-node-editor によるエディタと、
// 選択中ノードのプロパティ（レイヤーパネルと共有）を持つ。
//
// エディタの作法（カード描画・丸ピン・ドット背景・リンクの作成 / 削除）は
// terrain-editor のノードエディタ UI から移植した。ノードそのものは
// このプロジェクト独自（サーフェス / シェイプ / 水面 / 出力）。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "ui/UiStyle.h"

#include <imgui.h>
#include <imgui-node-editor/imgui_node_editor.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <variant>
#include <vector>

namespace ed = ax::NodeEditor;

namespace tg {
namespace {

ImU32 ColorToU32(const ImVec4& color) {
    return ImGui::ColorConvertFloat4ToU32(color);
}

// 種類ごとのアクセント色。グレー基調を崩さないよう彩度は低め。
ImVec4 NodeAccentColor(graph::NodeKind kind) {
    switch (kind) {
        case graph::NodeKind::Surface:
            return ImVec4(0.55f, 0.66f, 0.58f, 1.0f);
        case graph::NodeKind::Heightmap:
            return ImVec4(0.72f, 0.66f, 0.50f, 1.0f);
        case graph::NodeKind::Shape:
            return ImVec4(0.66f, 0.62f, 0.52f, 1.0f);
        case graph::NodeKind::Liquid:
            return ImVec4(0.50f, 0.62f, 0.70f, 1.0f);
        case graph::NodeKind::Blur:
            return ImVec4(0.62f, 0.58f, 0.68f, 1.0f);
        case graph::NodeKind::Sediment:
            return ImVec4(0.70f, 0.62f, 0.52f, 1.0f);
        case graph::NodeKind::Crumbling:
            return ImVec4(0.74f, 0.58f, 0.50f, 1.0f);
        case graph::NodeKind::Snow:
            return ImVec4(0.72f, 0.76f, 0.82f, 1.0f);
        case graph::NodeKind::River:
            return ImVec4(0.48f, 0.64f, 0.72f, 1.0f);
        case graph::NodeKind::Droplet:
            return ImVec4(0.56f, 0.66f, 0.62f, 1.0f);
        case graph::NodeKind::Scatter:
            return ImVec4(0.60f, 0.70f, 0.52f, 1.0f);
        case graph::NodeKind::MaskImage:
            return ImVec4(0.72f, 0.72f, 0.72f, 1.0f);
        case graph::NodeKind::MaskNoise:
            return ImVec4(0.68f, 0.72f, 0.62f, 1.0f);
        case graph::NodeKind::MaskFluvial:
            return ImVec4(0.55f, 0.68f, 0.74f, 1.0f);
        case graph::NodeKind::MaskHeight:
            return ImVec4(0.74f, 0.70f, 0.60f, 1.0f);
        case graph::NodeKind::MaskSlope:
            return ImVec4(0.60f, 0.70f, 0.66f, 1.0f);
        case graph::NodeKind::MaskCurvature:
            return ImVec4(0.66f, 0.68f, 0.74f, 1.0f);
        case graph::NodeKind::MaskLevels:
            return ImVec4(0.78f, 0.76f, 0.70f, 1.0f);
        case graph::NodeKind::MaskBlur:
            return ImVec4(0.76f, 0.72f, 0.64f, 1.0f);
        case graph::NodeKind::MaskBlend:
            return ImVec4(0.74f, 0.70f, 0.78f, 1.0f);
        case graph::NodeKind::Path:
            return ImVec4(0.52f, 0.74f, 0.84f, 1.0f);
        case graph::NodeKind::RoadMarking:
            return ImVec4(0.80f, 0.80f, 0.76f, 1.0f);
        case graph::NodeKind::RoadMask:
            return ImVec4(0.78f, 0.66f, 0.50f, 1.0f);
        case graph::NodeKind::Decal:
            return ImVec4(0.72f, 0.60f, 0.76f, 1.0f);
        case graph::NodeKind::Shoulder:
            return ImVec4(0.70f, 0.64f, 0.52f, 1.0f);
        case graph::NodeKind::Merge:
            return ImVec4(0.62f, 0.70f, 0.66f, 1.0f);
        case graph::NodeKind::Crack:
            return ImVec4(0.66f, 0.58f, 0.62f, 1.0f);
        case graph::NodeKind::MaskPath:
        case graph::NodeKind::MaskArea:
            return ImVec4(0.58f, 0.74f, 0.82f, 1.0f);
        case graph::NodeKind::Output:
        default:
            return ImVec4(0.59f, 0.64f, 0.68f, 1.0f);
    }
}

// ピンとリンクの色。**線が何を運んでいるかを色で見分ける。**
// 値は terrain-editor に合わせてある（あちらの HeightField がこちらの Material）。
// 緑とオレンジは明度が近く、色相だけが離れているので、
// 暗い盤面でどちらも同じ強さで読める。
ImVec4 PinTypeColor(graph::ValueType valueType) {
    switch (valueType) {
        // マスクはオレンジ。0〜1 の 1 チャンネル。
        case graph::ValueType::Mask:
            return ImVec4(0.82f, 0.64f, 0.36f, 1.0f);
        // パスは水色。線（点とエッジ）が流れる。緑 / オレンジと色相が離れていて、
        // 明度は同じくらいなので暗い盤面で同じ強さで読める。
        case graph::ValueType::Path:
            return ImVec4(0.55f, 0.80f, 0.95f, 1.0f);
        // マテリアルは緑。4 チャンネル一式（ハイトを含む）。
        case graph::ValueType::Mesh:
            return ImGui::GetStyleColorVec4(ImGuiCol_PlotLines);
        // 道路空間マスクは茶色寄りのオレンジ。タイル空間のマスク（オレンジ）と区別する。
        case graph::ValueType::RoadMask:
            return ImVec4(0.80f, 0.56f, 0.34f, 1.0f);
        case graph::ValueType::Material:
        default:
            return ImVec4(0.70f, 0.93f, 0.78f, 1.0f);
    }
}

// ピンの矩形。当たり判定をラベルまで広げるので、丸の位置は別に持つ。
struct PinGeometry {
    ImVec2 min;     // 丸の矩形
    ImVec2 max;
    ImVec2 center;  // 接続点（リンクの端）
};

// 丸ピンを描いて矩形を返す。**当たり判定（ed::PinRect）は呼び出し側で決める。**
// ラベルまで含めて掴めるようにするため（出力ピンはクリックでプレビューも切り替える）。
// filled が真なら丸を塗る。**ビューポートに出ている出力**の印に使う。
PinGeometry DrawRoundPin(const graph::Pin& pin, bool filled = false) {
    const ImVec2 size(14.0f, 20.0f);
    ImGui::Dummy(size);
    PinGeometry geometry;
    geometry.min = ImGui::GetItemRectMin();
    geometry.max = ImGui::GetItemRectMax();
    geometry.center = ImVec2((geometry.min.x + geometry.max.x) * 0.5f,
                             (geometry.min.y + geometry.max.y) * 0.5f);
    ed::PinPivotRect(ImVec2(geometry.center.x - 6.0f, geometry.center.y - 6.0f),
                     ImVec2(geometry.center.x + 6.0f, geometry.center.y + 6.0f));
    const ImU32 pinColor = ColorToU32(PinTypeColor(pin.valueType));
    if (filled) {
        ImGui::GetWindowDrawList()->AddCircleFilled(geometry.center, 4.3f, pinColor, 16);
    } else {
        ImGui::GetWindowDrawList()->AddCircle(geometry.center, 4.3f, pinColor, 16, 1.6f);
    }
    return geometry;
}

// ドットグリッドの背景。既定のグリッド線は消して自前で描く。
void DrawGraphDots(const ImVec2& screenMin, const ImVec2& screenMax) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 canvasMin = ed::ScreenToCanvas(screenMin);
    const ImVec2 canvasMax = ed::ScreenToCanvas(screenMax);
    constexpr float kBaseSpacing = 24.0f;
    const ImU32 backgroundColor = ColorToU32(ImVec4(0.112f, 0.112f, 0.112f, 1.0f));
    const ImU32 dotColor = ColorToU32(ImVec4(0.26f, 0.26f, 0.26f, 0.46f));

    ed::Suspend();
    drawList->PushClipRect(screenMin, screenMax, true);
    drawList->AddRectFilled(screenMin, screenMax, backgroundColor);
    const ImVec2 screen0 = ed::CanvasToScreen(ImVec2(0.0f, 0.0f));
    const ImVec2 screenStep = ed::CanvasToScreen(ImVec2(kBaseSpacing, 0.0f));
    const float baseScreenSpacing = std::max(1.0f, std::abs(screenStep.x - screen0.x));
    // 引きで見たときにドットが密集しないよう、画面上の間隔が保たれる倍率へ広げる。
    const float spacing = kBaseSpacing * std::max(1.0f, std::ceil(12.0f / baseScreenSpacing));
    const float startX = std::floor(std::min(canvasMin.x, canvasMax.x) / spacing) * spacing;
    const float endX = std::ceil(std::max(canvasMin.x, canvasMax.x) / spacing) * spacing;
    const float startY = std::floor(std::min(canvasMin.y, canvasMax.y) / spacing) * spacing;
    const float endY = std::ceil(std::max(canvasMin.y, canvasMax.y) / spacing) * spacing;
    for (float y = startY; y <= endY; y += spacing) {
        for (float x = startX; x <= endX; x += spacing) {
            const ImVec2 screen = ed::CanvasToScreen(ImVec2(x, y));
            if (screen.x < screenMin.x || screen.x > screenMax.x || screen.y < screenMin.y ||
                screen.y > screenMax.y) {
                continue;
            }
            drawList->AddRectFilled(ImVec2(screen.x - 1.0f, screen.y - 1.0f),
                                    ImVec2(screen.x + 1.0f, screen.y + 1.0f), dotColor);
        }
    }
    drawList->PopClipRect();
    ed::Resume();
}

// ノードの表示名。レイヤー設定を持つ種類はレイヤー名を出す。
const char* NodeDisplayName(const graph::Node& node) {
    if (const auto* settings = std::get_if<graph::LayerNodeSettings>(&node.settings)) {
        if (!settings->layer.name.empty()) {
            return settings->layer.name.c_str();
        }
    }
    const graph::NodeDefinition* definition = graph::FindNodeDefinition(node.kind);
    return (definition != nullptr) ? definition->title : "?";
}

int ToGraphId(uintptr_t id) {
    return static_cast<int>(id);
}

}  // namespace

// 材質スロットの行（Road と Shoulder で共通）。1 は下地、2〜4 は Mask 2〜4 で被覆する。
// 座標と反復長はスロットごと。変更があれば真。
bool Application::DrawMaterialSlotRows(const graph::Node& node, bool* layerWorldUv, float* layerUvRepeatMeters,
                                       float& layerBlendRange, float defaultBlendRange) {
    bool changed = false;
    ui::SectionHeader("材質スロット");
    if (!ui::BeginPropertyTable("layerRows")) return false;
    static const char* const kUvSpaceLabels[] = {"面に沿う", "ワールド XZ"};
    std::vector<const graph::Pin*> materialPins;
    std::vector<const graph::Pin*> maskPins;
    for (const auto& pin : node.inputs) {
        if (pin.valueType == graph::ValueType::Material) materialPins.push_back(&pin);
        if (pin.valueType == graph::ValueType::RoadMask) maskPins.push_back(&pin);
    }
    for (int slot = 0; slot < graph::kRoadMaterialSlots; ++slot) {
        char label[32];
        std::snprintf(label, sizeof(label), "スロット %d", slot + 1);
        const bool hasMaterial = slot < static_cast<int>(materialPins.size()) &&
                                 m_graph.FindUpstreamNodeForPin(materialPins[slot]->id) != nullptr;
        const bool hasMask = slot == 0 || (slot - 1 < static_cast<int>(maskPins.size()) &&
                                          m_graph.FindUpstreamNodeForPin(maskPins[slot - 1]->id) != nullptr);
        ui::PropertyValue(label, "%s", !hasMaterial ? "材質なし" : (hasMask ? "有効" : "マスクなし（無効）"));
        if (!hasMaterial) continue;
        char spaceId[32];
        std::snprintf(spaceId, sizeof(spaceId), "  座標##slot%d", slot);
        int space = layerWorldUv[slot] ? 1 : 0;
        if (ui::PropertyCombo(spaceId, &space, kUvSpaceLabels, IM_ARRAYSIZE(kUvSpaceLabels), 0,
                              "面に沿う: 道路 UV。ワールド XZ: 位置の XZ 平面。路肩や地面と地続きにする層は XZ")) {
            layerWorldUv[slot] = (space == 1);
            changed = true;
        }
        if (slot > 0) {
            char repeatId[32];
            std::snprintf(repeatId, sizeof(repeatId), "  UV反復長##slot%d", slot);
            changed |= ui::PropertyFloat(repeatId, &layerUvRepeatMeters[slot], 0.1f, 100.0f, 1.0f,
                                         "このスロットの材質で UV が 1 増える実距離", "%.2f m");
        }
    }
    changed |= ui::PropertyFloat("ブレンド幅", &layerBlendRange, 0.0f, 1.0f, defaultBlendRange,
                                 "スロット同士をハイトで競合させるときの境界の柔らかさ。小さいほど凹凸なりにぎざぎざ", "%.2f");
    ui::EndPropertyTable();
    return changed;
}

namespace {

// エディタへ渡してよい座標か。エディタは**知らないノードの位置を FLT_MAX で返す**ので、
// それを信じて書き戻す・流し込むとノードが無限遠へ飛び、キャンバスの座標計算が
// 壊れて操作できなくなる。読み込んだファイルの値の検証にも使う。
bool IsValidNodePosition(float x, float y) {
    constexpr float kMaxCoordinate = 1.0e6f;
    return std::isfinite(x) && std::isfinite(y) && std::abs(x) <= kMaxCoordinate &&
           std::abs(y) <= kMaxCoordinate;
}

}  // namespace

void Application::DestroyGraphEditor() {
    if (m_nodeEditor != nullptr) {
        ed::DestroyEditor(m_nodeEditor);
        m_nodeEditor = nullptr;
    }
}

void Application::RequestGraphNodePlacement(bool navigate) {
    m_graphNodesToPlace.clear();
    for (const graph::Node& node : m_graph.Nodes()) {
        m_graphNodesToPlace.push_back(node.id);
    }
    // 全体の流し込みの後だけ画面へ収め直す。1 個の追加やアンドゥでは
    // 視点を動かさない（そのたびに視点が飛ぶと編集にならない）。
    if (navigate) {
        m_graphNavigateCountdown = 3;
    }
}

// ビューポートに出すノードを決める。**選択とは別**に持つので、
// 結果を見ながら別のノードのプロパティをいじれる。
void Application::SetPreviewGraphNode(graph::GraphId nodeId, graph::GraphId outputPin) {
    const graph::Node* node = m_graph.FindNode(nodeId);
    // 出力ノードと、プレビューできない種類は「出力ノードのチェーン」に落とす。
    const bool previewable = (node != nullptr && graph::IsPreviewableNodeKind(node->kind));
    m_previewGraphNode = previewable ? node->id : 0;
    // 見る出力。そのノードの出力ピンでなければ 0（＝最初の出力）に落とす。
    m_previewGraphPin = 0;
    if (previewable) {
        for (const graph::Pin& pin : node->outputs) {
            if (pin.id == outputPin) {
                m_previewGraphPin = pin.id;
                break;
            }
        }
    }
}

void Application::SyncMeshGraph() {
    // 途中のメッシュノード（Road / Lane Marking / Decal）を見ているときは、そのノードまでの鎖を出す。
    graph::GraphId previewMeshNode = 0;
    if (const graph::Node* node = m_graph.FindNode(m_previewGraphNode);
        node != nullptr && graph::IsMeshNodeKind(node->kind)) {
        previewMeshNode = node->id;
    }
    if (m_meshGraphRevision == m_graph.Revision() && m_meshGraphPreviewNode == previewMeshNode) return;
    auto compiled = graph::CompileMeshGraph(m_graph, previewMeshNode);
    const bool uploaded = compiled.active
        ? m_renderer.SetGeneratedMeshScene(m_device, compiled.scene)
        : (!m_meshGraphActive || m_renderer.RestoreAuthoredMeshScene(m_device));
    m_meshGraphError = compiled.error;
    if (!uploaded) m_meshGraphError = "道路メッシュをGPUへ転送できませんでした";
    if (uploaded) {
        m_meshGraphActive = compiled.active;
        m_meshSelection = MeshSelectionState{};
    }
    m_meshGraphRevision = m_graph.Revision();
    m_meshGraphPreviewNode = previewMeshNode;
}

void Application::SyncGraphStack() {
    // プレビューの対象。**出力ピンのクリックで決める**（選択とは別）。
    // 0 のときは出力ノードのチェーン。
    // メッシュノードのプレビューは SyncMeshGraph が受け持つので、2D の合成は出力ノードのままにする。
    graph::GraphId target = 0;
    if (const graph::Node* node = m_graph.FindNode(m_previewGraphNode);
        node != nullptr && graph::IsPreviewableNodeKind(node->kind)) {
        if (!graph::IsMeshNodeKind(node->kind)) target = node->id;
    } else {
        m_previewGraphNode = 0;
        m_previewGraphPin = 0;
    }
    // 地形の実寸はチェーンの根にある Heightmap ノードが持つ。
    // **読み込むときに一度決めたら、以後はプレビュー側で触らない。**
    if (const graph::TerrainScale* scale = m_graph.FindChainScale(target)) {
        m_renderer.PlaneSize() = scale->sizeMeters;
        m_renderer.DisplacementScale() = scale->heightMeters;
    }
    // 合成の法線は実寸の勾配から作るので、評価器にも同じ実寸を渡す。
    // ノードが実寸を持たないときはプレビュー設定がジオメトリを決めるので、
    // そちらに合わせる（押し出した形と陰影の起伏を一致させる）。
    // レイヤー列が変わらなくても実寸だけ動くことがあるため、早期 return より前に置く。
    m_graphStack.SetTerrainScale(m_renderer.PlaneSize(), m_renderer.DisplacementScale());

    // 描画側は「いまマスクを見ているか」を知らないと斜線を引けない。毎フレーム写す。
    m_renderer.MaskPreviewActive() = false;
    if (const graph::Node* node = m_graph.FindNode(target); node != nullptr) {
        for (const graph::Pin& pin : node->outputs) {
            const bool isPreviewed =
                (pin.id == m_previewGraphPin) ||
                (m_previewGraphPin == 0 && pin.id == node->outputs.front().id);
            if (isPreviewed && pin.valueType == graph::ValueType::Mask) {
                m_renderer.MaskPreviewActive() = true;
            }
        }
    }

    if (m_compiledGraphRevision == m_graph.Revision() && m_compiledGraphTarget == target &&
        m_compiledGraphTargetPin == m_previewGraphPin) {
        return;
    }
    m_compiledGraphRevision = m_graph.Revision();
    m_compiledGraphTarget = target;
    m_compiledGraphTargetPin = m_previewGraphPin;
    graph::CompiledGraph compiled = (target != 0)
                                        ? m_graph.CompileLayersTo(target, m_previewGraphPin)
                                        : m_graph.CompileLayers();
    m_graphStack.Layers() = std::move(compiled.layers);
    m_graphStack.MaskOps() = std::move(compiled.maskOps);
    m_graphStack.MarkDirty();

    // op の出どころを版ごとに控える。評価が追いつくまでの数版ぶんあれば足りる。
    constexpr size_t kKeepRevisions = 4;
    GraphMaskOpSources sources;
    sources.revision = m_graphStack.Revision();
    sources.ops = std::move(compiled.maskOpSources);
    sources.layers = std::move(compiled.layerSources);
    m_graphMaskOpSources.push_back(std::move(sources));
    while (m_graphMaskOpSources.size() > kKeepRevisions) {
        m_graphMaskOpSources.erase(m_graphMaskOpSources.begin());
    }
}

D3D12_GPU_DESCRIPTOR_HANDLE Application::GraphLayerThumbnail(graph::GraphId nodeId) const {
    const compositor::MaterialEvaluator& evaluator = m_renderer.Evaluator();
    const uint64_t revision = evaluator.EvaluatedRevision();
    for (const GraphMaskOpSources& sources : m_graphMaskOpSources) {
        if (sources.revision != revision) {
            continue;
        }
        // 同じノードが Mask だけの差し込み（maskOnly）で先に並ぶことがあるので、
        // 後ろから探して Result のほう（本流）を取る。
        for (size_t i = sources.layers.size(); i-- > 0;) {
            if (sources.layers[i] == nodeId) {
                return evaluator.LayerThumbnailHandle(i);
            }
        }
        break;
    }
    return D3D12_GPU_DESCRIPTOR_HANDLE{0};
}

D3D12_GPU_DESCRIPTOR_HANDLE Application::GraphMaskThumbnail(graph::GraphId nodeId,
                                                            size_t outputIndex) const {
    const compositor::MaterialEvaluator& evaluator = m_renderer.Evaluator();
    const uint64_t revision = evaluator.EvaluatedRevision();
    for (const GraphMaskOpSources& sources : m_graphMaskOpSources) {
        if (sources.revision != revision) {
            continue;
        }
        for (size_t i = 0; i < sources.ops.size(); ++i) {
            if (sources.ops[i].nodeId == nodeId && sources.ops[i].outputIndex == outputIndex) {
                return evaluator.MaskOpThumbnailHandle(i);
            }
        }
        break;
    }
    return D3D12_GPU_DESCRIPTOR_HANDLE{0};
}

// 選択中のノードを控える。**出力ノードは対象外**（1 つだけ繋ぐ前提のノードで、
// 増やしても迷うだけなので）。
void Application::CopySelectedGraphNodes() {
    std::vector<const graph::Node*> nodes;
    for (const graph::GraphId id : m_selectedGraphNodes) {
        const graph::Node* node = m_graph.FindNode(id);
        if (node != nullptr && node->kind != graph::NodeKind::Output) {
            nodes.push_back(node);
        }
    }
    if (nodes.empty()) {
        return;
    }

    m_graphClipboard.clear();
    m_graphPasteCount = 0;
    for (const graph::Node* node : nodes) {
        GraphClipboardNode entry;
        entry.kind = node->kind;
        entry.settings = node->settings;
        entry.posX = node->posX;
        entry.posY = node->posY;
        const ImVec2 size = ed::GetNodeSize(ed::NodeId(node->id));
        entry.sizeX = size.x;
        entry.sizeY = size.y;
        for (const graph::Pin& pin : node->inputs) {
            GraphClipboardNode::Source source;
            // この入力へ繋がっているリンクの「出力ピン」を覚える。
            for (const graph::Link& link : m_graph.Links()) {
                if (link.endPin != pin.id) {
                    continue;
                }
                const graph::Pin* startPin = m_graph.FindPin(link.startPin);
                if (startPin == nullptr) {
                    break;
                }
                // コピーした集合の中を指しているなら、貼った側どうしで繋ぎ直す。
                for (size_t i = 0; i < nodes.size(); ++i) {
                    if (nodes[i]->id == startPin->nodeId) {
                        source.copiedIndex = static_cast<int>(i);
                        break;
                    }
                }
                // 集合の外なら、**元の親へ繋いだまま**にする。
                if (source.copiedIndex < 0) {
                    source.externalPin = link.startPin;
                }
                break;
            }
            entry.inputs.push_back(source);
        }
        m_graphClipboard.push_back(std::move(entry));
    }
    TG_LOG_INFO("ノードをコピーしました: %zu 個", m_graphClipboard.size());
}

void Application::PasteGraphNodes(const ImVec2& viewCenter) {
    if (m_graphClipboard.empty()) {
        return;
    }
    // 貼るたびに少しずらす。同じ場所に重ねると、貼れたのかどうか分からない。
    // **4 回で一巡させる。** 増やし続けると、貼るほど画面の中央から遠ざかる。
    ++m_graphPasteCount;
    const float offset = 28.0f * static_cast<float>(m_graphPasteCount % 4);

    // **貼る先は今見えている所**。コピー元が画面の外にあっても、貼ったノードが
    // どこかへ消えないように、集合の中心をキャンバスの中央へ持ってくる。
    // 集合の中の相対の配置はそのまま。
    float minX = m_graphClipboard.front().posX;
    float minY = m_graphClipboard.front().posY;
    float maxX = minX;
    float maxY = minY;
    for (const GraphClipboardNode& entry : m_graphClipboard) {
        minX = std::min(minX, entry.posX);
        minY = std::min(minY, entry.posY);
        maxX = std::max(maxX, entry.posX + entry.sizeX);
        maxY = std::max(maxY, entry.posY + entry.sizeY);
    }
    const float deltaX = viewCenter.x - (minX + maxX) * 0.5f + offset;
    const float deltaY = viewCenter.y - (minY + maxY) * 0.5f + offset;

    std::vector<graph::GraphId> created(m_graphClipboard.size(), 0);
    for (size_t i = 0; i < m_graphClipboard.size(); ++i) {
        const GraphClipboardNode& entry = m_graphClipboard[i];
        const graph::GraphId nodeId = m_graph.CreateNode(entry.kind);
        graph::Node* node = m_graph.FindMutableNode(nodeId);
        if (node == nullptr) {
            continue;
        }
        node->settings = entry.settings;
        node->posX = entry.posX + deltaX;
        node->posY = entry.posY + deltaY;
        node->positionValid = true;
        created[i] = nodeId;
        m_graphNodesToPlace.push_back(nodeId);
    }

    // 接続を張り直す。集合の中どうしは貼った側で、外は**元の親のまま**繋ぐ。
    // 出力側（自分を使っていた下流）は繋がない。入力ピンは 1 本しか持てないので、
    // 繋ぐと元のノードから奪ってしまう。
    for (size_t i = 0; i < m_graphClipboard.size(); ++i) {
        const graph::Node* node = m_graph.FindNode(created[i]);
        if (node == nullptr) {
            continue;
        }
        const GraphClipboardNode& entry = m_graphClipboard[i];
        for (size_t pinIndex = 0; pinIndex < entry.inputs.size(); ++pinIndex) {
            if (pinIndex >= node->inputs.size()) {
                break;
            }
            const GraphClipboardNode::Source& source = entry.inputs[pinIndex];
            const graph::GraphId endPin = node->inputs[pinIndex].id;
            if (source.copiedIndex >= 0 &&
                static_cast<size_t>(source.copiedIndex) < created.size()) {
                const graph::Node* upstream = m_graph.FindNode(created[source.copiedIndex]);
                if (upstream != nullptr && !upstream->outputs.empty()) {
                    m_graph.CreateLink(upstream->outputs.front().id, endPin);
                }
            } else if (source.externalPin != 0) {
                // 元のノードが消えていれば CanCreateLink が弾く（何も起きない）。
                m_graph.CreateLink(source.externalPin, endPin);
            }
        }
    }

    for (const graph::GraphId id : created) {
        if (id != 0) {
            m_selectedGraphNode = id;
            break;
        }
    }
    MarkDocumentChanged();
    TG_LOG_INFO("ノードを貼り付けました: %zu 個", m_graphClipboard.size());
}

void Application::DrawGraphNode(const graph::Node& node) {
    // ノードの幅。**ピンのラベルが重ならない幅まで広げる。**
    // 入力は左、出力は右へ寄せるので、同じ行に並ぶ 2 つのラベルの合計が要る幅になる。
    // 200px 固定にしていたときは、Mask Blend の Foreground / Background のような
    // 長い名前が出力の Mask と重なっていた。
    constexpr float kNodeMinWidth = 200.0f;
    // 丸ピン 1 つぶん（丸の幅 + ImGui の項目間隔）。
    const float pinWidth = 14.0f + ImGui::GetStyle().ItemSpacing.x;
    float rowWidth = 0.0f;
    for (size_t row = 0; row < std::max(node.inputs.size(), node.outputs.size()); ++row) {
        float width = 0.0f;
        if (row < node.inputs.size()) {
            width += pinWidth + ImGui::CalcTextSize(node.inputs[row].label.c_str()).x;
        }
        if (row < node.outputs.size()) {
            width += ImGui::CalcTextSize(node.outputs[row].label.c_str()).x + pinWidth;
        }
        rowWidth = std::max(rowWidth, width);
    }
    // 入力と出力のラベルの間に隙間を空ける。詰まっていると 1 語に見える。
    const float kNodeWidth = std::max(kNodeMinWidth, rowWidth + 24.0f);
    const ImVec4 accent = NodeAccentColor(node.kind);
    // **プレビュー中のノードは枠を明るくする。** 選択（プロパティ）と
    // プレビューは別なので、どれが画面に出ているのかが分かるようにする。
    const bool isPreview = (node.id == m_previewGraphNode) ||
                           (m_previewGraphNode == 0 && node.kind == graph::NodeKind::Output);
    const ImVec4 nodeBorderColor = isPreview ? ImVec4(0.72f, 0.76f, 0.62f, 1.0f)
                                             : ImVec4(0.22f, 0.22f, 0.22f, 1.0f);
    const ImVec4 activeNodeBorderColor(0.59f, 0.64f, 0.68f, 1.0f);
    ed::PushStyleVar(ed::StyleVar_NodePadding, ImVec4(12.0f, 10.0f, 12.0f, 10.0f));
    ed::PushStyleVar(ed::StyleVar_NodeRounding, 6.0f);
    ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, isPreview ? 2.0f : 1.0f);
    ed::PushStyleVar(ed::StyleVar_SelectedNodeBorderWidth, 1.8f);
    ed::PushStyleColor(ed::StyleColor_NodeBg, ImVec4(0.150f, 0.150f, 0.150f, 0.98f));
    ed::PushStyleColor(ed::StyleColor_NodeBorder, nodeBorderColor);
    ed::PushStyleColor(ed::StyleColor_HovNodeBorder, activeNodeBorderColor);
    ed::PushStyleColor(ed::StyleColor_SelNodeBorder, activeNodeBorderColor);

    ed::BeginNode(ed::NodeId(node.id));

    // ヘッダ: 種類色の印 + 名前。レイヤーが無効なら名前を落とした色で描く。
    const auto* layerSettings = std::get_if<graph::LayerNodeSettings>(&node.settings);
    const bool enabled = (layerSettings == nullptr) || layerSettings->layer.enabled;
    {
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(ImVec2(cursor.x, cursor.y + 3.0f),
                                ImVec2(cursor.x + 10.0f, cursor.y + 13.0f),
                                ColorToU32(accent), 2.0f);
        ImGui::Dummy(ImVec2(16.0f, 16.0f));
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.0f);
        const ImVec4 titleColor =
            enabled ? ImVec4(0.88f, 0.88f, 0.88f, 1.0f) : ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
        ImGui::TextColored(titleColor, "%s", NodeDisplayName(node));
        if (isPreview) {
            // ビューポートに出ている印。名前の右に小さく添える。
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.72f, 0.76f, 0.62f, 1.0f), "●");
        }
        // 種類はヘッダの下に小さく添える。名前と種類の両方が分かるようにする。
        if (const graph::NodeDefinition* definition = graph::FindNodeDefinition(node.kind);
            definition != nullptr && layerSettings != nullptr) {
            ImGui::TextColored(ImVec4(0.55f, 0.57f, 0.55f, 1.0f), "%s%s", definition->title,
                               enabled ? "" : "（無効）");
        }
    }

    // サムネイル。**繋ぎ替えずに中身が分かる**ようにするためのもの。
    //   - レイヤーのノード（Heightmap / Surface / Shape / 加工…）: そのレイヤーまで
    //     合成した結果（アルベドに Height の勾配で陰影を付けたもの）。
    //     Mask 出力を持つ加工ノードは、隣に最初の Mask（白黒）も出す。
    //   - マスクのノード: 焼いたマスク（白黒）。
    //   - プレビューしていない枝は評価されないので、枠だけの空き（未評価）になる。
    {
        const float thumbnailSize = ui::Scaled(ui::kNodeThumbnail);
        if (graph::IsMaskNodeKind(node.kind)) {
            ImGui::Dummy(ImVec2(kNodeWidth, 2.0f));
            const D3D12_GPU_DESCRIPTOR_HANDLE handle = GraphMaskThumbnail(node.id, 0);
            ui::ThumbnailImage(static_cast<ImTextureID>(handle.ptr), thumbnailSize);
        } else if (layerSettings != nullptr) {
            ImGui::Dummy(ImVec2(kNodeWidth, 2.0f));
            D3D12_GPU_DESCRIPTOR_HANDLE result = GraphLayerThumbnail(node.id);
            // 合成結果が無いとき（メッシュシーン表示中や未評価の枝）は、割り当てた材質のサムネイルを出す。
            // 繋ぎ替えずに何の材質かが分かればよいので、球の絵で足りる。
            if (result.ptr == 0 && layerSettings->layer.material != compositor::kNoMaterialAsset) {
                if (const compositor::MaterialAsset* asset = m_materialLibrary.Find(layerSettings->layer.material);
                    asset != nullptr && asset->thumbnail.IsValid()) {
                    result = asset->thumbnail.srv.gpu;
                }
            }
            ui::ThumbnailImage(static_cast<ImTextureID>(result.ptr), thumbnailSize);
            // マテリアル一覧からサムネイルへ落とすと、そのノードに割り当たる
            // （Surface だけ）。ID の無いアイテムでも BeginDragDropTarget は矩形から
            // ID を作るので受けられる。
            if (node.kind == graph::NodeKind::Surface && ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(kMaterialDragDropType);
                    payload != nullptr) {
                    const auto dropped =
                        *static_cast<const compositor::MaterialAssetId*>(payload->Data);
                    if (graph::Node* mutableNode = m_graph.FindMutableNode(node.id)) {
                        if (auto* mutableSettings =
                                std::get_if<graph::LayerNodeSettings>(&mutableNode->settings);
                            mutableSettings != nullptr &&
                            mutableSettings->layer.material != dropped) {
                            mutableSettings->layer.material = dropped;
                            m_graph.MarkDirty();
                            MarkDocumentChanged();
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (graph::IsLayerMaskSourceKind(node.kind)) {
                ImGui::SameLine();
                const D3D12_GPU_DESCRIPTOR_HANDLE mask = GraphMaskThumbnail(node.id, 0);
                ui::ThumbnailImage(static_cast<ImTextureID>(mask.ptr), thumbnailSize);
            }
        }
    }

    ImGui::Dummy(ImVec2(kNodeWidth, 8.0f));
    const float rowStartX = ImGui::GetCursorPosX();
    const float rowY = ImGui::GetCursorPosY();

    // **ラベルもピンの当たり判定に入れる。** 丸だけだと小さく、
    // 出力ピンのクリック（プレビューの切り替え）も接続も狙いにくい。
    const ImVec4 pinLabelColor(0.62f, 0.64f, 0.62f, 1.0f);

    for (size_t inputIndex = 0; inputIndex < node.inputs.size(); ++inputIndex) {
        const graph::Pin& input = node.inputs[inputIndex];
        const float inputY = rowY + static_cast<float>(inputIndex) * 24.0f;
        ImGui::SetCursorPos(ImVec2(rowStartX, inputY));
        ed::BeginPin(ed::PinId(input.id), ed::PinKind::Input);
        const PinGeometry geometry = DrawRoundPin(input);
        ImGui::SameLine();
        ImGui::SetCursorPosY(inputY + 2.0f);
        ImGui::TextColored(pinLabelColor, "%s", input.label.c_str());
        // 丸からラベルの右端まで。**縦は丸の高さに揃える**（行が重ならないように）。
        ed::PinRect(geometry.min, ImVec2(ImGui::GetItemRectMax().x, geometry.max.y));
        ed::EndPin();
    }

    for (size_t outputIndex = 0; outputIndex < node.outputs.size(); ++outputIndex) {
        const graph::Pin& output = node.outputs[outputIndex];
        // **どの出力を見ているか**を丸の塗りで示す。堆積のように出力が 2 つある
        // ノードでは、Result と Mask のどちらが画面に出ているのかが要る。
        const bool previewOutput = isPreview && ((output.id == m_previewGraphPin) ||
                                                 (m_previewGraphPin == 0 && outputIndex == 0));
        const float outputY = rowY + static_cast<float>(outputIndex) * 24.0f;
        const float labelWidth = ImGui::CalcTextSize(output.label.c_str()).x;
        ImGui::SetCursorPos(ImVec2(rowStartX + kNodeWidth - labelWidth - 22.0f, outputY + 2.0f));
        ed::BeginPin(ed::PinId(output.id), ed::PinKind::Output);
        ImGui::TextColored(pinLabelColor, "%s", output.label.c_str());
        const ImVec2 labelMin = ImGui::GetItemRectMin();
        ImGui::SameLine();
        ImGui::SetCursorPosY(outputY);
        const PinGeometry geometry = DrawRoundPin(output, previewOutput);
        // ラベルの左端から丸まで。
        ed::PinRect(ImVec2(labelMin.x, geometry.min.y), geometry.max);
        ed::EndPin();
    }
    const size_t pinRowCount = std::max(node.inputs.size(), node.outputs.size());
    ImGui::Dummy(
        ImVec2(kNodeWidth, std::max(4.0f, static_cast<float>(pinRowCount) * 24.0f - 20.0f)));

    ed::EndNode();
    ed::PopStyleColor(4);
    ed::PopStyleVar(4);
}

void Application::DrawGraphEditor() {
    if (m_nodeEditor == nullptr) {
        ed::Config config{};
        // 位置は Node が持ち、プロジェクトに保存する。エディタ側の設定ファイルは使わない。
        config.SettingsFile = nullptr;
        config.NavigateButtonIndex = 2;
        m_nodeEditor = ed::CreateEditor(&config);
        RequestGraphNodePlacement();
    }

    static ImVec2 addNodePosition(0.0f, 0.0f);
    const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 canvasMax(canvasMin.x + avail.x, canvasMin.y + avail.y);
    // ホバー判定は ed::Begin より前に取る。フレーム内では io.MousePos が
    // キャンバス座標に差し替えられていて、スクリーン座標の矩形と比べられない。
    const bool canvasHovered = ImGui::IsMouseHoveringRect(canvasMin, canvasMax);

    ed::SetCurrentEditor(m_nodeEditor);
    ed::PushStyleColor(ed::StyleColor_Bg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ed::PushStyleColor(ed::StyleColor_Grid, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ed::Begin("terrainGraphEditor", avail);
    DrawGraphDots(canvasMin, canvasMax);


    // 積まれた位置要求をエディタへ流し込む。まだ位置を持たないノード
    // （と、壊れた座標を持つノード）には現在のビューの中央を与える。
    if (!m_graphNodesToPlace.empty()) {
        for (const graph::GraphId nodeId : m_graphNodesToPlace) {
            graph::Node* node = m_graph.FindMutableNode(nodeId);
            if (node == nullptr) {
                continue;
            }
            if (!node->positionValid || !IsValidNodePosition(node->posX, node->posY)) {
                const ImVec2 center = ed::ScreenToCanvas(
                    ImVec2((canvasMin.x + canvasMax.x) * 0.5f, (canvasMin.y + canvasMax.y) * 0.5f));
                node->posX = center.x;
                node->posY = center.y;
                node->positionValid = true;
            }
            ed::SetNodePosition(ed::NodeId(node->id), ImVec2(node->posX, node->posY));
            // 追加やアンドゥで選ばれたノードは、エディタ側の選択も合わせる。
            // 合わせないと、次のフレームの選択同期（未選択 → 0）に消されてしまう。
            if (nodeId == m_selectedGraphNode) {
                ed::SelectNode(ed::NodeId(nodeId));
            }
        }
        m_graphNodesToPlace.clear();
    }

    for (const graph::Node& node : m_graph.Nodes()) {
        DrawGraphNode(node);
    }

    // A でグラフ全体を画面に収める（ビューポートの A と同じ作法）。
    // 内容の矩形は live なノードから計算されるため、描画の後に呼ぶ。
    const ImGuiIO& io = ImGui::GetIO();
    if (canvasHovered && !io.WantTextInput && !io.KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        ed::NavigateToContent();
    }

    // Ctrl+C / Ctrl+V でノードをコピーする。**キャンバスの上にいるときだけ**
    // 拾う（名前の入力中や他のパネルの操作を横取りしない）。
    if (canvasHovered && !io.WantTextInput && io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            CopySelectedGraphNodes();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
            PasteGraphNodes(ed::ScreenToCanvas(ImVec2((canvasMin.x + canvasMax.x) * 0.5f,
                                                      (canvasMin.y + canvasMax.y) * 0.5f)));
        }
    }

    // 位置を流し込んだ後の整列。**ノードを描いた後**でないと内容の矩形が空で
    // 何も起きない（live なノードから計算されるため）。さらに、エディタは
    // キャンバスのサイズ変化のたびに前の表示領域を復元する（ed::Begin 内）ので、
    // ドックの確定を待ってサイズが安定してから寄せる。
    const bool canvasStable =
        (avail.x == m_graphCanvasSize.x && avail.y == m_graphCanvasSize.y);
    m_graphCanvasSize = avail;
    if (m_graphNavigateCountdown > 0 && m_graphNodesToPlace.empty() && canvasStable) {
        if (--m_graphNavigateCountdown == 0) {
            ed::NavigateToContent(0.0f);
        }
    }

    for (const graph::Link& link : m_graph.Links()) {
        ImVec4 color(0.52f, 0.60f, 0.55f, 1.0f);
        if (const graph::Pin* startPin = m_graph.FindPin(link.startPin)) {
            color = PinTypeColor(startPin->valueType);
        }
        ed::Link(ed::LinkId(link.id), ed::PinId(link.startPin), ed::PinId(link.endPin), color,
                 2.5f);
    }

    // --- リンクの作成 -------------------------------------------------------
    if (ed::BeginCreate(ImVec4(0.52f, 0.70f, 0.59f, 1.0f), 2.5f)) {
        ed::PinId startPinId;
        ed::PinId endPinId;
        if (ed::QueryNewLink(&startPinId, &endPinId)) {
            const int startPin = ToGraphId(startPinId.Get());
            const int endPin = ToGraphId(endPinId.Get());
            if (m_graph.CanCreateLink(startPin, endPin)) {
                if (ed::AcceptNewItem(ImVec4(0.70f, 0.78f, 0.72f, 1.0f), 3.0f)) {
                    if (m_graph.CreateLink(startPin, endPin)) {
                        MarkDocumentChanged();
                    }
                }
            } else {
                ed::RejectNewItem(ImVec4(0.78f, 0.28f, 0.24f, 1.0f), 2.0f);
            }
        }
    }
    ed::EndCreate();

    // --- リンクとノードの削除 -----------------------------------------------
    if (ed::BeginDelete()) {
        ed::LinkId deletedLinkId;
        while (ed::QueryDeletedLink(&deletedLinkId)) {
            if (ed::AcceptDeletedItem()) {
                if (m_graph.DeleteLink(ToGraphId(deletedLinkId.Get()))) {
                    MarkDocumentChanged();
                }
            }
        }
        ed::NodeId deletedNodeId;
        while (ed::QueryDeletedNode(&deletedNodeId)) {
            if (ed::AcceptDeletedItem()) {
                const int nodeId = ToGraphId(deletedNodeId.Get());
                if (m_graph.DeleteNode(nodeId)) {
                    MarkDocumentChanged();
                    if (m_previewGraphNode == nodeId) {
                        m_previewGraphNode = 0;
                        m_previewGraphPin = 0;
                    }
                    if (m_selectedGraphNode == nodeId) {
                        m_selectedGraphNode = 0;
                    }
                }
            }
        }
    }
    ed::EndDelete();

    // --- 背景の右クリックでノードを追加 -------------------------------------
    if (ed::ShowBackgroundContextMenu()) {
        // エディタのフレーム内では io.MousePos が**キャンバス座標に差し替えられている**
        // （imgui_canvas が Begin で変換する）。そのまま使う。ScreenToCanvas を
        // 重ねると二重変換になり、ノードが視界の外へ飛ぶ（実際に踏んだ）。
        addNodePosition = ImGui::GetMousePos();
        // 念のため現在の視界へ収める。視界の外に生まれると見失う。
        // 深いズームでは上限が下限を割り得るので、max で順序を保証する。
        const ImVec2 viewMin = ed::ScreenToCanvas(canvasMin);
        const ImVec2 viewMax = ed::ScreenToCanvas(canvasMax);
        const float loX = viewMin.x + 16.0f;
        const float loY = viewMin.y + 16.0f;
        addNodePosition.x = std::clamp(addNodePosition.x, loX, std::max(loX, viewMax.x - 240.0f));
        addNodePosition.y = std::clamp(addNodePosition.y, loY, std::max(loY, viewMax.y - 120.0f));
        ed::Suspend();
        ImGui::OpenPopup("addGraphNode");
        ed::Resume();
    }
    ed::Suspend();
    if (ImGui::BeginPopup("addGraphNode")) {
        ImGui::TextDisabled("ノードを追加");
        ImGui::Separator();
        const auto addNodeMenuItem = [&](graph::NodeKind kind, const char* label) {
            if (!ImGui::MenuItem(label)) {
                return;
            }
            const graph::GraphId nodeId = m_graph.CreateNode(kind);
            graph::Node* node = m_graph.FindMutableNode(nodeId);
            if (node == nullptr) {
                TG_LOG_WARN("ノードを追加できませんでした（種類の定義が見つかりません）");
                return;
            }
            if (auto* settings = std::get_if<graph::LayerNodeSettings>(&node->settings)) {
                // 追加時の初期値は旧レイヤーパネルと同じ既定値を使う。
                settings->layer = (kind == graph::NodeKind::Heightmap)
                                      ? kDefaultHeightmapLayer
                                      : DefaultLayerFor(graph::LayerKindFor(kind));
                settings->layer.name +=
                    " " + std::to_string(m_graph.Nodes().size());
            }
            node->posX = addNodePosition.x;
            node->posY = addNodePosition.y;
            node->positionValid = true;
            m_graphNodesToPlace.push_back(nodeId);
            m_selectedGraphNode = nodeId;
            // 作った直後は、その結果を見たいはず。プレビューも移す。
            SetPreviewGraphNode(nodeId);
            MarkDocumentChanged();
            // ステータスバーに残す。追加が効いたかを画面で確かめられるようにする。
            TG_LOG_INFO("ノードを追加しました: %s", NodeDisplayName(*node));
        };
        addNodeMenuItem(graph::NodeKind::Road, "Road — Pathから道路面と左右境界を生成");
        addNodeMenuItem(graph::NodeKind::RoadMarking, "Lane Marking — 道路面に白線の帯を生成");
        addNodeMenuItem(graph::NodeKind::RoadMask, "Road Mask — 轍・端・ムラの道路空間マスク");
        addNodeMenuItem(graph::NodeKind::Decal, "Decal — 面上のPathに沿って模様の帯を貼る");
        addNodeMenuItem(graph::NodeKind::Shoulder, "Shoulder — 道路の境界から外側へ路肩を張る");
        addNodeMenuItem(graph::NodeKind::Merge, "Merge — 複数のRoadSurfaceを1つにまとめる");
        addNodeMenuItem(graph::NodeKind::Crack, "Crack — ひび割れの塊を乱数で配置する");
        addNodeMenuItem(graph::NodeKind::MeshOutput, "Mesh Output — 道路メッシュを表示");
        ImGui::Separator();
        addNodeMenuItem(graph::NodeKind::Heightmap, "Heightmap — 画像を地形として読み込む");
        ImGui::Separator();
        addNodeMenuItem(graph::NodeKind::Surface, "Surface — 素材を高さで張り合わせる");
        addNodeMenuItem(graph::NodeKind::Shape, "Shape — 高さへ起伏を加算する");
        addNodeMenuItem(graph::NodeKind::Liquid, "Liquid — 水位より低い所に水を張る");
        ImGui::Separator();
        addNodeMenuItem(graph::NodeKind::Blur, "Heightmap Blur — ハイトをぼかしてならす");
        addNodeMenuItem(graph::NodeKind::Sediment,
                        "Sediment — 土砂を重力で再分配して谷に積もらせる");
        addNodeMenuItem(graph::NodeKind::Crumbling,
                        "Crumbling — 崩れた岩屑を斜面下へ流して積む");
        addNodeMenuItem(graph::NodeKind::Snow,
                        "Snow — 雪を降らせ、急な雪面から落として積もらせる");
        addNodeMenuItem(graph::NodeKind::River,
                        "River — 川筋から河床を掘り、下流へ下がる水面を張る");
        addNodeMenuItem(graph::NodeKind::Droplet,
                        "Droplet Erosion — 水滴を流して谷を刻み、土砂を運んで積む");
        addNodeMenuItem(graph::NodeKind::Scatter,
                        "Scatter — 単純な形をばら撒き、分布のマスクを出す");
        ImGui::Separator();
        addNodeMenuItem(graph::NodeKind::MaskImage,
                        "Mask Image — 画像をマスクにする（白い所だけ乗る）");
        addNodeMenuItem(graph::NodeKind::MaskNoise,
                        "Mask Noise — ノイズをマスクにする（下地に依らない）");
        addNodeMenuItem(graph::NodeKind::MaskFluvial,
                        "Mask Fluvial — 下地の川筋をマスクにする");
        addNodeMenuItem(graph::NodeKind::MaskHeight,
                        "Mask Height — 下地の標高帯（m）をマスクにする");
        addNodeMenuItem(graph::NodeKind::MaskSlope,
                        "Mask Slope — 下地の傾斜（角度）をマスクにする");
        addNodeMenuItem(graph::NodeKind::MaskCurvature,
                        "Mask Curvature — 下地の凹凸（尾根 / 谷）をマスクにする");
        addNodeMenuItem(graph::NodeKind::MaskLevels,
                        "Mask Levels — マスクの黒点 / 白点 / ガンマを調整する");
        addNodeMenuItem(graph::NodeKind::MaskBlur,
                        "Mask Blur — マスクをぼかして境界をなだらかにする");
        addNodeMenuItem(graph::NodeKind::MaskBlend,
                        "Mask Blend — マスク 2 枚を合成する");
        ImGui::Separator();
        addNodeMenuItem(graph::NodeKind::Path,
                        "Path — 実寸の3次元カーブを編集");
        addNodeMenuItem(graph::NodeKind::MaskPath,
                        "Mask Path — パスの足跡をマスクにする");
        addNodeMenuItem(graph::NodeKind::MaskArea,
                        "Mask Area — パスの閉じた鎖の内側をマスクにする（エリア選択）");
        ImGui::Separator();
        addNodeMenuItem(graph::NodeKind::Output, "Output — ここに繋いだ結果をプレビューする");
        ImGui::EndPopup();
    }
    ed::Resume();

    // --- プレビュー対象の切り替え -------------------------------------------
    // **選択とは別。** ノードを選んでプロパティをいじりながら、別のノードの
    // 出力をビューポートに出しておけるようにする（terrain-editor と同じ作法）。
    //
    // 出力ピンは押した瞬間からリンクのドラッグが始まるので、
    // **同じピンの上でほとんど動かずに離したとき**だけクリックとみなす。
    {
        const graph::GraphId hoveredPin = ToGraphId(ed::GetHoveredPin().Get());
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            m_graphPressedPin = hoveredPin;
            m_graphPressedPinPos = ImGui::GetMousePos();
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            const ImVec2 released = ImGui::GetMousePos();
            const float moved = std::abs(released.x - m_graphPressedPinPos.x) +
                                std::abs(released.y - m_graphPressedPinPos.y);
            if (m_graphPressedPin != 0 && hoveredPin == m_graphPressedPin && moved < 6.0f) {
                if (const graph::Pin* pin = m_graph.FindPin(m_graphPressedPin);
                    pin != nullptr && pin->kind == graph::PinKind::Output) {
                    // 押したピンそのものを見る（堆積の Mask をクリックすれば
                    // 積もった厚みが白黒で出る）。
                    SetPreviewGraphNode(pin->nodeId, pin->id);
                }
            }
            m_graphPressedPin = 0;
        }
        // ノードのダブルクリックでも切り替える（ピンが小さいときの逃げ道）。
        if (const ed::NodeId doubleClicked = ed::GetDoubleClickedNode()) {
            SetPreviewGraphNode(ToGraphId(doubleClicked.Get()));
        }
        // 背景のダブルクリックで出力ノードのチェーンへ戻す。
        if (ed::IsBackgroundDoubleClicked()) {
            SetPreviewGraphNode(0);
        }
    }

    // --- 選択 ---------------------------------------------------------------
    // 選択はプロパティに出すノード。外したら 0 に戻す（プレビューには影響しない）。
    // **配置待ちのノードがある間は消さない。** 追加した直後のフレームは
    // エディタ側の選択がまだ無く、ここで 0 に戻すと「追加 → 選択」が消える
    // （エディタへの選択の反映は次のフレームの流し込みで行う）。
    // コピーは複数選択（枠で囲む）にも効かせたいので、全部控えておく。
    ed::NodeId selectedNodes[64];
    const int selectedCount = ed::GetSelectedNodes(selectedNodes, IM_ARRAYSIZE(selectedNodes));
    if (selectedCount > 0) {
        m_selectedGraphNodes.clear();
        for (int i = 0; i < selectedCount; ++i) {
            m_selectedGraphNodes.push_back(ToGraphId(selectedNodes[i].Get()));
        }
        m_selectedGraphNode = m_selectedGraphNodes.front();
    } else if (m_graphNodesToPlace.empty()) {
        m_selectedGraphNodes.clear();
        m_selectedGraphNode = 0;
    }

    ed::End();

    // エディタが持つ位置をノードへ書き戻す（保存はここから読む）。
    // **このフレーム中に作られたばかりでエディタが知らないノードは飛ばす。**
    // エディタは知らないノードに FLT_MAX を返すため、書き戻すと次の流し込みで
    // ノードが無限遠へ飛び、キャンバスが操作不能になる（実際に踏んだ）。
    for (graph::Node& node : m_graph.MutableNodes()) {
        const ImVec2 position = ed::GetNodePosition(ed::NodeId(node.id));
        if (!IsValidNodePosition(position.x, position.y)) {
            continue;
        }
        node.posX = position.x;
        node.posY = position.y;
        node.positionValid = true;
    }
    ed::PopStyleColor(2);
    ed::SetCurrentEditor(nullptr);
}

void Application::DrawGraphPanel() {
    // 既定レイアウトを組んだ直後は、右カラムの前面タブをこのパネルにする。
    if (m_focusDefaultTabs > 0) {
        ImGui::SetNextWindowFocus();
    }
    if (!ImGui::Begin("グラフ")) {
        ImGui::End();
        return;
    }

    if (m_renderer.HasMeshScene()) {
        ui::HintText("メッシュシーンを表示中。Path → Road → Mesh Outputで道路を生成できます。");
        if (ui::BeginPropertyTable("meshSceneRows")) {
            ui::PropertyValue("メッシュ数", "%zu", m_renderer.Scene().meshes.size());
            ui::EndPropertyTable();
        }
    }
    if (const graph::Node* selected = m_graph.FindNode(m_selectedGraphNode);
        selected != nullptr && graph::IsLayerNodeKind(selected->kind)) {
        ui::HintText("選択したノードまでを表示中（選択を外すと出力まで）");
    } else {
        ui::HintText("出力ノードへ繋いだチェーンがプレビューになる");
    }

    float editorHeight = ui::Scaled(m_graphEditorHeight);
    const float paneWidth = ImGui::GetContentRegionAvail().x;
    const float maxHeight =
        std::max(ui::Scaled(160.0f), ImGui::GetContentRegionAvail().y - ui::Scaled(120.0f));
    ImGui::BeginChild("graphEditorPane", ImVec2(0.0f, editorHeight));
    DrawGraphEditor();
    ImGui::EndChild();

    ui::HorizontalSplitter("graphSplitter", &editorHeight, ui::Scaled(160.0f), maxHeight,
                           paneWidth);
    m_graphEditorHeight = editorHeight / std::max(ui::Scaled(1.0f), 0.01f);

    ImGui::BeginChild("graphPropertyPane", ImVec2(0.0f, 0.0f));

    // **プレビュー対象は選択とは別。** どれが画面に出ているかをここに出し、
    // 出力へ戻す手段も置く（出力ピンのクリックで切り替わる、と気づけるように）。
    if (m_meshGraphActive) {
        // 途中のメッシュノードを見ているときは、そのノード名を出して Mesh Output へ戻す手段を置く。
        const graph::Node* previewMeshNode = m_graph.FindNode(m_meshGraphPreviewNode);
        if (ui::BeginPropertyTable("meshGraphPreviewRow")) {
            ui::PropertyValue("プレビュー", "%s",
                              previewMeshNode != nullptr ? NodeDisplayName(*previewMeshNode) : "Mesh Output");
            ui::EndPropertyTable();
        }
        if (previewMeshNode != nullptr) {
            ui::HintText("このノードまでの道路メッシュを表示中。出力ピンのクリックで切り替わる。");
            if (ui::Button("Mesh Output へ戻す", ui::kWideButtonWidth)) {
                SetPreviewGraphNode(0);
            }
        } else {
            ui::HintText("Mesh Outputへ接続した道路を表示中。Pathを選択するとカーブを編集できます。");
        }
    } else {
        const graph::Node* previewNode = m_graph.FindNode(m_previewGraphNode);
        const char* previewName =
            (previewNode != nullptr) ? NodeDisplayName(*previewNode) : "Output";
        // 出力が 2 つ以上あるノードは、どちらを見ているのかも出す。
        const graph::Pin* previewPin = m_graph.FindPin(m_previewGraphPin);
        if (ui::BeginPropertyTable("graphPreviewRow")) {
            if (previewPin != nullptr && previewNode != nullptr &&
                previewNode->outputs.size() > 1) {
                ui::PropertyValue("プレビュー", "%s（%s）", previewName,
                                  previewPin->label.c_str());
            } else {
                ui::PropertyValue("プレビュー", "%s", previewName);
            }
            ui::EndPropertyTable();
        }
        if (previewNode != nullptr) {
            if (ui::Button("出力へ戻す", ui::kWideButtonWidth)) {
                SetPreviewGraphNode(0);
            }
        }
        ui::HintText("出力ピンをクリック（またはノードをダブルクリック）で、"
                     "ビューポートに出す出力を切り替える。"
                     "Mask の出力を選ぶと、そのマスクが白黒で貼られる");
        ImGui::Spacing();
    }

    if (!m_meshGraphError.empty()) ui::HintText("%s", m_meshGraphError.c_str());
    graph::Node* selected = m_graph.FindMutableNode(m_selectedGraphNode);
    if (selected == nullptr) {
        ui::HintText("ノードを選ぶと設定が出る。背景の右クリックで追加、"
                     "ピンをドラッグして接続、Ctrl+C / Ctrl+V でコピー");
    } else if (auto* road = std::get_if<graph::RoadNodeSettings>(&selected->settings)) {
        bool changed = false;
        const graph::RoadNodeSettings defaults;
        if (ui::BeginPropertyTable("roadRows")) {
            changed |= ui::PropertyFloat("道路幅", &road->widthMeters, 0.1f, 50.0f,
                defaults.widthMeters, "中心線から左右へ半分ずつ広げる全幅", "%.2f m");
            {
                int forward = static_cast<int>(road->lanesForward);
                int backward = static_cast<int>(road->lanesBackward);
                if (ui::PropertyInt("車線数（進行方向）", &forward, 1, 8, static_cast<int>(defaults.lanesForward),
                                    "線形の向きへ進む車線の数。どちら側に並ぶかは走行側で決まる")) {
                    road->lanesForward = static_cast<uint32_t>(forward);
                    changed = true;
                }
                if (ui::PropertyInt("車線数（対向）", &backward, 0, 8, static_cast<int>(defaults.lanesBackward),
                                    "対向車線の数。0 で一方通行（中央線は出ない）")) {
                    road->lanesBackward = static_cast<uint32_t>(backward);
                    changed = true;
                }
                ui::PropertyValue("車線幅", "%.2f m",
                                  road->widthMeters / static_cast<float>(std::max(1u, road->lanesForward) + road->lanesBackward));
            }
            changed |= ui::PropertyFloat("UV反復長", &road->uvRepeatMeters, 0.1f, 100.0f,
                defaults.uvRepeatMeters, "UVが1増える実距離。道路の長さと幅の両方に適用する", "%.2f m");
            {
                static const char* const kUvAxisLabels[] = {"長さ方向 = V（縦）", "長さ方向 = U（横）"};
                int axis = road->uvAlongU ? 1 : 0;
                if (ui::PropertyCombo("UVの向き", &axis, kUvAxisLabels, IM_ARRAYSIZE(kUvAxisLabels), 0,
                                      "テクスチャのどの軸を道路の長さ方向に沿わせるか。横長の素材は U")) {
                    road->uvAlongU = (axis == 1);
                    changed = true;
                }
            }
            changed |= ui::PropertyFloat("変位量", &road->displacementMeters, 0.0f, 1.0f,
                defaults.displacementMeters,
                "Materialのハイトで路面を法線方向へ押し出す量。ハイト0〜1の全幅がこの高さ（m）。"
                "0なら形は変わらない。テセレーションはプレビュー設定の「道路」で", "%.3f m", 0, 0.005f);
            ui::PropertyValue("走行側", "%s", m_graph.RoadNetwork().leftHandTraffic ? "左側通行" : "右側通行");
            ui::EndPropertyTable();
        }
        // 材質スロット。1 は下地、2〜4 は Mask 2〜4 で被覆する。座標と反復長はスロットごと。
        changed |= DrawMaterialSlotRows(*selected, road->layerWorldUv, road->layerUvRepeatMeters,
                                        road->layerBlendRange, defaults.layerBlendRange);
        ui::HintText("Material にSurfaceなどのResultを接続して材質を適用。Material 2〜4 は Road Mask を Mask 2〜4 へ繋いだ所に出る。"
                     "RoadSurfaceはMesh Outputへ、Left / Rightは進行方向に向かって左右の境界Path。走行側はプレビュー設定の「道路」で切り替える。");
        if (changed) { m_graph.MarkDirty(); MarkDocumentChanged(); }
    } else if (auto* decal = std::get_if<graph::DecalNodeSettings>(&selected->settings)) {
        bool changed = false;
        const graph::DecalNodeSettings defaults;
        if (ui::BeginPropertyTable("decalRows")) {
            changed |= ui::PropertyFloat("幅", &decal->widthMeters, 0.05f, 50.0f, defaults.widthMeters, "帯の幅", "%.2f m");
            changed |= ui::PropertyFloat("浮かせ量", &decal->liftMeters, 0.0f, 0.1f, defaults.liftMeters,
                                         "路面から法線方向へ持ち上げる量", "%.3f m");
            changed |= ui::PropertyFloat("UV反復長", &decal->uvRepeatMeters, 0.05f, 100.0f, defaults.uvRepeatMeters,
                                         "帯の長さ方向で UV が 1 増える実距離。幅方向は 0〜1", "%.2f m");
            {
                static const char* const kUvAxisLabels[] = {"長さ方向 = V（縦）", "長さ方向 = U（横）"};
                int axis = decal->uvAlongU ? 1 : 0;
                if (ui::PropertyCombo("UVの向き", &axis, kUvAxisLabels, IM_ARRAYSIZE(kUvAxisLabels), 0,
                                      "テクスチャのどの軸を帯の長さ方向に沿わせるか")) {
                    decal->uvAlongU = (axis == 1);
                    changed = true;
                }
            }
            ui::EndPropertyTable();
        }
        ui::HintText("RoadのRoadSurfaceと、Surfaceにその道路を繋いだPathを接続する。Pathは路面の上でCtrl＋クリックして引く。"
                     "Materialの不透明度で模様をくり抜き、出力のRoadSurfaceをLane MarkingかMesh Outputへ。");
        if (changed) { m_graph.MarkDirty(); MarkDocumentChanged(); }
    } else if (auto* shoulder = std::get_if<graph::ShoulderNodeSettings>(&selected->settings)) {
        bool changed = false;
        const graph::ShoulderNodeSettings defaults;
        if (ui::BeginPropertyTable("shoulderRows")) {
            changed |= ui::PropertyFloat("幅", &shoulder->widthMeters, 0.1f, 50.0f, defaults.widthMeters,
                                         "境界から外側へ張る幅", "%.2f m");
            changed |= ui::PropertyFloat("横断勾配", &shoulder->crossSlopePercent, -50.0f, 50.0f, defaults.crossSlopePercent,
                                         "外側へ向かって下がる割合。1 m 進んで何 cm 下がるか", "%.1f %%");
            changed |= ui::PropertyFloat("段差", &shoulder->stepHeightMeters, 0.0f, 0.5f, defaults.stepHeightMeters,
                                         "舗装端の段差。0 より大きいと境界の直後に面取り列を挟み、路肩全体をこの高さだけ下げる",
                                         "%.3f m", 0, 0.005f);
            if (shoulder->stepHeightMeters > 0.0f) {
                changed |= ui::PropertyFloat("面取り幅", &shoulder->stepWidthMeters, 0.005f, 1.0f, defaults.stepWidthMeters,
                                             "境界から段差の底までの横幅。小さいほど垂直に近い", "%.3f m", 0, 0.005f);
            }
            changed |= ui::PropertyFloat("UV反復長", &shoulder->uvRepeatMeters, 0.1f, 100.0f, defaults.uvRepeatMeters,
                                         "UV が 1 増える実距離", "%.2f m");
            {
                static const char* const kUvAxisLabels[] = {"長さ方向 = V（縦）", "長さ方向 = U（横）"};
                int axis = shoulder->uvAlongU ? 1 : 0;
                if (ui::PropertyCombo("UVの向き", &axis, kUvAxisLabels, IM_ARRAYSIZE(kUvAxisLabels), 0,
                                      "テクスチャのどの軸を路肩の長さ方向に沿わせるか")) {
                    shoulder->uvAlongU = (axis == 1);
                    changed = true;
                }
            }
            changed |= ui::PropertyFloat("変位量", &shoulder->displacementMeters, 0.0f, 1.0f, defaults.displacementMeters,
                                         "Materialのハイトで路肩を法線方向へ押し出す量。ハイト0〜1の全幅がこの高さ（m）。"
                                         "境界で道路と同じ材質・同じ量にすると段が出ない", "%.3f m", 0, 0.005f);
            ui::EndPropertyTable();
        }
        changed |= DrawMaterialSlotRows(*selected, shoulder->layerWorldUv, shoulder->layerUvRepeatMeters,
                                        shoulder->layerBlendRange, defaults.layerBlendRange);
        ui::HintText("PathにRoadのLeft / Right（または別のShoulderのOuter）を接続する。境界の頂点を共有するので道路と水密。"
                     "材質スロットとMask 2〜4はRoadと同じ。Road Maskの「側」は路肩では 右＝境界側、左＝外側。"
                     "出力のRoadSurfaceをMesh Outputへ、Outerは次の路肩や縁石へ。走行側には依存しない。");
        if (changed) { m_graph.MarkDirty(); MarkDocumentChanged(); }
    } else if (auto* crack = std::get_if<graph::CrackNodeSettings>(&selected->settings)) {
        bool changed = false;
        const graph::CrackNodeSettings defaults;
        if (ui::BeginPropertyTable("crackRows")) {
            {
                int seed = static_cast<int>(crack->seed);
                if (ui::PropertyInt("乱数種", &seed, 0, 99999, static_cast<int>(defaults.seed), "変えると配置と形が変わる")) {
                    crack->seed = static_cast<uint32_t>(std::max(0, seed));
                    changed = true;
                }
            }
            changed |= ui::PropertyFloat("密度", &crack->densityPer100m, 0.0f, 200.0f, defaults.densityPer100m,
                                         "100 m あたりの塊の数", "%.1f /100m");
            changed |= ui::PropertyFloat("長さ（最小）", &crack->lengthMinMeters, 0.5f, 30.0f, defaults.lengthMinMeters,
                                         "幹の長さの下限", "%.1f m");
            changed |= ui::PropertyFloat("長さ（最大）", &crack->lengthMaxMeters, 0.5f, 30.0f, defaults.lengthMaxMeters,
                                         "幹の長さの上限。横向きは車線幅が上限", "%.1f m");
            if (crack->lengthMaxMeters < crack->lengthMinMeters) { crack->lengthMaxMeters = crack->lengthMinMeters; changed = true; }
            {
                static const char* const kOrientationLabels[] = {"縦（長さ方向）", "横（車線を横切る）", "混合"};
                int orientation = static_cast<int>(crack->orientation);
                if (ui::PropertyCombo("向き", &orientation, kOrientationLabels, IM_ARRAYSIZE(kOrientationLabels), 2,
                                      "幹の向き。混合は割合で混ぜる")) {
                    crack->orientation = static_cast<graph::CrackOrientation>(orientation);
                    changed = true;
                }
                if (crack->orientation == graph::CrackOrientation::Mixed) {
                    changed |= ui::PropertyFloat("横の割合", &crack->transverseRatio, 0.0f, 1.0f, defaults.transverseRatio,
                                                 "混合のときに横向きになる割合", "%.2f");
                }
            }
            changed |= ui::PropertyFloat("向きのばらつき", &crack->angleJitterDegrees, 0.0f, 90.0f, defaults.angleJitterDegrees,
                                         "幹の向きと曲がり方のばらつき", "%.0f°");
            {
                static const char* const kPlacementLabels[] = {"一様", "轍寄り", "端寄り"};
                int placement = static_cast<int>(crack->placement);
                if (ui::PropertyCombo("横位置", &placement, kPlacementLabels, IM_ARRAYSIZE(kPlacementLabels), 0,
                                      "塊の横位置の分布。轍寄りは Road の車線から決める")) {
                    crack->placement = static_cast<graph::CrackPlacement>(placement);
                    changed = true;
                }
            }
            changed |= ui::PropertyFloat("幹の幅", &crack->trunkWidthMeters, 0.01f, 1.0f, defaults.trunkWidthMeters,
                                         "幹の帯の幅。素材のアルファで割れ目の細さが決まるので、帯は少し広め", "%.3f m", 0, 0.005f);
            {
                int lo = static_cast<int>(crack->branchesMin);
                int hi = static_cast<int>(crack->branchesMax);
                if (ui::PropertyInt("枝の数（最小）", &lo, 0, 12, static_cast<int>(defaults.branchesMin), "幹から分かれる枝の本数の下限")) {
                    crack->branchesMin = static_cast<uint32_t>(lo); changed = true;
                }
                if (ui::PropertyInt("枝の数（最大）", &hi, 0, 12, static_cast<int>(defaults.branchesMax), "枝の本数の上限。半分の枝がさらに 1 本の子枝を出す")) {
                    crack->branchesMax = static_cast<uint32_t>(hi); changed = true;
                }
                if (crack->branchesMax < crack->branchesMin) { crack->branchesMax = crack->branchesMin; changed = true; }
            }
            changed |= ui::PropertyFloat("枝の長さ", &crack->branchLengthRatio, 0.05f, 2.0f, defaults.branchLengthRatio,
                                         "幹の長さに対する枝の長さの比", "%.2f");
            changed |= ui::PropertyFloat("枝の幅", &crack->branchWidthRatio, 0.05f, 1.0f, defaults.branchWidthRatio,
                                         "幹の幅に対する枝の根元の幅の比。先端で 0 へ絞る", "%.2f");
            changed |= ui::PropertyFloat("浮かせ量", &crack->liftMeters, 0.0f, 0.1f, defaults.liftMeters,
                                         "路面から法線方向へ持ち上げる量", "%.3f m");
            changed |= ui::PropertyFloat("UV反復長", &crack->uvRepeatMeters, 0.05f, 100.0f, defaults.uvRepeatMeters,
                                         "帯の長さ方向で UV が 1 増える実距離。幅方向は 0〜1", "%.2f m");
            {
                static const char* const kUvAxisLabels[] = {"長さ方向 = V（縦）", "長さ方向 = U（横）"};
                int axis = crack->uvAlongU ? 1 : 0;
                if (ui::PropertyCombo("UVの向き", &axis, kUvAxisLabels, IM_ARRAYSIZE(kUvAxisLabels), 0,
                                      "テクスチャのどの軸を帯の長さ方向に沿わせるか。1024×128 のような横長素材は U")) {
                    crack->uvAlongU = (axis == 1);
                    changed = true;
                }
            }
            ui::EndPropertyTable();
        }
        ui::HintText("RoadのRoadSurfaceを接続すると、3〜6 m の枝分かれしたひび割れを乱数で置く。Materialに割れ目の材質（マスク抜き）を繋ぐ。"
                     "個別に置きたいものは面上のPath＋Decalで描く。出力のRoadSurfaceをMergeかMesh Outputへ。");
        if (changed) { m_graph.MarkDirty(); MarkDocumentChanged(); }
    } else if (selected->kind == graph::NodeKind::Merge) {
        if (ui::BeginPropertyTable("mergeRows")) {
            ui::PropertyValue("入力", "%zu 本（空き 1）", selected->inputs.size());
            ui::EndPropertyTable();
        }
        ui::HintText("Road・Shoulder・Decal などのRoadSurfaceを繋ぐと、まとめて1つのRoadSurfaceにする。"
                     "繋ぐたびに入力が1本増える。同じノード由来のメッシュは1回だけ積む。下流の白線・Decalは Mesh 1 の面に乗る。");
    } else if (auto* roadMask = std::get_if<graph::RoadMaskNodeSettings>(&selected->settings)) {
        bool changed = false;
        const graph::RoadMaskNodeSettings defaults;
        if (ui::BeginPropertyTable("roadMaskRows")) {
            static const char* const kShapeLabels[] = {"轍", "端の減衰", "長さ方向ノイズ", "一様"};
            int shape = static_cast<int>(roadMask->shape);
            if (ui::PropertyCombo("形", &shape, kShapeLabels, IM_ARRAYSIZE(kShapeLabels), 0,
                                  "轍: 車線中央 ± タイヤ間隔/2 の帯。端の減衰: 道路端で 1。長さ方向ノイズ: しきい値で切る")) {
                roadMask->shape = static_cast<graph::RoadMaskShape>(shape);
                changed = true;
            }
            switch (roadMask->shape) {
                case graph::RoadMaskShape::WheelTracks:
                    changed |= ui::PropertyBool("車線に合わせる", &roadMask->tracksFromLanes, defaults.tracksFromLanes,
                                                "Road の車線数から各車線の中央に置く。路肩など車線の無い面では手入力の値を使う");
                    if (!roadMask->tracksFromLanes) {
                        changed |= ui::PropertyFloat("車線中央", &roadMask->laneOffsetMeters, 0.0f, 10.0f, defaults.laneOffsetMeters,
                                                     "中心線から車線中央までの距離", "%.2f m");
                    }
                    changed |= ui::PropertyFloat("タイヤ間隔", &roadMask->trackSpacingMeters, 0.5f, 3.0f, defaults.trackSpacingMeters,
                                                 "左右のタイヤの間隔", "%.2f m");
                    changed |= ui::PropertyFloat("帯の幅", &roadMask->trackWidthMeters, 0.05f, 2.0f, defaults.trackWidthMeters,
                                                 "轍 1 本の幅", "%.2f m");
                    changed |= ui::PropertyFloat("ぼかし", &roadMask->featherMeters, 0.0f, 2.0f, defaults.featherMeters,
                                                 "帯の縁を 0 へ落とす幅", "%.2f m");
                    changed |= ui::PropertyBool("両車線", &roadMask->bothLanes, defaults.bothLanes,
                                                "対向車線にも置く。手入力のときは中心線の左右両方に置く");
                    break;
                case graph::RoadMaskShape::EdgeFalloff: {
                    static const char* const kSideLabels[] = {"両側", "左", "右"};
                    int side = static_cast<int>(roadMask->edgeSide);
                    if (ui::PropertyCombo("側", &side, kSideLabels, IM_ARRAYSIZE(kSideLabels), 0,
                                          "どちらの端に出すか。左右は Path の進行方向基準（Road の Left / Right と同じ）。走行側には依存しない")) {
                        roadMask->edgeSide = static_cast<graph::RoadMaskSide>(side);
                        changed = true;
                    }
                    changed |= ui::PropertyFloat("端の幅", &roadMask->edgeWidthMeters, 0.0f, 5.0f, defaults.edgeWidthMeters,
                                                 "道路端から内側へ 1 のまま続く幅", "%.2f m");
                    changed |= ui::PropertyFloat("ぼかし", &roadMask->featherMeters, 0.0f, 5.0f, defaults.featherMeters,
                                                 "その内側で 0 へ落とす幅", "%.2f m");
                    break;
                }
                case graph::RoadMaskShape::LengthNoise:
                    changed |= ui::PropertyFloat("ノイズの大きさ", &roadMask->noiseScaleMeters, 0.1f, 50.0f, defaults.noiseScaleMeters,
                                                 "ノイズ 1 周期の実距離", "%.1f m", ImGuiSliderFlags_Logarithmic);
                    changed |= ui::PropertyFloat("しきい値", &roadMask->threshold, 0.0f, 1.0f, defaults.threshold,
                                                 "これより大きい所が 1", "%.2f");
                    changed |= ui::PropertyFloat("柔らかさ", &roadMask->softness, 0.01f, 1.0f, defaults.softness,
                                                 "しきい値まわりの遷移幅", "%.2f");
                    break;
                default:
                    break;
            }
            changed |= ui::PropertyFloat("ムラ", &roadMask->breakupAmount, 0.0f, 1.0f, defaults.breakupAmount,
                                         "長さ方向のノイズを掛ける量。0 で一様", "%.2f");
            if (roadMask->breakupAmount > 0.0f) {
                changed |= ui::PropertyFloat("ムラの大きさ", &roadMask->breakupScaleMeters, 0.1f, 50.0f, defaults.breakupScaleMeters,
                                             "ムラ 1 周期の実距離", "%.1f m", ImGuiSliderFlags_Logarithmic);
            }
            int seed = static_cast<int>(roadMask->seed);
            if (ui::PropertyInt("シード", &seed, 0, 9999, static_cast<int>(defaults.seed), "ノイズの並びを変える")) {
                roadMask->seed = static_cast<uint32_t>(std::max(0, seed));
                changed = true;
            }
            changed |= ui::PropertyFloat("強さ", &roadMask->strength, 0.0f, 1.0f, defaults.strength, "全体に掛ける倍率", "%.2f");
            changed |= ui::PropertyBool("反転", &roadMask->invert, defaults.invert, "1 − 値にする");
            ui::EndPropertyTable();
        }
        ui::HintText("Mask を Road の Mask 2〜4 へ繋ぐと、対応する Material 2〜4 の被覆率になる。横位置と実距離で決まり、タイルは繰り返さない。");
        if (changed) { m_graph.MarkDirty(); MarkDocumentChanged(); }
    } else if (auto* marking = std::get_if<graph::RoadMarkingNodeSettings>(&selected->settings)) {
        bool changed = false;
        const graph::RoadMarkingNodeSettings defaults;
        if (ui::BeginPropertyTable("roadMarkingRows")) {
            changed |= ui::PropertyBool("中央線", &marking->centerLine, defaults.centerLine,
                "進行方向と対向の境に1本引く。Road の車線数で位置が決まる。一方通行なら出ない");
            changed |= ui::PropertyBool("外側線", &marking->edgeLines, defaults.edgeLines,
                "左右の道路端の手前に1本ずつ引く");
            changed |= ui::PropertyBool("車線境界線", &marking->laneLines, defaults.laneLines,
                "同方向の車線の間に破線で引く。Road の車線数が片側 2 以上のときに出る");
            if (marking->laneLines) {
                changed |= ui::PropertyFloat("破線の長さ", &marking->dashLengthMeters, 0.1f, 50.0f,
                    defaults.dashLengthMeters, "破線 1 本の長さ", "%.1f m");
                changed |= ui::PropertyFloat("破線の間隔", &marking->dashGapMeters, 0.0f, 50.0f,
                    defaults.dashGapMeters, "破線と破線の間の空き。0 で実線", "%.1f m");
            }
            changed |= ui::PropertyBool("停止線", &marking->stopLines, defaults.stopLines,
                "Path の点に付けた停止線を、その向きの車線の幅いっぱいに引く");
            if (marking->stopLines) {
                changed |= ui::PropertyFloat("停止線の幅", &marking->stopLineWidthMeters, 0.1f, 2.0f,
                    defaults.stopLineWidthMeters, "停止線の道路の長さ方向の幅", "%.2f m");
            }
            changed |= ui::PropertyFloat("線幅", &marking->lineWidthMeters, 0.05f, 1.0f,
                defaults.lineWidthMeters, "帯の幅", "%.2f m");
            changed |= ui::PropertyFloat("端からの距離", &marking->edgeInsetMeters, 0.0f, 5.0f,
                defaults.edgeInsetMeters, "道路端から外側線の中心までの距離", "%.2f m");
            changed |= ui::PropertyFloat("浮かせ量", &marking->liftMeters, 0.0f, 0.1f,
                defaults.liftMeters, "路面から法線方向へ持ち上げる量。0だと路面とちらつく", "%.3f m");
            changed |= ui::PropertyFloat("UV反復長", &marking->uvRepeatMeters, 0.1f, 100.0f,
                defaults.uvRepeatMeters, "帯の長さ方向でUVが1増える実距離。幅方向は0〜1", "%.2f m");
            {
                static const char* const kUvAxisLabels[] = {"長さ方向 = V（縦）", "長さ方向 = U（横）"};
                int axis = marking->uvAlongU ? 1 : 0;
                if (ui::PropertyCombo("UVの向き", &axis, kUvAxisLabels, IM_ARRAYSIZE(kUvAxisLabels), 0,
                                      "テクスチャのどの軸を帯の長さ方向に沿わせるか。2048×256 のような横長の白線素材は U")) {
                    marking->uvAlongU = (axis == 1);
                    changed = true;
                }
            }
            changed |= ui::PropertyBool("進行方向の矢印", &marking->arrows, defaults.arrows,
                "各車線の中央に矢印を置く。進行方向の車線は線形の向き、対向車線は逆向き");
            if (marking->arrows) {
                changed |= ui::PropertyFloat("矢印の間隔", &marking->arrowIntervalMeters, 1.0f, 200.0f,
                    defaults.arrowIntervalMeters, "矢印を置く間隔", "%.0f m");
                changed |= ui::PropertyFloat("矢印の長さ", &marking->arrowLengthMeters, 0.5f, 20.0f,
                    defaults.arrowLengthMeters, "矢印の全長。幅は道路幅から決める", "%.1f m");
            }
            ui::PropertyValue("走行側", "%s", m_graph.RoadNetwork().leftHandTraffic ? "左側通行" : "右側通行");
            ui::EndPropertyTable();
        }
        ui::HintText("RoadのRoadSurfaceを接続し、出力のRoadSurfaceをMesh Outputへ。Materialに塗料の材質を接続できる。未接続は白。走行側はプレビュー設定の「道路」で切り替える。");
        if (changed) { m_graph.MarkDirty(); MarkDocumentChanged(); }
    } else if (selected->kind == graph::NodeKind::MeshOutput) {
        ui::HintText("RoadSurfaceを接続すると道路を表示する。複数のMesh Outputを同時に表示できる。");
    } else if (auto* settings = std::get_if<graph::LayerNodeSettings>(&selected->settings)) {
        bool changed = false;
        if (ui::BeginPropertyTable("graphNodeBasicRows")) {
            changed |= ui::PropertyBool("有効", &settings->layer.enabled, true,
                                        "無効にすると合成から外れる");
            ui::EndPropertyTable();
        }
        // 「下地」入力が繋がっていないノードは一番下のレイヤー扱い。
        // ソース（ハイトマップ）はそもそも入力を持たないので常にこちら。
        const bool isBase =
            selected->inputs.empty() ||
            m_graph.FindUpstreamNodeForPin(selected->inputs.front().id) == nullptr;
        const bool isSource = graph::IsSourceNodeKind(selected->kind);
        // Mask 入力にノードが繋がっていれば、マスクの出どころはそちら。
        bool maskFromNode = false;
        for (const graph::Pin& pin : selected->inputs) {
            if (pin.valueType == graph::ValueType::Mask &&
                m_graph.FindUpstreamNodeForPin(pin.id) != nullptr) {
                maskFromNode = true;
            }
        }
        changed |= DrawLayerSettings(settings->layer, isBase, isSource, maskFromNode,
                                     m_graph.MaskSourceResolves(*selected));

        // 地形の実寸。**ソースだけが持ち、読み込むときに一度だけ決める。**
        // プレビュー設定ではなくここに置くのは、実寸が見え方の設定ではなく
        // 読み込んだデータそのものの性質だから。
        if (isSource) {
            ui::SectionHeader("スケール");
            if (ui::BeginPropertyTable("graphNodeScaleRows")) {
                const graph::TerrainScale defaults;
                changed |= ui::PropertyFloat(
                    "サイズ", &settings->scale.sizeMeters, 0.5f, 8192.0f, defaults.sizeMeters,
                    "地形の一辺の長さ（m）。カメラと影の範囲もこれに追従する", "%.1f m",
                    ImGuiSliderFlags_Logarithmic);
                changed |= ui::PropertyFloat(
                    "標高差", &settings->scale.heightMeters, 0.0f,
                    std::max(1.0f, settings->scale.sizeMeters * 0.5f), defaults.heightMeters,
                    "ハイト 0〜1 の全幅が何 m になるか（最低地点から最高地点までの差）",
                    "%.1f m", ImGuiSliderFlags_Logarithmic);
                ui::EndPropertyTable();
            }
            ui::HintText("読み込んだ地形の実寸。プレビュー設定の平面のサイズと変位量はこれに従う");
        }
        if (changed) {
            m_graph.MarkDirty();
            MarkDocumentChanged();
        }
    } else if (auto* mask = std::get_if<graph::MaskNodeSettings>(&selected->settings)) {
        bool changed = false;
        const char* header = "マスク画像";
        const char* hint =
            "レイヤーの Mask 入力へ繋ぐと、白い所にだけそのレイヤーが乗る。"
            "効き方（係数 / カーブ / レベル）はレイヤー側で決める";
        switch (selected->kind) {
            case graph::NodeKind::MaskNoise:
                header = "ノイズ";
                hint = "下地に依らないノイズをマスクにする。"
                       "周波数は整数へ丸めて使うので、出力は必ずタイルする";
                break;
            case graph::NodeKind::MaskFluvial:
                header = "川筋";
                hint = "下地の高さから水の集まる所（川筋）を作る。"
                       "Base にどこまでのハイトを使うかを繋ぐ";
                break;
            case graph::NodeKind::MaskHeight:
                header = "標高";
                hint = "下地の標高帯（m）をマスクにする。"
                       "0 m は地形の一番低い所で、標高差 m が一番高い所。"
                       "Base にどこまでのハイトを使うかを繋ぐ";
                break;
            case graph::NodeKind::MaskSlope:
                header = "傾斜";
                hint = "下地の傾斜（角度）をマスクにする。"
                       "Base にどこまでのハイトを使うかを繋ぐ";
                break;
            case graph::NodeKind::MaskCurvature:
                header = "曲率";
                hint = "下地の凹凸をマスクにする。周りの平均と比べて、"
                       "高い所（尾根）か低い所（谷）を拾う。"
                       "Base にどこまでのハイトを使うかを繋ぐ";
                break;
            case graph::NodeKind::MaskLevels:
                header = "レベル";
                hint = "入力のマスクの黒点 / 白点 / ガンマを整える";
                break;
            case graph::NodeKind::MaskBlur:
                header = "ぼかし";
                hint = "入力のマスクをぼかす。境界のギザギザや、"
                       "しきい値で二値になったマスクを馴染ませるのに使う";
                break;
            case graph::NodeKind::MaskBlend:
                header = "合成";
                hint = "マスク 2 枚を合成する。片方だけ繋いだときはそれを通す";
                break;
            case graph::NodeKind::MaskPath:
                header = "パスの足跡";
                hint = "Path 入力の線を、点ごとの幅とフェザーでマスクにする。"
                       "形はパスの点が持ち、ここでは調整だけ";
                break;
            case graph::NodeKind::MaskArea:
                header = "パスの面";
                hint = "Path 入力の閉じた鎖を多角形とみなし、内側を 1 にする。"
                       "輪の中に輪を描けば穴になる。開いた鎖と点ごとの幅は読まない";
                break;
            default:
                break;
        }
        ui::SectionHeader(header);
        if (ui::BeginPropertyTable("graphMaskRows")) {
            switch (selected->kind) {
                case graph::NodeKind::MaskNoise:
                    changed |= DrawNoiseRows(mask->noise, graph::MaskNodeSettings().noise);
                    break;
                case graph::NodeKind::MaskFluvial:
                    changed |= DrawFluvialRows(mask->fluvial);
                    break;
                case graph::NodeKind::MaskHeight:
                    changed |= DrawHeightMaskRows(mask->height);
                    break;
                case graph::NodeKind::MaskSlope:
                    changed |= DrawSlopeRows(mask->slope);
                    break;
                case graph::NodeKind::MaskCurvature:
                    changed |= DrawCurvatureRows(mask->curvature);
                    break;
                case graph::NodeKind::MaskLevels:
                    changed |= DrawLevelsRows(mask->levels);
                    break;
                case graph::NodeKind::MaskBlur:
                    changed |= DrawMaskBlurRows(mask->blur);
                    break;
                case graph::NodeKind::MaskBlend:
                    changed |= DrawBlendRows(mask->blend);
                    break;
                case graph::NodeKind::MaskPath:
                    changed |= DrawPathMaskRows(mask->pathMask);
                    break;
                case graph::NodeKind::MaskArea:
                    changed |= DrawAreaMaskRows(mask->areaMask);
                    break;
                default:
                    changed |= DrawMapSlotRow("画像", mask->map, m_textureLibrary);
                    break;
            }
            ui::EndPropertyTable();
        }
        ui::HintText("%s", hint);
        // Mask Area は閉じた鎖しか読まない。無いと黙って空のマスクになるので注意書きを出す。
        if (selected->kind == graph::NodeKind::MaskArea) {
            const graph::Node* pathNode = m_graph.FindUpstreamNodeForPin(selected->inputs.front().id);
            const auto* pathSettings =
                (pathNode != nullptr) ? std::get_if<graph::PathNodeSettings>(&pathNode->settings)
                                      : nullptr;
            bool hasClosed = false;
            if (pathSettings != nullptr) {
                for (const graph::PathStrand& strand : graph::BuildPathStrands(pathSettings->path)) {
                    hasClosed |= strand.closed;
                }
            }
            if (pathSettings == nullptr) {
                ui::HintText("Path 入力が繋がっていないので、マスクは空になる");
            } else if (!hasClosed) {
                ui::HintText("閉じた鎖が無いので、マスクは空になる。端の点を始点へ重ねるか、"
                             "鎖を右クリック → 閉じる");
            }
        }
        if (changed) {
            m_graph.MarkDirty();
            MarkDocumentChanged();
        }
    } else if (std::get_if<graph::PathNodeSettings>(&selected->settings) != nullptr) {
        if (DrawPathSettings(*selected)) {
            m_graph.MarkDirty();
            MarkDocumentChanged();
        }
    } else {
        ui::HintText("出力ノード。「マテリアル」へ繋いだチェーンがプレビューになる");
    }
    ImGui::EndChild();

    ImGui::End();
}

}  // namespace tg
