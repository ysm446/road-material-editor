#include "graph/NodeGraph.h"

#include "compositor/MaterialStack.h"

#include <algorithm>
#include <array>
#include <unordered_set>
#include <utility>

namespace tg::graph {
namespace {

// --- 定義テーブル ---------------------------------------------------------
// ノードの種類・保存名・表示名・ピン構成。CreateNode() がここからピンを作る。

// **ノードの名前とピンのラベルは英語で書く。**
// ノードグラフを持つツール（Substance / Houdini / Gaea など）はどれも英語表記で、
// 素材やノードの呼び名もその語彙で流通している。説明文だけ日本語にする。
// 合成レイヤーのピン。**Mask 入力は「どこに乗せるか」**を外から与えるもので、
// 繋がっていなければノード側のマスク設定がそのまま効く。
constexpr std::array<PinDefinition, 3> kLayerNodePins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Mask"},
    {PinKind::Output, ValueType::Material, "Result"},
}};

// パスのピン。Surface に Road の RoadSurface を繋ぐと、そのパスは面の座標（横位置 × 実距離）で
// 保持され、道路を変形しても面に貼り付いたまま追従する（デカールの経路）。繋がなければ実寸 XYZ。
// 旧地形パスの Base（どの地形に沿うか）はこのピンの前身で、旧ファイルのリンクは型が違うので捨てる。
constexpr std::array<PinDefinition, 2> kPathPins = {{
    {PinKind::Input, ValueType::Mesh, "Surface"},
    {PinKind::Output, ValueType::Path, "Path"},
}};
constexpr std::array<PinDefinition, 4> kDecalPins = {{
    {PinKind::Input, ValueType::Mesh, "RoadSurface"},
    {PinKind::Input, ValueType::Path, "Path"},
    {PinKind::Input, ValueType::Material, "Material"},
    {PinKind::Output, ValueType::Mesh, "RoadSurface"},
}};
// 路肩のピン。Path には Road の Left / Right か、別の路肩の Outer を繋ぐ。
// Outer は外側の境界（実寸 Path）で、次の路肩や縁石へ渡す。
// 材質スロットとマスクは Road と同じ並び。
constexpr std::array<PinDefinition, 10> kShoulderPins = {{
    {PinKind::Input, ValueType::Path, "Path"},
    {PinKind::Input, ValueType::Material, "Material"},
    {PinKind::Input, ValueType::Material, "Material 2"},
    {PinKind::Input, ValueType::Material, "Material 3"},
    {PinKind::Input, ValueType::Material, "Material 4"},
    {PinKind::Input, ValueType::RoadMask, "Mask 2"},
    {PinKind::Input, ValueType::RoadMask, "Mask 3"},
    {PinKind::Input, ValueType::RoadMask, "Mask 4"},
    {PinKind::Output, ValueType::Mesh, "RoadSurface"},
    {PinKind::Output, ValueType::Path, "Outer"},
}};
// ひび割れのピン。Decal と同じく RoadSurface を受けて RoadSurface を返す。Material は幹の材質。
constexpr std::array<PinDefinition, 3> kCrackPins = {{
    {PinKind::Input, ValueType::Mesh, "RoadSurface"},
    {PinKind::Input, ValueType::Material, "Material"},
    {PinKind::Output, ValueType::Mesh, "RoadSurface"},
}};
// Merge のピン。入力は可変で、繋ぐたびに空きが 1 本増える（NormalizeVariablePins）。
constexpr std::array<PinDefinition, 2> kMergePins = {{
    {PinKind::Input, ValueType::Mesh, "Mesh 1"},
    {PinKind::Output, ValueType::Mesh, "RoadSurface"},
}};

// 材質はスロット 1〜4。スロット 2〜4 は道路マスク（Mask 2〜4）で被覆する。
constexpr std::array<PinDefinition, 11> kRoadPins = {{
    {PinKind::Input, ValueType::Path, "Path"},
    {PinKind::Input, ValueType::Material, "Material"},
    {PinKind::Input, ValueType::Material, "Material 2"},
    {PinKind::Input, ValueType::Material, "Material 3"},
    {PinKind::Input, ValueType::Material, "Material 4"},
    {PinKind::Input, ValueType::RoadMask, "Mask 2"},
    {PinKind::Input, ValueType::RoadMask, "Mask 3"},
    {PinKind::Input, ValueType::RoadMask, "Mask 4"},
    {PinKind::Output, ValueType::Mesh, "RoadSurface"},
    {PinKind::Output, ValueType::Path, "Left"},
    {PinKind::Output, ValueType::Path, "Right"},
}};
constexpr std::array<PinDefinition, 1> kRoadMaskPins = {{
    {PinKind::Output, ValueType::RoadMask, "Mask"},
}};
constexpr std::array<PinDefinition, 1> kMeshOutputPins = {{
    {PinKind::Input, ValueType::Mesh, "Mesh"},
}};
constexpr std::array<PinDefinition, 3> kRoadMarkingPins = {{
    {PinKind::Input, ValueType::Mesh, "RoadSurface"},
    {PinKind::Input, ValueType::Material, "Material"},
    {PinKind::Output, ValueType::Mesh, "RoadSurface"},
}};

constexpr std::array<NodeDefinition, 10> kNodeDefinitions = {{
    {NodeKind::Road, "road", "Road", kRoadPins},
    {NodeKind::RoadMask, "roadMask", "Road Mask", kRoadMaskPins},
    {NodeKind::Decal, "decal", "Decal", kDecalPins},
    {NodeKind::Shoulder, "shoulder", "Shoulder", kShoulderPins},
    {NodeKind::Merge, "merge", "Merge", kMergePins},
    {NodeKind::Crack, "crack", "Crack", kCrackPins},
    {NodeKind::RoadMarking, "roadMarking", "Lane Marking", kRoadMarkingPins},
    {NodeKind::MeshOutput, "meshOutput", "Mesh Output", kMeshOutputPins},
    {NodeKind::Surface, "surface", "Surface", kLayerNodePins},
    {NodeKind::Path, "path", "Path", kPathPins},
}};

}  // namespace

std::span<const NodeDefinition> NodeDefinitions() {
    return kNodeDefinitions;
}

const NodeDefinition* FindNodeDefinition(NodeKind kind) {
    for (const NodeDefinition& definition : kNodeDefinitions) {
        if (definition.kind == kind) {
            return &definition;
        }
    }
    return nullptr;
}

const NodeDefinition* FindNodeDefinitionByName(std::string_view name) {
    for (const NodeDefinition& definition : kNodeDefinitions) {
        if (definition.name == name) {
            return &definition;
        }
    }
    return nullptr;
}

bool IsLayerNodeKind(NodeKind kind) {
    return kind == NodeKind::Surface;
}

bool IsMeshNodeKind(NodeKind kind) {
    return kind == NodeKind::Road || kind == NodeKind::RoadMarking || kind == NodeKind::Decal ||
           kind == NodeKind::Shoulder || kind == NodeKind::Merge || kind == NodeKind::Crack;
}

bool IsPreviewableNodeKind(NodeKind kind) {
    // 道路メッシュのノードは、そのノードまでの鎖をメッシュシーンに出す。
    return IsLayerNodeKind(kind) || kind == NodeKind::Path || IsMeshNodeKind(kind);
}

compositor::LayerKind LayerKindFor(NodeKind /*kind*/) {
    // レイヤー設定を持つのは Surface だけになった。
    return compositor::LayerKind::Surface;
}

// --- NodeGraph ------------------------------------------------------------

NodeGraph NodeGraph::CreateDefault() {
    NodeGraph graph;
    const GraphId baseId = graph.CreateNode(NodeKind::Surface);
    if (Node* base = graph.FindMutableNode(baseId)) {
        compositor::MaterialLayer layer = compositor::MaterialStack::MakeBaseLayer();
        base->settings = LayerNodeSettings{std::move(layer)};
        base->posX = 60.0f;
        base->posY = 120.0f;
        base->positionValid = true;
    }
    return graph;
}

const Pin* NodeGraph::FindPin(GraphId pinId) const {
    for (const Node& node : m_nodes) {
        for (const Pin& pin : node.inputs) {
            if (pin.id == pinId) {
                return &pin;
            }
        }
        for (const Pin& pin : node.outputs) {
            if (pin.id == pinId) {
                return &pin;
            }
        }
    }
    return nullptr;
}

const Node* NodeGraph::FindNode(GraphId nodeId) const {
    const auto it = std::find_if(m_nodes.begin(), m_nodes.end(),
                                 [nodeId](const Node& node) { return node.id == nodeId; });
    return it == m_nodes.end() ? nullptr : &*it;
}

Node* NodeGraph::FindMutableNode(GraphId nodeId) {
    const auto it = std::find_if(m_nodes.begin(), m_nodes.end(),
                                 [nodeId](const Node& node) { return node.id == nodeId; });
    return it == m_nodes.end() ? nullptr : &*it;
}

const Node* NodeGraph::FindUpstreamNodeForPin(GraphId inputPinId) const {
    for (const Link& link : m_links) {
        if (link.endPin != inputPinId) {
            continue;
        }
        if (const Pin* startPin = FindPin(link.startPin)) {
            return FindNode(startPin->nodeId);
        }
    }
    return nullptr;
}

// producer の出力から下流を辿り、target に届くか。循環チェックに使う。
bool NodeGraph::ReachesDownstream(GraphId fromNodeId, GraphId targetNodeId) const {
    std::unordered_set<GraphId> visited;
    std::vector<GraphId> stack{fromNodeId};
    while (!stack.empty()) {
        const GraphId current = stack.back();
        stack.pop_back();
        if (current == targetNodeId) {
            return true;
        }
        if (!visited.insert(current).second) {
            continue;
        }
        const Node* node = FindNode(current);
        if (node == nullptr) {
            continue;
        }
        for (const Pin& output : node->outputs) {
            for (const Link& link : m_links) {
                if (link.startPin != output.id) {
                    continue;
                }
                if (const Pin* endPin = FindPin(link.endPin)) {
                    stack.push_back(endPin->nodeId);
                }
            }
        }
    }
    return false;
}

bool NodeGraph::CanCreateLink(GraphId startPin, GraphId endPin) const {
    if (startPin == 0 || endPin == 0 || startPin == endPin) {
        return false;
    }
    const Pin* start = FindPin(startPin);
    const Pin* end = FindPin(endPin);
    if (start == nullptr || end == nullptr || start->nodeId == end->nodeId ||
        start->valueType != end->valueType) {
        return false;
    }
    if (start->kind == end->kind) {
        return false;
    }
    // 出力側 → 入力側へ揃えてから循環を見る。
    if (start->kind == PinKind::Input) {
        std::swap(start, end);
    }
    // end（消費側）の下流に start（生産側）がいたら、この接続で輪ができる。
    if (ReachesDownstream(end->nodeId, start->nodeId)) {
        return false;
    }
    return true;
}

bool NodeGraph::CreateLink(GraphId startPin, GraphId endPin) {
    if (!CanCreateLink(startPin, endPin)) {
        return false;
    }
    const Pin* start = FindPin(startPin);
    if (start != nullptr && start->kind == PinKind::Input) {
        std::swap(startPin, endPin);
    }
    // 入力ピンは 1 本だけ。既にある接続は置き換える。
    std::erase_if(m_links, [endPin](const Link& link) { return link.endPin == endPin; });
    m_links.push_back({AllocateGraphId(), startPin, endPin});
    NormalizeVariablePins();
    MarkDirty();
    return true;
}

bool NodeGraph::DeleteLink(GraphId linkId) {
    const size_t oldSize = m_links.size();
    std::erase_if(m_links, [linkId](const Link& link) { return link.id == linkId; });
    if (m_links.size() == oldSize) {
        return false;
    }
    NormalizeVariablePins();
    MarkDirty();
    return true;
}

void NodeGraph::NormalizeVariablePins() {
    for (Node& node : m_nodes) {
        if (node.kind != NodeKind::Merge) continue;
        // 繋がっている入力を順に残し、末尾に空きを 1 本だけ置く。ラベルは並びで振り直す。
        std::vector<Pin> connected;
        for (const Pin& pin : node.inputs) {
            const bool linked = std::any_of(m_links.begin(), m_links.end(),
                                            [&](const Link& link) { return link.endPin == pin.id; });
            if (linked) connected.push_back(pin);
        }
        Pin spare;
        spare.id = AllocateGraphId();
        spare.nodeId = node.id;
        spare.kind = PinKind::Input;
        spare.valueType = ValueType::Mesh;
        connected.push_back(std::move(spare));
        for (size_t i = 0; i < connected.size(); ++i) {
            connected[i].label = "Mesh " + std::to_string(i + 1);
        }
        node.inputs = std::move(connected);
    }
}

GraphId NodeGraph::CreateNode(NodeKind kind) {
    const NodeDefinition* definition = FindNodeDefinition(kind);
    if (definition == nullptr) {
        return 0;
    }
    Node node;
    node.id = AllocateGraphId();
    node.kind = kind;
    if (IsLayerNodeKind(kind)) {
        LayerNodeSettings settings;
        settings.layer.kind = LayerKindFor(kind);
        node.settings = std::move(settings);
    } else if (kind == NodeKind::Road) {
        node.settings = RoadNodeSettings{};
    } else if (kind == NodeKind::RoadMarking) {
        node.settings = RoadMarkingNodeSettings{};
    } else if (kind == NodeKind::RoadMask) {
        node.settings = RoadMaskNodeSettings{};
    } else if (kind == NodeKind::Decal) {
        node.settings = DecalNodeSettings{};
    } else if (kind == NodeKind::Shoulder) {
        node.settings = ShoulderNodeSettings{};
    } else if (kind == NodeKind::Merge) {
        node.settings = MergeNodeSettings{};
    } else if (kind == NodeKind::Crack) {
        node.settings = CrackNodeSettings{};
    } else if (kind == NodeKind::Path) {
        node.settings = PathNodeSettings{};
        std::get<PathNodeSettings>(node.settings).path.worldSpace = true;
    } else {
        // Mesh Output は設定を持たない。
        node.settings = std::monostate{};
    }
    for (const PinDefinition& pin : definition->pins) {
        Pin created;
        created.id = AllocateGraphId();
        created.nodeId = node.id;
        created.kind = pin.kind;
        created.valueType = pin.valueType;
        created.label = pin.label;
        if (pin.kind == PinKind::Input) {
            node.inputs.push_back(std::move(created));
        } else {
            node.outputs.push_back(std::move(created));
        }
    }
    const GraphId nodeId = node.id;
    m_nodes.push_back(std::move(node));
    MarkDirty();
    return nodeId;
}

bool NodeGraph::DeleteNode(GraphId nodeId) {
    const Node* node = FindNode(nodeId);
    if (node == nullptr) {
        return false;
    }
    std::vector<GraphId> pinIds;
    pinIds.reserve(node->inputs.size() + node->outputs.size());
    for (const Pin& pin : node->inputs) {
        pinIds.push_back(pin.id);
    }
    for (const Pin& pin : node->outputs) {
        pinIds.push_back(pin.id);
    }
    std::erase_if(m_links, [&pinIds](const Link& link) {
        return std::find(pinIds.begin(), pinIds.end(), link.startPin) != pinIds.end() ||
               std::find(pinIds.begin(), pinIds.end(), link.endPin) != pinIds.end();
    });
    std::erase_if(m_nodes, [nodeId](const Node& candidate) { return candidate.id == nodeId; });
    NormalizeVariablePins();
    MarkDirty();
    return true;
}

void NodeGraph::Replace(std::vector<Node> nodes, std::vector<Link> links) {
    m_nodes = std::move(nodes);
    m_links = std::move(links);
    // 壊れたリンク（ピンが無い・型が合わない）は捨てる。読み込みの安全網。
    std::erase_if(m_links, [this](const Link& link) {
        const Pin* start = FindPin(link.startPin);
        const Pin* end = FindPin(link.endPin);
        return start == nullptr || end == nullptr || start->kind != PinKind::Output ||
               end->kind != PinKind::Input || start->valueType != end->valueType;
    });
    RebuildNextGraphId();
    NormalizeVariablePins();
    MarkDirty();
}

void NodeGraph::RebuildNextGraphId() {
    GraphId maxId = 0;
    for (const Node& node : m_nodes) {
        maxId = std::max(maxId, node.id);
        for (const Pin& pin : node.inputs) {
            maxId = std::max(maxId, pin.id);
        }
        for (const Pin& pin : node.outputs) {
            maxId = std::max(maxId, pin.id);
        }
    }
    for (const Link& link : m_links) {
        maxId = std::max(maxId, link.id);
    }
    m_nextGraphId = maxId + 1;
}

// 「下地」チェーンを上から下へ辿る。輪は visited で止める。
std::vector<const Node*> NodeGraph::ChainFrom(const Node* top) const {
    std::vector<const Node*> chain;
    std::unordered_set<GraphId> visited;
    const Node* current = top;
    while (current != nullptr && IsLayerNodeKind(current->kind) &&
           visited.insert(current->id).second) {
        chain.push_back(current);
        current = current->inputs.empty() ? nullptr
                                          : FindUpstreamNodeForPin(current->inputs.front().id);
    }
    return chain;
}

// レイヤー列の元ノードを、コンパイル結果へ写す。列のほうが長ければ（プレビュー用の
// 塗りレイヤー）残りは 0。
void NodeGraph::RecordLayerSources(const std::vector<const Node*>& layerNodes,
                                   CompiledGraph& compiled) {
    compiled.layerSources.assign(compiled.layers.size(), 0);
    for (size_t i = 0; i < layerNodes.size() && i < compiled.layers.size(); ++i) {
        compiled.layerSources[i] = (layerNodes[i] != nullptr) ? layerNodes[i]->id : 0;
    }
}

CompiledGraph NodeGraph::CompileChainFrom(const Node* top) const {
    const std::vector<const Node*> chain = ChainFrom(top);

    // 遡った順（上→下）を、レイヤー列の順（下→上）へ反転する。
    CompiledGraph compiled;
    std::vector<const Node*> layerNodes;
    compiled.layers.reserve(chain.size());
    layerNodes.reserve(chain.size());
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (const auto* settings = std::get_if<LayerNodeSettings>(&(*it)->settings)) {
            compiled.layers.push_back(settings->layer);
            layerNodes.push_back(*it);
        }
    }

    // 有効な下地が 1 枚も無ければ、評価器はどの出力テクスチャにも書かず、
    // 直前の評価結果がそのまま見えてしまう。変位 0 の中立平面へ戻す。
    const bool hasEnabledBase =
        std::any_of(compiled.layers.begin(), compiled.layers.end(), [](const auto& layer) {
            return layer.enabled && !compositor::IsHeightOperationKind(layer.kind);
        });
    if (!hasEnabledBase) {
        compiled.layers.clear();
        compiled.layers.push_back(compositor::MaterialStack::MakeBaseLayer());
        layerNodes.clear();
        return compiled;
    }

    RecordLayerSources(layerNodes, compiled);
    return compiled;
}

CompiledGraph NodeGraph::CompileLayers() const {
    // 出力ノードは無くなった。既定のチェーンは空で、下地 1 枚になる。
    return CompileChainFrom(nullptr);
}

CompiledGraph NodeGraph::CompileLayersTo(GraphId nodeId, GraphId /*outputPin*/) const {
    const Node* node = FindNode(nodeId);
    if (node == nullptr || !IsLayerNodeKind(node->kind)) {
        // Path やメッシュのノードはレイヤー列を持たない。下地 1 枚（中立平面）になる。
        return CompileLayers();
    }
    return CompileChainFrom(node);
}

}  // namespace tg::graph
