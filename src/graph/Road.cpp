#include "graph/Road.h"
#include "graph/RoadProfile.h"

#include <algorithm>
#include <cmath>

#include <string>
#include <unordered_set>
#include <vector>

namespace tg::graph {
namespace {
using namespace DirectX;
XMFLOAT3 Position(const PathCurveSample& p) { return {p.x, p.y, p.z}; }
XMVECTOR Load(const XMFLOAT3& p) { return XMLoadFloat3(&p); }
float Length(XMVECTOR p) { return XMVectorGetX(XMVector3Length(p)); }
void AddBoundaryPoint(PathSettings& path, const XMFLOAT3& p) {
    const auto previous = path.points.empty() ? 0 : path.points.back().id;
    const auto id = AddPathPoint(path, p.x, p.z, 0);
    path.FindPoint(id)->y = p.y;
    if (previous != 0) ConnectPathPoints(path, previous, id);
}

bool Evaluate(const NodeGraph& graph, GraphId nodeId, RoadGeometry& result,
              std::string& error, std::unordered_set<GraphId>& visiting) {
    const Node* node = graph.FindNode(nodeId);
    const auto* settings = node ? std::get_if<RoadNodeSettings>(&node->settings) : nullptr;
    if (!settings || node->inputs.empty()) { error = "Roadノードが接続されていません"; return false; }
    if (visiting.size() >= 64 || !visiting.insert(nodeId).second) {
        error = "道路の依存が循環しているか、深すぎます"; return false;
    }
    const Pin* source = nullptr;
    for (const auto& link : graph.Links()) {
        if (link.endPin == node->inputs.front().id) source = graph.FindPin(link.startPin);
    }
    const Node* upstream = source ? graph.FindNode(source->nodeId) : nullptr;
    bool success = false;
    if (const auto* path = upstream ? std::get_if<PathNodeSettings>(&upstream->settings) : nullptr) {
        success = BuildRoad(path->path, *settings, result, error);
    } else if (upstream && upstream->kind == NodeKind::Road) {
        RoadGeometry parent;
        if (Evaluate(graph, upstream->id, parent, error, visiting)) {
            success = BuildRoad(source->label == "Left" ? parent.left : parent.right,
                                *settings, result, error);
        }
    } else {
        error = "実寸Pathを接続してください";
    }
    visiting.erase(nodeId);
    return success;
}
}  // namespace

bool BuildRoad(const PathSettings& path, const RoadNodeSettings& settings,
               RoadGeometry& result, std::string& error) {
    result = {};
    error.clear();
    const auto fail = [&](const char* message) { error = message; return false; };
    if (!path.worldSpace) return fail("旧地形Pathを実寸カーブへ変換してください");
    if (!std::isfinite(settings.widthMeters) || settings.widthMeters < 0.1f ||
        settings.widthMeters > 50.0f || !std::isfinite(settings.uvRepeatMeters) ||
        settings.uvRepeatMeters < 0.1f || settings.uvRepeatMeters > 100.0f)
        return fail("幅は0.1〜50 m、UV反復長は0.1〜100 mにしてください");
    if (path.points.size() > 2048 || path.edges.size() > 2048)
        return fail("Pathが大きすぎます（最大2048点）");
    const auto strands = BuildPathStrands(path);
    if (strands.size() != 1 || strands.front().closed || strands.front().points.size() != path.points.size())
        return fail("分岐・閉ループ・孤立点のない1本のPathが必要です");
    const auto samples = SamplePathStrand(path, strands.front(), 24);
    std::vector<XMFLOAT3> centers;
    for (const auto& sample : samples) {
        auto p = Position(sample);
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            return fail("Pathの座標が不正です");
        if (centers.empty() || Length(XMVectorSubtract(Load(p), Load(centers.back()))) > 1e-5f)
            centers.push_back(p);
    }
    if (centers.size() < 2 || centers.size() > 65536) return fail("道路を生成できる点数ではありません");
    // 曲線の端ではパラメータ刻みが極小区間になる。実距離で刻み直し、
    // 数値誤差や接線の揺れが道路端で増幅されないようにする。直線の角は保持する。
    bool curved = false;
    for (const auto& edge : path.edges) curved |= edge.curve != PathCurve::Line;
    // 縦断曲線とバンクは距離に沿って連続に変わるので、直線の線形でも実距離で刻み直す。
    const bool profiled = !path.verticalPoints.empty() || path.bankEnabled;
    const bool resample = curved || profiled;
    if (resample) {
        std::vector<float> lengths(centers.size(), 0.0f);
        for (size_t i = 1; i < centers.size(); ++i)
            lengths[i] = lengths[i-1] + Length(XMVectorSubtract(Load(centers[i]), Load(centers[i-1])));
        const float total = lengths.back();
        if (!std::isfinite(total) || total > 16000.0f) return fail("道路が長すぎます");
        const size_t steps = std::max(size_t(1), static_cast<size_t>(std::ceil(total / 1.0f)));
        std::vector<XMFLOAT3> uniform;
        size_t segment = 1;
        for (size_t i = 0; i <= steps; ++i) {
            const float distance = total * static_cast<float>(i) / static_cast<float>(steps);
            while (segment + 1 < lengths.size() && lengths[segment] < distance) ++segment;
            const float t = (distance-lengths[segment-1]) / (lengths[segment]-lengths[segment-1]);
            XMFLOAT3 p;
            XMStoreFloat3(&p, XMVectorLerp(Load(centers[segment-1]), Load(centers[segment]), t));
            uniform.push_back(p);
        }
        centers = std::move(uniform);
    }
    // 縦断曲線。線形の高さを距離軸の放物線で置き換える。ポイントが無ければそのまま。
    {
        const ProfileCurve base = BuildProfileCurve(centers);
        const std::vector<float> heights = EvaluateVerticalProfile(path, base);
        for (size_t i = 0; i < centers.size(); ++i) centers[i].y = heights[i];
    }
    const ProfileCurve centerline = BuildProfileCurve(centers);
    const uint32_t columns = static_cast<uint32_t>(std::ceil(settings.widthMeters));
    const uint32_t stride = columns + 1;
    std::vector<XMFLOAT3> rights;
    for (size_t i = 1; i < centers.size(); ++i) {
        const float dx = centers[i].x - centers[i-1].x;
        const float dz = centers[i].z - centers[i-1].z;
        const float horizontal = std::hypot(dx, dz);
        if (horizontal < 1e-5f) return fail("垂直な区間には道路面を生成できません");
        rights.push_back({dz / horizontal, 0.0f, -dx / horizontal});
    }
    RoadGeometry built;
    built.stride = stride;
    built.settings = settings;
    built.left.worldSpace = built.right.worldSpace = true;
    float distance = 0.0f;
    for (size_t i = 0; i < centers.size(); ++i) {
        XMVECTOR right = Load(rights[std::min(i, rights.size()-1)]);
        float miter = 1.0f;
        if (i > 0 && i < rights.size()) {
            const XMVECTOR sum = XMVectorAdd(right, Load(rights[i-1]));
            if (Length(sum) < 1e-4f) return fail("折り返しが急すぎます。カーブを緩めてください");
            const XMVECTOR average = XMVector3Normalize(sum);
            const float dot = XMVectorGetX(XMVector3Dot(average, right));
            if (dot < 0.25f) return fail("角が急すぎます。カーブを緩めてください");
            miter = 1.0f / dot;
            right = average;
        }
        if (i > 0) distance += Length(XMVectorSubtract(Load(centers[i]), Load(centers[i-1])));
        if (path.bankEnabled) {
            // バンク。接線まわりに横ベクトルを回す。正で Left（列 0）側が上がる。
            const float bank = EvaluateBankAngleRadians(path, centerline, centerline.arcLengths[i]);
            if (std::abs(bank) > 1e-6f) {
                const XMVECTOR tangent = XMVector3Normalize(XMVectorSubtract(
                    Load(centers[std::min(i + 1, centers.size() - 1)]), Load(centers[i == 0 ? 0 : i - 1])));
                const XMVECTOR up = XMVector3Normalize(XMVector3Cross(tangent, right));
                right = XMVectorSubtract(XMVectorScale(right, std::cos(bank)), XMVectorScale(up, std::sin(bank)));
            }
        }
        const XMVECTOR offset = XMVectorScale(right, settings.widthMeters * 0.5f * miter);
        for (uint32_t column = 0; column <= columns; ++column) {
            const float across = static_cast<float>(column) / static_cast<float>(columns);
            renderer::MeshVertex vertex{};
            XMStoreFloat3(&vertex.position, XMVectorAdd(Load(centers[i]), XMVectorScale(offset, across * 2.0f - 1.0f)));
            vertex.uv = {across * settings.widthMeters / settings.uvRepeatMeters,
                         distance / settings.uvRepeatMeters};
            built.surface.vertices.push_back(vertex);
            if (column == 0) AddBoundaryPoint(built.left, vertex.position);
            if (column == columns) AddBoundaryPoint(built.right, vertex.position);
        }
        if (i > 0) {
            for (uint32_t column = 0; column < columns; ++column) {
                const uint32_t a = static_cast<uint32_t>(i-1)*stride + column;
                built.surface.indices.insert(built.surface.indices.end(),
                    {a,a+stride,a+1,a+1,a+stride,a+stride+1});
            }
        }
    }
    auto& mesh = built.surface;
    for (size_t i = 0; i < mesh.indices.size(); i += 3) {
        auto& a = mesh.vertices[mesh.indices[i]];
        auto& b = mesh.vertices[mesh.indices[i+1]];
        auto& c = mesh.vertices[mesh.indices[i+2]];
        const XMVECTOR n = XMVector3Cross(XMVectorSubtract(Load(b.position), Load(a.position)),
                                         XMVectorSubtract(Load(c.position), Load(a.position)));
        if (XMVectorGetY(n) <= 1e-7f) return fail("幅に対してカーブが急すぎて道路面が反転します");
        for (auto* vertex : {&a, &b, &c}) XMStoreFloat3(&vertex->normal, XMVectorAdd(Load(vertex->normal), n));
    }
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        auto& vertex = mesh.vertices[i];
        const XMVECTOR n = XMVector3Normalize(Load(vertex.normal));
        const XMVECTOR across = XMVectorSubtract(Load(mesh.vertices[(i/stride)*stride+columns].position), Load(mesh.vertices[(i/stride)*stride].position));
        const XMVECTOR t = XMVector3Normalize(XMVectorSubtract(across, XMVectorScale(n, XMVectorGetX(XMVector3Dot(n, across)))));
        XMStoreFloat3(&vertex.normal, n);
        XMStoreFloat4(&vertex.tangent, t);
        vertex.tangent.w = -1.0f;
    }
    // 直線は先に角のマイターを作ってから行を補間する。先に中心線だけを
    // 分割すると、角の短い区間で幅の内側が反転してしまう。
    if (!resample) {
        renderer::MeshData divided;
        for (size_t row = 0; row < centers.size(); ++row) {
            size_t steps = 1;
            if (row > 0) {
                const float length = Length(XMVectorSubtract(Load(centers[row]), Load(centers[row-1])));
                if (!std::isfinite(length) || length > 16000.0f) return fail("道路が長すぎます");
                steps = std::max(size_t(1), static_cast<size_t>(std::ceil(length)));
            }
            if (divided.vertices.size()/stride + steps > 65536) return fail("道路の分割数が多すぎます");
            for (size_t step = 1; step <= steps; ++step) {
                const float t = static_cast<float>(step)/static_cast<float>(steps);
                for (uint32_t column = 0; column <= columns; ++column) {
                    const auto& b = mesh.vertices[row*stride+column];
                    const auto& a = mesh.vertices[(row == 0 ? 0 : row-1)*stride+column];
                    renderer::MeshVertex v;
                    XMStoreFloat3(&v.position, XMVectorLerp(Load(a.position), Load(b.position), t));
                    const auto n = XMVector3Normalize(XMVectorLerp(Load(a.normal), Load(b.normal), t));
                    const auto tangent = XMVectorLerp(XMLoadFloat4(&a.tangent), XMLoadFloat4(&b.tangent), t);
                    XMStoreFloat3(&v.normal, n);
                    XMStoreFloat4(&v.tangent, XMVector3Normalize(XMVectorSubtract(tangent,
                        XMVectorScale(n,XMVectorGetX(XMVector3Dot(n,tangent))))));
                    v.tangent.w = -1.0f;
                    v.uv = {a.uv.x+(b.uv.x-a.uv.x)*t,a.uv.y+(b.uv.y-a.uv.y)*t};
                    divided.vertices.push_back(v);
                }
                const auto count = static_cast<uint32_t>(divided.vertices.size()/stride);
                if (count > 1) for (uint32_t column = 0; column < columns; ++column) {
                    const auto a = (count-2)*stride+column;
                    divided.indices.insert(divided.indices.end(),{a,a+stride,a+1,a+1,a+stride,a+stride+1});
                }
            }
        }
        mesh = std::move(divided);
    }
    result = std::move(built);
    return true;
}

bool BuildRoadMarkings(const RoadGeometry& road, const RoadMarkingNodeSettings& settings,
                       renderer::MeshData& result, std::string& error) {
    result = {};
    error.clear();
    const auto fail = [&](const char* message) { error = message; return false; };
    const auto& surface = road.surface;
    if (road.stride < 2 || surface.vertices.size() < road.stride * 2 ||
        surface.vertices.size() % road.stride != 0)
        return fail("道路面が生成されていません");
    const float width = road.settings.widthMeters;
    const float line = settings.lineWidthMeters;
    if (!std::isfinite(line) || line < 0.05f || line > 1.0f ||
        !std::isfinite(settings.edgeInsetMeters) || settings.edgeInsetMeters < 0.0f ||
        !std::isfinite(settings.liftMeters) || settings.liftMeters < 0.0f || settings.liftMeters > 0.1f ||
        !std::isfinite(settings.uvRepeatMeters) || settings.uvRepeatMeters < 0.1f ||
        settings.uvRepeatMeters > 100.0f)
        return fail("線幅は0.05〜1 m、浮かせ量は0〜0.1 m、UV反復長は0.1〜100 mにしてください");
    // 帯の中心の横位置（m）。負が左、正が右。
    std::vector<float> offsets;
    if (settings.centerLine) offsets.push_back(0.0f);
    if (settings.edgeLines) {
        const float edge = width * 0.5f - settings.edgeInsetMeters;
        if (edge - line * 0.5f < 0.0f) return fail("外側線が中心を越えています。端からの距離を小さくしてください");
        if (settings.centerLine && edge - line * 0.5f < line * 0.5f)
            return fail("道路幅に対して線が重なります。線幅か端からの距離を見直してください");
        offsets.push_back(-edge);
        offsets.push_back(edge);
    }
    if (offsets.empty()) return fail("中央線か外側線のどちらかを有効にしてください");
    for (const float offset : offsets)
        if (std::abs(offset) + line * 0.5f > width * 0.5f + 1e-4f)
            return fail("線が道路の外に出ます。線幅か端からの距離を見直してください");
    const size_t rows = surface.vertices.size() / road.stride;
    if (rows * offsets.size() * 2 > 65536 * 3) return fail("白線の頂点数が多すぎます");
    for (const float offset : offsets) {
        const uint32_t base = static_cast<uint32_t>(result.vertices.size());
        for (size_t row = 0; row < rows; ++row) {
            const auto& left = surface.vertices[row * road.stride];
            const auto& right = surface.vertices[row * road.stride + road.stride - 1];
            // 左右端の差は幅にマイター倍率を掛けた横ベクトル。角でも道路端と平行な帯になる。
            const XMVECTOR across = XMVectorSubtract(Load(right.position), Load(left.position));
            const XMVECTOR center = XMVectorScale(XMVectorAdd(Load(left.position), Load(right.position)), 0.5f);
            const float distance = left.uv.y * road.settings.uvRepeatMeters;
            for (int side = 0; side < 2; ++side) {
                const float lateral = offset + (side == 0 ? -line : line) * 0.5f;
                const float t = lateral / width;
                const XMVECTOR n = XMVector3Normalize(XMVectorLerp(Load(left.normal), Load(right.normal), t + 0.5f));
                renderer::MeshVertex vertex{};
                XMStoreFloat3(&vertex.position, XMVectorAdd(XMVectorAdd(center, XMVectorScale(across, t)),
                                                            XMVectorScale(n, settings.liftMeters)));
                XMStoreFloat3(&vertex.normal, n);
                const XMVECTOR tangent = XMVector3Normalize(XMVectorSubtract(across,
                    XMVectorScale(n, XMVectorGetX(XMVector3Dot(n, across)))));
                XMStoreFloat4(&vertex.tangent, tangent);
                vertex.tangent.w = -1.0f;
                vertex.uv = {static_cast<float>(side), distance / settings.uvRepeatMeters};
                result.vertices.push_back(vertex);
            }
            if (row > 0) {
                const uint32_t a = base + static_cast<uint32_t>(row - 1) * 2;
                result.indices.insert(result.indices.end(), {a, a + 2, a + 1, a + 1, a + 2, a + 3});
            }
        }
    }
    return true;
}

bool EvaluateRoad(const NodeGraph& graph, GraphId nodeId, RoadGeometry& result, std::string& error) {
    std::unordered_set<GraphId> visiting;
    return Evaluate(graph, nodeId, result, error, visiting);
}

namespace {
// Material入力に繋いだResultを、そのメッシュの材質として合成する。
void AttachMaterial(const NodeGraph& graph, const Node& node, renderer::SceneMesh& mesh) {
    for (const auto& pin : node.inputs) {
        if (pin.valueType != ValueType::Material) continue;
        if (const Node* source = graph.FindUpstreamNodeForPin(pin.id)) {
            auto material = graph.CompileLayersTo(source->id);
            mesh.materialStack.emplace();
            mesh.materialStack->Layers() = std::move(material.layers);
            mesh.materialStack->MaskOps() = std::move(material.maskOps);
            // 1 UVタイルの実寸でハイト由来の法線を評価する。
            mesh.materialStack->SetTerrainScale(mesh.roadMetersPerUv, 1.0f);
        }
    }
}

// Mesh Outputから上流へたどり、Roadを起点に白線などの部品を順に積む。
struct MeshChain {
    std::vector<renderer::SceneMesh> meshes;
    RoadGeometry road;
};
bool EvaluateMeshChain(const NodeGraph& graph, const Node* node, MeshChain& chain,
                       std::string& error, std::unordered_set<GraphId>& visiting) {
    if (!node) { error = "Mesh OutputにRoadSurfaceを接続してください"; return false; }
    if (visiting.size() >= 64 || !visiting.insert(node->id).second) {
        error = "メッシュの依存が循環しているか、深すぎます"; return false;
    }
    bool success = false;
    if (node->kind == NodeKind::Road) {
        if (EvaluateRoad(graph, node->id, chain.road, error)) {
            renderer::SceneMesh mesh;
            mesh.geometry = chain.road.surface;
            mesh.material.roughness = 0.85f;
            mesh.roadMetersPerUv = chain.road.settings.uvRepeatMeters;
            AttachMaterial(graph, *node, mesh);
            chain.meshes.push_back(std::move(mesh));
            success = true;
        }
    } else if (const auto* marking = std::get_if<RoadMarkingNodeSettings>(&node->settings)) {
        const Node* upstream = node->inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node->inputs.front().id);
        if (!upstream) {
            error = "Lane MarkingにRoadSurfaceを接続してください";
        } else if (EvaluateMeshChain(graph, upstream, chain, error, visiting)) {
            renderer::SceneMesh mesh;
            if (BuildRoadMarkings(chain.road, *marking, mesh.geometry, error)) {
                mesh.material.baseColor = {0.85f, 0.85f, 0.82f};
                mesh.material.roughness = 0.6f;
                mesh.roadMetersPerUv = marking->uvRepeatMeters;
                mesh.roadGridOverlay = false;
                AttachMaterial(graph, *node, mesh);
                chain.meshes.push_back(std::move(mesh));
                success = true;
            }
        }
    } else {
        error = "Mesh OutputにはRoadかLane MarkingのRoadSurfaceを接続してください";
    }
    visiting.erase(node->id);
    return success;
}
}  // namespace

CompiledMeshGraph CompileMeshGraph(const NodeGraph& graph) {
    CompiledMeshGraph compiled;
    for (const auto& node : graph.Nodes()) {
        if (node.kind != NodeKind::MeshOutput) continue;
        compiled.active = true;
        const Node* source = node.inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node.inputs.front().id);
        MeshChain chain;
        std::string error;
        std::unordered_set<GraphId> visiting;
        if (!EvaluateMeshChain(graph, source, chain, error, visiting)) {
            if (!compiled.error.empty()) compiled.error += " / ";
            compiled.error += error;
            continue;
        }
        for (auto& mesh : chain.meshes) compiled.scene.meshes.push_back(std::move(mesh));
    }
    return compiled;
}
}  // namespace tg::graph
