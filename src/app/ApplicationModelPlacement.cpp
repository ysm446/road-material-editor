// Model ノード（グラフの 3D モデル）。ノードの追加と Mesh Output への接続、描画、ビューポートでの選択・ドラッグ移動、
// プロパティ。描画はレンダラの drawSceneExtras から呼ばれ、道路と同じシャドウマップ・照明で描く。
// 置き方（位置・回転・倍率）の変更は道路のメッシュに関係しないので、グラフの改版（道路の再生成）を起こさない。
// 仕様は docs/reference/model-assets.md の「Model ノード」。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "core/Log.h"
#include "graph/Road.h"
#include "ui/UiStyle.h"

#include <imgui.h>

#include <DirectXCollision.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cwctype>
#include <unordered_map>

namespace tg {
namespace fs = std::filesystem;
using namespace DirectX;

namespace {

std::wstring LowerExtension(const fs::path& path) {
    std::wstring ext = path.extension().wstring();
    for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext;
}

}  // namespace

graph::GraphId Application::PlaceModel(uint64_t modelId, const XMFLOAT3& position) {
    if (FindModel(modelId) == nullptr) return 0;
    const graph::GraphId nodeId = m_graph.CreateNode(graph::NodeKind::Model);
    graph::Node* node = m_graph.FindMutableNode(nodeId);
    if (node == nullptr) return 0;
    auto& settings = std::get<graph::ModelNodeSettings>(node->settings);
    settings.model = modelId;
    settings.position[0] = position.x;
    settings.position[1] = position.y;
    settings.position[2] = position.z;
    m_graphNodesToPlace.push_back(nodeId);

    // Mesh Output へ繋ぐ。無ければ作る。既に別のものが繋がっていれば Merge でまとめる。
    graph::GraphId outputId = 0;
    for (const graph::Node& candidate : m_graph.Nodes())
        if (candidate.kind == graph::NodeKind::MeshOutput) { outputId = candidate.id; break; }
    if (outputId == 0) {
        outputId = m_graph.CreateNode(graph::NodeKind::MeshOutput);
        m_graphNodesToPlace.push_back(outputId);
    }
    // Mesh Output の位置が未定（作った直後）なら、既存のノードの右に置く。重なって見えなくならないように。
    if (graph::Node* created = m_graph.FindMutableNode(outputId); created != nullptr && !created->positionValid) {
        float maxX = 0.0f, minY = 0.0f;
        bool any = false;
        for (const graph::Node& other : m_graph.Nodes()) {
            if (!other.positionValid || other.id == outputId || other.id == nodeId) continue;
            maxX = any ? std::max(maxX, other.posX) : other.posX;
            minY = any ? std::min(minY, other.posY) : other.posY;
            any = true;
        }
        created->posX = maxX + 640.0f;
        created->posY = minY;
        created->positionValid = true;
    }
    const graph::Node* output = m_graph.FindNode(outputId);
    const graph::GraphId outputInput = output->inputs.front().id;
    const graph::GraphId modelOutput = m_graph.FindNode(nodeId)->outputs.front().id;
    // エディタ上の置き場所。Mesh Output の左（Merge を足したら左下）へずらす。
    const bool outputPlaced = output->positionValid;
    const float baseX = output->posX - 320.0f;
    const float baseY = output->posY;
    const auto placeNear = [&](graph::GraphId id, float dx, float dy) {
        graph::Node* n = m_graph.FindMutableNode(id);
        if (n == nullptr || !outputPlaced) return;
        n->posX = baseX + dx;
        n->posY = baseY + dy;
        n->positionValid = true;
    };
    const graph::Node* upstream = m_graph.FindUpstreamNodeForPin(outputInput);
    if (upstream == nullptr) {
        m_graph.CreateLink(modelOutput, outputInput);
        placeNear(nodeId, 0.0f, 0.0f);
    } else if (upstream->kind == graph::NodeKind::Merge) {
        const graph::GraphId mergeId = upstream->id;
        m_graph.NormalizeVariablePins();
        const graph::Node* merge = m_graph.FindNode(mergeId);
        for (const graph::Pin& pin : merge->inputs) {
            if (m_graph.FindUpstreamNodeForPin(pin.id) != nullptr) continue;
            m_graph.CreateLink(modelOutput, pin.id);
            break;
        }
        m_graph.NormalizeVariablePins();
        placeNear(nodeId, -320.0f, 140.0f * static_cast<float>(m_graph.FindNode(mergeId)->inputs.size()));
    } else {
        // 今繋がっている出力ピンを Merge の Mesh 1 へ、モデルを Mesh 2 へ。Merge を Mesh Output へ。
        graph::GraphId previousPin = 0;
        for (const graph::Link& link : m_graph.Links())
            if (link.endPin == outputInput) previousPin = link.startPin;
        const graph::GraphId mergeId = m_graph.CreateNode(graph::NodeKind::Merge);
        m_graphNodesToPlace.push_back(mergeId);
        m_graph.CreateLink(previousPin, m_graph.FindNode(mergeId)->inputs.front().id);
        m_graph.NormalizeVariablePins();
        m_graph.CreateLink(modelOutput, m_graph.FindNode(mergeId)->inputs.back().id);
        m_graph.NormalizeVariablePins();
        m_graph.CreateLink(m_graph.FindNode(mergeId)->outputs.front().id, outputInput);
        placeNear(mergeId, 0.0f, 0.0f);
        placeNear(nodeId, -320.0f, 180.0f);
    }
    m_graph.MarkDirty();
    m_selectedGraphNode = nodeId;
    m_graphSelectionRequest = nodeId;
    m_meshHighlight.selected.clear();
    MarkDocumentChanged();
    TG_LOG_INFO("Model ノードを追加しました: %s", FindModel(modelId)->name.c_str());
    return nodeId;
}

std::vector<graph::GraphId> Application::VisibleModelNodes() const {
    const graph::Node* preview = m_graph.FindNode(m_previewGraphNode);
    return graph::CollectOutputModelNodes(
        m_graph, preview != nullptr && graph::IsMeshNodeKind(preview->kind) ? preview->id : 0);
}

bool Application::ModelNodeTransform(graph::GraphId nodeId, const renderer::ModelAsset*& model,
                                     renderer::ModelInstance& instance) const {
    const graph::Node* node = m_graph.FindNode(nodeId);
    const auto* settings = node ? std::get_if<graph::ModelNodeSettings>(&node->settings) : nullptr;
    if (settings == nullptr || settings->model == 0) return false;
    const auto found = std::find_if(m_models.begin(), m_models.end(),
                                    [&](const renderer::ModelAsset& m) { return m.id == settings->model; });
    if (found == m_models.end() || !found->geometry) return false;
    model = &*found;
    instance.id = nodeId;
    instance.model = settings->model;
    instance.position = {settings->position[0], settings->position[1], settings->position[2]};
    instance.rotationDegrees = settings->rotationDegrees;
    instance.scale = settings->scale;
    return true;
}

graph::ModelNodeSettings* Application::SelectedModelNode() {
    graph::Node* node = m_graph.FindMutableNode(m_selectedGraphNode);
    return node ? std::get_if<graph::ModelNodeSettings>(&node->settings) : nullptr;
}

float Application::ModelInstancesRadius() const {
    float radius = 0.0f;
    for (const graph::GraphId nodeId : VisibleModelNodes()) {
        const renderer::ModelAsset* model = nullptr;
        renderer::ModelInstance instance;
        BoundingBox bounds;
        if (!ModelNodeTransform(nodeId, model, instance) || !renderer::ModelInstanceBounds(*model, instance, bounds)) continue;
        const float extent = XMVectorGetX(XMVector3Length(XMLoadFloat3(&bounds.Extents)));
        radius = std::max(radius, XMVectorGetX(XMVector3Length(XMLoadFloat3(&bounds.Center))) + extent);
    }
    return radius;
}

void Application::DrawSceneModels(ID3D12GraphicsCommandList* commandList, const renderer::SceneDrawContext& context) {
    // 同じモデルを置いたノードはまとめて描く（メッシュとパイプラインの切り替えを減らす）。
    std::unordered_map<uint64_t, std::vector<XMFLOAT4X4>> worlds;
    for (const graph::GraphId nodeId : VisibleModelNodes()) {
        const renderer::ModelAsset* model = nullptr;
        renderer::ModelInstance instance;
        if (!ModelNodeTransform(nodeId, model, instance)) continue;
        XMStoreFloat4x4(&worlds[model->id].emplace_back(), renderer::ModelInstanceWorld(*model, instance));
    }
    for (const auto& [modelId, list] : worlds) {
        const auto preview = m_modelPreviews.find(modelId);
        const renderer::ModelAsset* model = FindModel(modelId);
        if (preview == m_modelPreviews.end() || model == nullptr) continue;
        preview->second->RenderInScene(m_device, m_pipelineCache, commandList, *model, m_materialLibrary,
                                       m_textureLibrary, context, list);
    }
}

graph::GraphId Application::PickModelNode(const XMFLOAT3& origin, const XMFLOAT3& direction, float& distance) const {
    graph::GraphId hit = 0;
    distance = FLT_MAX;
    const XMVECTOR rayOrigin = XMLoadFloat3(&origin);
    const XMVECTOR rayDirection = XMLoadFloat3(&direction);
    for (const graph::GraphId nodeId : VisibleModelNodes()) {
        const renderer::ModelAsset* model = nullptr;
        renderer::ModelInstance instance;
        BoundingBox bounds;
        float boxDistance = 0.0f;
        if (!ModelNodeTransform(nodeId, model, instance) || !renderer::ModelInstanceBounds(*model, instance, bounds) ||
            !bounds.Intersects(rayOrigin, rayDirection, boxDistance) || boxDistance >= distance) continue;
        // 三角形はモデルの座標で調べ、当たった点をワールドへ戻して距離を比べる。
        const XMMATRIX world = renderer::ModelInstanceWorld(*model, instance);
        XMVECTOR determinant;
        const XMMATRIX inverse = XMMatrixInverse(&determinant, world);
        if (XMVectorGetX(determinant) == 0.0f) continue;
        const XMVECTOR localOrigin = XMVector3TransformCoord(rayOrigin, inverse);
        const XMVECTOR localDirection = XMVector3Normalize(XMVector3TransformNormal(rayDirection, inverse));
        for (const auto& part : model->geometry->lods[0].parts) {
            const auto& vertices = part.mesh.vertices;
            const auto& indices = part.mesh.indices;
            for (size_t t = 0; t + 2 < indices.size(); t += 3) {
                float localDistance = 0.0f;
                if (!TriangleTests::Intersects(localOrigin, localDirection, XMLoadFloat3(&vertices[indices[t]].position),
                                               XMLoadFloat3(&vertices[indices[t + 1]].position),
                                               XMLoadFloat3(&vertices[indices[t + 2]].position), localDistance)) continue;
                const XMVECTOR point = XMVector3TransformCoord(
                    XMVectorAdd(localOrigin, XMVectorScale(localDirection, localDistance)), world);
                const float worldDistance = XMVectorGetX(XMVector3Length(XMVectorSubtract(point, rayOrigin)));
                if (worldDistance < distance) {
                    distance = worldDistance;
                    hit = nodeId;
                }
            }
        }
    }
    return hit;
}

bool Application::PickGround(const ImVec2& mouse, const ImVec2& viewportMin, const ImVec2& viewportMax, float planeY,
                             bool useMeshes, XMFLOAT3& point) const {
    XMFLOAT3 origin, direction;
    if (!ViewportRay(mouse, viewportMin, viewportMax, origin, direction)) return false;
    const XMVECTOR rayOrigin = XMLoadFloat3(&origin);
    const XMVECTOR rayDirection = XMLoadFloat3(&direction);
    float nearest = FLT_MAX;
    if (useMeshes && m_renderer.HasMeshScene()) {
        // 道路・路肩などの面の上へ置く（変位前の形。白線などの帯も面として扱う）。
        for (const auto& mesh : m_renderer.Scene().meshes) {
            if (mesh.materialOnly) continue;
            const auto& vertices = mesh.geometry.vertices;
            const auto& indices = mesh.geometry.indices;
            for (size_t t = 0; t + 2 < indices.size(); t += 3) {
                float distance = 0.0f;
                if (TriangleTests::Intersects(rayOrigin, rayDirection, XMLoadFloat3(&vertices[indices[t]].position),
                                              XMLoadFloat3(&vertices[indices[t + 1]].position),
                                              XMLoadFloat3(&vertices[indices[t + 2]].position), distance) &&
                    distance < nearest) nearest = distance;
            }
        }
    }
    if (nearest == FLT_MAX) {
        // 水平面 y = planeY。上から見下ろす向きでなければ当たらない（遠すぎる交点も捨てる）。
        if (std::abs(direction.y) < 1e-5f) return false;
        const float t = (planeY - origin.y) / direction.y;
        if (t <= 0.0f || t > 10000.0f) return false;
        nearest = t;
    }
    XMStoreFloat3(&point, XMVectorAdd(rayOrigin, XMVectorScale(rayDirection, nearest)));
    return true;
}

bool Application::HandleModelInstanceInput(bool itemActive, bool itemHovered, const ImVec2& viewportMin,
                                           const ImVec2& viewportMax) {
    (void)itemActive;
    auto& drag = m_modelInstanceDrag;
    const ImGuiIO& io = ImGui::GetIO();
    m_hoveredModelNode = 0;
    graph::ModelNodeSettings* selected = SelectedModelNode();

    // --- 掴んでいる間: 動かしたら水平面に沿って移動する ------------------------
    if (drag.pending) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || selected == nullptr) {
            drag = {};
            return true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            // 掴む前の位置へ戻す。
            if (drag.dragging) {
                selected->position[0] = drag.startPosition.x;
                selected->position[2] = drag.startPosition.z;
                m_documentDirty = true;
            }
            drag = {};
            return true;
        }
        drag.dragging |= ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left, ui::Scaled(3.0f));
        XMFLOAT3 point;
        if (drag.dragging && PickGround(io.MousePos, viewportMin, viewportMax, drag.planeY, false, point)) {
            const float x = point.x + drag.offset.x, z = point.z + drag.offset.z;
            if (x != selected->position[0] || z != selected->position[2]) {
                selected->position[0] = x;
                selected->position[2] = z;
                m_documentDirty = true;
            }
        }
        m_hoveredModelNode = m_selectedGraphNode;
        return true;
    }

    if (!itemHovered) return false;
    XMFLOAT3 origin, direction;
    float distance = 0.0f;
    if (ViewportRay(io.MousePos, viewportMin, viewportMax, origin, direction)) {
        m_hoveredModelNode = PickModelNode(origin, direction, distance);
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (m_hoveredModelNode == 0) {
            // モデルを選んでいたら外す（ほかのノードの選択はそのまま）。空やメッシュのクリックはメッシュの選択へ渡す。
            if (selected != nullptr) {
                m_selectedGraphNode = 0;
                m_graphSelectionRequest = 0;
            }
            return false;
        }
        m_selectedGraphNode = m_hoveredModelNode;
        m_graphSelectionRequest = m_hoveredModelNode;
        m_meshHighlight.selected.clear();
        selected = SelectedModelNode();
        drag = {};
        drag.pending = true;
        drag.pressPos = io.MousePos;
        drag.planeY = selected->position[1];
        drag.startPosition = {selected->position[0], selected->position[1], selected->position[2]};
        XMFLOAT3 point;
        if (PickGround(io.MousePos, viewportMin, viewportMax, drag.planeY, false, point)) {
            drag.offset = {selected->position[0] - point.x, 0.0f, selected->position[2] - point.z};
        }
        return true;
    }
    if (selected != nullptr && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            // Model ノードごと消す（グラフのノードの削除と同じ。アンドゥで戻る）。
            m_graph.DeleteNode(m_selectedGraphNode);
            m_graph.NormalizeVariablePins();
            m_graph.MarkDirty();
            m_selectedGraphNode = 0;
            m_graphSelectionRequest = 0;
            MarkDocumentChanged();
            return true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            m_selectedGraphNode = 0;
            m_graphSelectionRequest = 0;
        }
    }
    return m_hoveredModelNode != 0;
}

void Application::DrawModelInstanceOverlay(const ImVec2& viewportMin, const ImVec2& viewportMax) {
    const graph::GraphId selectedNode = SelectedModelNode() ? m_selectedGraphNode : 0;
    if (selectedNode == 0 && m_hoveredModelNode == 0) return;
    const auto& camera = m_renderer.GetCamera();
    const XMMATRIX viewProjection = camera.ViewMatrix() * camera.ProjectionMatrix();
    const ImVec2 size(viewportMax.x - viewportMin.x, viewportMax.y - viewportMin.y);
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(viewportMin, viewportMax, true);
    const auto visible = VisibleModelNodes();
    const auto box = [&](graph::GraphId id, ImU32 color, float thickness) {
        const renderer::ModelAsset* model = nullptr;
        renderer::ModelInstance instance;
        if (std::find(visible.begin(), visible.end(), id) == visible.end() || !ModelNodeTransform(id, model, instance)) return;
        // モデルの座標の箱をワールドへ回して描く（回転しても形に沿う）。
        const XMMATRIX world = renderer::ModelInstanceWorld(*model, instance);
        const auto& lo = model->geometry->minimum;
        const auto& hi = model->geometry->maximum;
        ProjectedPoint corners[8];
        for (int i = 0; i < 8; ++i) {
            XMFLOAT3 corner{(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z};
            XMStoreFloat3(&corner, XMVector3TransformCoord(XMLoadFloat3(&corner), world));
            corners[i] = ProjectToViewport(viewProjection, corner, viewportMin, size);
        }
        static constexpr int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                              {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto& edge : kEdges) {
            const auto& a = corners[edge[0]];
            const auto& b = corners[edge[1]];
            if (a.visible && b.visible) draw->AddLine(a.screen, b.screen, color, thickness);
        }
    };
    if (m_hoveredModelNode != 0 && m_hoveredModelNode != selectedNode)
        box(m_hoveredModelNode, ImGui::GetColorU32(ImGuiCol_PlotLines), ui::Scaled(1.0f));
    if (selectedNode != 0) box(selectedNode, ImGui::GetColorU32(ImGuiCol_PlotLinesHovered), ui::Scaled(2.0f));
    draw->PopClipRect();
}

void Application::ModelDropTarget(const ImVec2& viewportMin, const ImVec2& viewportMax) {
    if (!ImGui::BeginDragDropTarget()) return;
    const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPathDragDropType);
    if (payload != nullptr && payload->DataSize >= int(sizeof(wchar_t))) {
        const std::wstring text(static_cast<const wchar_t*>(payload->Data));
        XMFLOAT3 point;
        if (PickGround(ImGui::GetIO().MousePos, viewportMin, viewportMax, 0.0f, true, point)) {
            // 複数を落としたら 1 つずつ X へずらして並べる。
            float offset = 0.0f;
            for (size_t begin = 0; begin < text.size();) {
                const size_t end = std::min(text.find(L'\n', begin), text.size());
                const fs::path path = text.substr(begin, end - begin);
                const auto ext = LowerExtension(path);
                if (ext == L".tgmodel" || ext == L".fbx") {
                    m_pendingModelPlacements.push_back({path, {point.x + offset, point.y, point.z}});
                    offset += 2.0f;
                }
                begin = end + 1;
            }
        }
    }
    ImGui::EndDragDropTarget();
}

bool Application::DrawModelNodeSettings(graph::Node& node) {
    auto* settings = std::get_if<graph::ModelNodeSettings>(&node.settings);
    if (settings == nullptr) return false;
    renderer::ModelAsset* model = FindModel(settings->model);
    bool changed = false;
    ui::SectionHeader("モデル");
    if (ui::BeginPropertyTable("modelNode")) {
        ui::PropertyLabel("モデル", "シーンに読み込んだモデル。アセットの帯で .tgmodel / .fbx をダブルクリックすると候補に加わる");
        ImGui::SetNextItemWidth(std::min(ui::Scaled(ui::kComboMaxWidth), ImGui::GetContentRegionAvail().x));
        if (ImGui::BeginCombo("##model", model ? model->name.c_str() : "なし")) {
            if (ImGui::Selectable("なし", settings->model == 0)) {
                settings->model = 0;
                changed = true;
            }
            for (const renderer::ModelAsset& candidate : m_models) {
                ImGui::PushID(static_cast<int>(candidate.id));
                if (ImGui::Selectable(candidate.name.c_str(), settings->model == candidate.id)) {
                    settings->model = candidate.id;
                    changed = true;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ui::PropertyEnd();
        model = FindModel(settings->model);
        const graph::ModelNodeSettings defaults;
        if (ui::PropertyFloat3Input("位置 (m)", settings->position, defaults.position,
                                    "モデルの底面の中心を置く位置。ビューポートでドラッグすると水平に動く") != 0) {
            changed = true;
        }
        changed |= ui::PropertyFloat("回転 Y", &settings->rotationDegrees, -180.0f, 180.0f, defaults.rotationDegrees,
                                     "上から見て反時計回り（度）", "%.1f");
        changed |= ui::PropertyFloat("倍率", &settings->scale, 0.01f, 100.0f, defaults.scale,
                                     "このノードで掛ける倍率。モデルアセットの倍率に掛かる", "%.3f",
                                     ImGuiSliderFlags_Logarithmic);
        settings->scale = std::clamp(settings->scale, 0.01f, 100.0f);
        if (model != nullptr && model->geometry) {
            const float scale = model->scale * settings->scale;
            const auto& g = *model->geometry;
            ui::PropertyValue("寸法 X / Y / Z", "%.2f / %.2f / %.2f m", (g.maximum.x - g.minimum.x) * scale,
                              (g.maximum.y - g.minimum.y) * scale, (g.maximum.z - g.minimum.z) * scale);
        }
        ui::EndPropertyTable();
    }
    if (model != nullptr && ui::Button("モデルを開く", ui::kWideButtonWidth)) {
        m_selectedModel = model->id;
        m_showModelPreview = true;
    }
    const auto visible = VisibleModelNodes();
    if (std::find(visible.begin(), visible.end(), node.id) == visible.end())
        ui::HintText("Mesh Output（または Merge を通して）へ繋ぐとビューポートに出る");
    ui::HintText("ビューポートでクリックして選び、ドラッグで水平に移動、Delete でノードごと削除");
    return changed;
}

bool Application::SelectedModelInstanceFocusTarget(XMFLOAT3& target) {
    const renderer::ModelAsset* model = nullptr;
    renderer::ModelInstance instance;
    BoundingBox bounds;
    if (!SelectedModelNode() || !ModelNodeTransform(m_selectedGraphNode, model, instance) ||
        !renderer::ModelInstanceBounds(*model, instance, bounds)) return false;
    target = bounds.Center;
    return true;
}

}  // namespace tg
