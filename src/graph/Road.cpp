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
    if (!curved) {
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
        mesh.roadMetersPerUv = std::get<RoadNodeSettings>(road->settings).uvRepeatMeters;
        compiled.scene.meshes.push_back(std::move(mesh));
    }
    return compiled;
}
}  // namespace tg::graph
