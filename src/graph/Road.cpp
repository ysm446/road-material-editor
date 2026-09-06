#include "graph/Road.h"

#include <algorithm>
#include <cmath>

#include <unordered_set>

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
    if (curved) {
        std::vector<float> lengths(centers.size(), 0.0f);
        for (size_t i = 1; i < centers.size(); ++i)
            lengths[i] = lengths[i-1] + Length(XMVectorSubtract(Load(centers[i]), Load(centers[i-1])));
        const float total = lengths.back();
        if (!std::isfinite(total) || total > 16000.0f) return fail("道路が長すぎます");
        const size_t steps = std::max(size_t(1), static_cast<size_t>(std::ceil(total / 0.25f)));
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
    std::vector<XMFLOAT3> rights;
    for (size_t i = 1; i < centers.size(); ++i) {
        const float dx = centers[i].x - centers[i-1].x;
        const float dz = centers[i].z - centers[i-1].z;
        const float horizontal = std::hypot(dx, dz);
        if (horizontal < 1e-5f) return fail("垂直な区間には道路面を生成できません");
        rights.push_back({dz / horizontal, 0.0f, -dx / horizontal});
    }
    RoadGeometry built;
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
        const XMVECTOR offset = XMVectorScale(right, settings.widthMeters * 0.5f * miter);
        for (int side = 0; side < 2; ++side) {
            renderer::MeshVertex vertex{};
            XMStoreFloat3(&vertex.position, XMVectorAdd(Load(centers[i]), XMVectorScale(offset, side == 0 ? -1.0f : 1.0f)));
            vertex.uv = {side * settings.widthMeters / settings.uvRepeatMeters,
                         distance / settings.uvRepeatMeters};
            built.surface.vertices.push_back(vertex);
            AddBoundaryPoint(side == 0 ? built.left : built.right, vertex.position);
        }
        if (i > 0) {
            const uint32_t a = static_cast<uint32_t>((i-1)*2);
            built.surface.indices.insert(built.surface.indices.end(), {a,a+2,a+1,a+1,a+2,a+3});
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
        const XMVECTOR across = XMVectorSubtract(Load(mesh.vertices[(i/2)*2+1].position), Load(mesh.vertices[(i/2)*2].position));
        const XMVECTOR t = XMVector3Normalize(XMVectorSubtract(across, XMVectorScale(n, XMVectorGetX(XMVector3Dot(n, across)))));
        XMStoreFloat3(&vertex.normal, n);
        XMStoreFloat4(&vertex.tangent, t);
        vertex.tangent.w = -1.0f;
    }
    result = std::move(built);
    return true;
}

bool EvaluateRoad(const NodeGraph& graph, GraphId nodeId, RoadGeometry& result, std::string& error) {
    std::unordered_set<GraphId> visiting;
    return Evaluate(graph, nodeId, result, error, visiting);
}

CompiledMeshGraph CompileMeshGraph(const NodeGraph& graph) {
    CompiledMeshGraph compiled;
    for (const auto& node : graph.Nodes()) {
        if (node.kind != NodeKind::MeshOutput) continue;
        compiled.active = true;
        const Node* road = node.inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node.inputs.front().id);
        RoadGeometry geometry;
        std::string error;
        if (!road || !EvaluateRoad(graph, road->id, geometry, error)) {
            if (!compiled.error.empty()) compiled.error += " / ";
            compiled.error += error.empty() ? "Mesh OutputにRoadSurfaceを接続してください" : error;
            continue;
        }
        renderer::SceneMesh mesh;
        mesh.geometry = std::move(geometry.surface);
        mesh.material.roughness = 0.85f;
        compiled.scene.meshes.push_back(std::move(mesh));
    }
    return compiled;
}
}  // namespace tg::graph
