#include "graph/Road.h"
#include "graph/RoadProfile.h"
#include "graph/RoadMask.h"

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
void BuildArrowMarkings(const RoadGeometry& road, const RoadMarkingNodeSettings& settings,
                        bool leftHandTraffic, renderer::MeshData& result);
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
            // バンク。接線まわりに横ベクトルを回す。right は列末尾（Left 側）へ向くベクトルなので、
            // 正のバンク（Left 側が上がる）では up 側へ回す。
            const float bank = EvaluateBankAngleRadians(path, centerline, centerline.arcLengths[i]);
            if (std::abs(bank) > 1e-6f) {
                const XMVECTOR tangent = XMVector3Normalize(XMVectorSubtract(
                    Load(centers[std::min(i + 1, centers.size() - 1)]), Load(centers[i == 0 ? 0 : i - 1])));
                const XMVECTOR up = XMVector3Normalize(XMVector3Cross(tangent, right));
                right = XMVectorAdd(XMVectorScale(right, std::cos(bank)), XMVectorScale(up, std::sin(bank)));
            }
        }
        const XMVECTOR offset = XMVectorScale(right, settings.widthMeters * 0.5f * miter);
        for (uint32_t column = 0; column <= columns; ++column) {
            const float across = static_cast<float>(column) / static_cast<float>(columns);
            renderer::MeshVertex vertex{};
            XMStoreFloat3(&vertex.position, XMVectorAdd(Load(centers[i]), XMVectorScale(offset, across * 2.0f - 1.0f)));
            vertex.uv = {across * settings.widthMeters / settings.uvRepeatMeters,
                         distance / settings.uvRepeatMeters};
            if (settings.uvAlongU) std::swap(vertex.uv.x, vertex.uv.y);
            vertex.roadUv = vertex.uv;
            built.surface.vertices.push_back(vertex);
            // 列 0 は進行方向に向かって右（右手系 Y-up で (dz, 0, -dx) は左を向く）。
            if (column == 0) AddBoundaryPoint(built.right, vertex.position);
            if (column == columns) AddBoundaryPoint(built.left, vertex.position);
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
                    v.roadUv = v.uv;
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
    // 行ごとの実距離。UV の向きを入れ替えても部品（白線・デカール）が同じ距離を使えるように持つ。
    {
        const size_t rowCount = mesh.vertices.size() / stride;
        built.rowDistances.assign(rowCount, 0.0f);
        for (size_t row = 1; row < rowCount; ++row) {
            const XMVECTOR a = XMVectorScale(XMVectorAdd(Load(mesh.vertices[(row - 1) * stride].position),
                                                         Load(mesh.vertices[(row - 1) * stride + columns].position)), 0.5f);
            const XMVECTOR b = XMVectorScale(XMVectorAdd(Load(mesh.vertices[row * stride].position),
                                                         Load(mesh.vertices[row * stride + columns].position)), 0.5f);
            built.rowDistances[row] = built.rowDistances[row - 1] + Length(XMVectorSubtract(b, a));
        }
    }
    result = std::move(built);
    return true;
}

bool BuildRoadMarkings(const RoadGeometry& road, const RoadMarkingNodeSettings& settings,
                       bool leftHandTraffic, renderer::MeshData& result, std::string& error) {
    result = {};
    error.clear();
    const auto fail = [&](const char* message) { error = message; return false; };
    const auto& surface = road.surface;
    if (road.stride < 2 || surface.vertices.size() < road.stride * 2 ||
        surface.vertices.size() % road.stride != 0 || road.rowDistances.size() != surface.vertices.size() / road.stride)
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
    if (offsets.empty() && !settings.arrows) return fail("中央線・外側線・矢印のどれかを有効にしてください");
    if (settings.arrows && (!std::isfinite(settings.arrowIntervalMeters) || settings.arrowIntervalMeters < 1.0f ||
                            !std::isfinite(settings.arrowLengthMeters) || settings.arrowLengthMeters < 0.5f ||
                            settings.arrowLengthMeters > 20.0f))
        return fail("矢印の間隔は1 m以上、長さは0.5〜20 mにしてください");
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
            const float distance = road.rowDistances[row];
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
                if (settings.uvAlongU) std::swap(vertex.uv.x, vertex.uv.y);
                // 路面上の位置。列 0（Right 端）からの横距離と実距離を道路の UV 反復長で割る。
                vertex.roadUv = {(width * 0.5f + lateral) / road.settings.uvRepeatMeters,
                                 distance / road.settings.uvRepeatMeters};
                if (road.settings.uvAlongU) std::swap(vertex.roadUv.x, vertex.roadUv.y);
                result.vertices.push_back(vertex);
            }
            if (row > 0) {
                const uint32_t a = base + static_cast<uint32_t>(row - 1) * 2;
                result.indices.insert(result.indices.end(), {a, a + 2, a + 1, a + 1, a + 2, a + 3});
            }
        }
    }
    if (settings.arrows) BuildArrowMarkings(road, settings, leftHandTraffic, result);
    return true;
}

namespace {
// 道路面上の任意の点（距離と横位置）。行の間は線形補間する。横位置は正が Left（列末尾）側。
struct SurfaceSample {
    XMVECTOR position;
    XMVECTOR normal;
    XMVECTOR across;
};
SurfaceSample SampleRoadSurface(const RoadGeometry& road, float distance, float lateral) {
    const auto& v = road.surface.vertices;
    const size_t rows = v.size() / road.stride;
    const auto rowDistance = [&](size_t row) { return road.rowDistances[row]; };
    size_t upper = 1;
    while (upper + 1 < rows && rowDistance(upper) < distance) ++upper;
    const size_t lower = upper - 1;
    const float span = rowDistance(upper) - rowDistance(lower);
    const float t = span > 1e-6f ? std::clamp((distance - rowDistance(lower)) / span, 0.0f, 1.0f) : 0.0f;
    const auto at = [&](size_t row, uint32_t column) { return &v[row * road.stride + column]; };
    const auto lerp3 = [&](const XMFLOAT3& a, const XMFLOAT3& b) { return XMVectorLerp(Load(a), Load(b), t); };
    const XMVECTOR left = lerp3(at(lower, 0)->position, at(upper, 0)->position);
    const XMVECTOR right = lerp3(at(lower, road.stride - 1)->position, at(upper, road.stride - 1)->position);
    const XMVECTOR leftNormal = lerp3(at(lower, 0)->normal, at(upper, 0)->normal);
    const XMVECTOR rightNormal = lerp3(at(lower, road.stride - 1)->normal, at(upper, road.stride - 1)->normal);
    const float s = lateral / road.settings.widthMeters;
    SurfaceSample sample;
    sample.across = XMVectorSubtract(right, left);
    sample.position = XMVectorAdd(XMVectorScale(XMVectorAdd(left, right), 0.5f), XMVectorScale(sample.across, s));
    sample.normal = XMVector3Normalize(XMVectorLerp(leftNormal, rightNormal, s + 0.5f));
    return sample;
}

// 進行方向の矢印。左右の車線の中央に一定間隔で置く。走行側の車線は線形の向き、対向車線は逆向き。
void BuildArrowMarkings(const RoadGeometry& road, const RoadMarkingNodeSettings& settings,
                        bool leftHandTraffic, renderer::MeshData& result) {
    const float total = road.rowDistances.empty() ? 0.0f : road.rowDistances.back();
    const float width = road.settings.widthMeters;
    const float length = settings.arrowLengthMeters;
    if (total < length + 1.0f) return;
    const float headHalf = std::clamp(width * 0.09f, 0.2f, 0.6f);
    const float shaftHalf = headHalf * 0.4f;
    const float headLength = length * 0.4f;
    const float base = length * 0.5f - headLength;
    // 車線の中央。正が Left 側。左側通行なら Left の車線が線形の向きへ進む。
    const float laneCenters[2] = {width * 0.25f, -width * 0.25f};
    const bool laneForward[2] = {leftHandTraffic, !leftHandTraffic};
    struct Local { float s, t, u, w; };
    const Local shape[7] = {
        {-length * 0.5f, -shaftHalf, 0.5f - shaftHalf / (2.0f * headHalf), 0.0f},
        {base, -shaftHalf, 0.5f - shaftHalf / (2.0f * headHalf), (base + length * 0.5f) / length},
        {base, shaftHalf, 0.5f + shaftHalf / (2.0f * headHalf), (base + length * 0.5f) / length},
        {-length * 0.5f, shaftHalf, 0.5f + shaftHalf / (2.0f * headHalf), 0.0f},
        {base, -headHalf, 0.0f, (base + length * 0.5f) / length},
        {base, headHalf, 1.0f, (base + length * 0.5f) / length},
        {length * 0.5f, 0.0f, 0.5f, 1.0f},
    };
    const uint32_t triangles[3][3] = {{0, 1, 3}, {1, 2, 3}, {4, 6, 5}};
    for (float center = settings.arrowIntervalMeters * 0.5f; center + length * 0.5f <= total;
         center += settings.arrowIntervalMeters) {
        if (center - length * 0.5f < 0.0f) continue;
        for (int lane = 0; lane < 2; ++lane) {
            const float direction = laneForward[lane] ? 1.0f : -1.0f;
            const uint32_t first = static_cast<uint32_t>(result.vertices.size());
            for (const Local& local : shape) {
                const SurfaceSample sample =
                    SampleRoadSurface(road, center + local.s * direction, laneCenters[lane] + local.t * direction);
                renderer::MeshVertex vertex{};
                XMStoreFloat3(&vertex.position, XMVectorAdd(sample.position, XMVectorScale(sample.normal, settings.liftMeters)));
                XMStoreFloat3(&vertex.normal, sample.normal);
                const XMVECTOR tangent = XMVector3Normalize(XMVectorSubtract(sample.across,
                    XMVectorScale(sample.normal, XMVectorGetX(XMVector3Dot(sample.normal, sample.across)))));
                XMStoreFloat4(&vertex.tangent, tangent);
                vertex.tangent.w = -1.0f;
                vertex.uv = {local.u, local.w};
                if (settings.uvAlongU) std::swap(vertex.uv.x, vertex.uv.y);
                vertex.roadUv = {(width * 0.5f + laneCenters[lane] + local.t * direction) / road.settings.uvRepeatMeters,
                                 (center + local.s * direction) / road.settings.uvRepeatMeters};
                if (road.settings.uvAlongU) std::swap(vertex.roadUv.x, vertex.roadUv.y);
                result.vertices.push_back(vertex);
            }
            for (const auto& triangle : triangles) {
                uint32_t a = first + triangle[0], b = first + triangle[1], c = first + triangle[2];
                // 向きを反転した矢印は巻きも反転するので、法線が路面と同じ側を向くよう並べ直す。
                const XMVECTOR n = XMVector3Cross(
                    XMVectorSubtract(Load(result.vertices[b].position), Load(result.vertices[a].position)),
                    XMVectorSubtract(Load(result.vertices[c].position), Load(result.vertices[a].position)));
                if (XMVectorGetX(XMVector3Dot(n, Load(result.vertices[a].normal))) < 0.0f) std::swap(b, c);
                result.indices.insert(result.indices.end(), {a, b, c});
            }
        }
    }
}
}  // namespace

bool EvaluateRoad(const NodeGraph& graph, GraphId nodeId, RoadGeometry& result, std::string& error) {
    std::unordered_set<GraphId> visiting;
    return Evaluate(graph, nodeId, result, error, visiting);
}

namespace {
// Material 入力に繋いだ Result を合成用のスタックにする。
bool BuildStack(const NodeGraph& graph, const Pin& pin, float metersPerUv, compositor::MaterialStack& out,
                compositor::MaterialAssetId* outTopMaterial) {
    const Node* source = graph.FindUpstreamNodeForPin(pin.id);
    if (source == nullptr) return false;
    auto material = graph.CompileLayersTo(source->id);
    out.Layers() = std::move(material.layers);
    out.MaskOps() = std::move(material.maskOps);
    // 1 UVタイルの実寸でハイト由来の法線を評価する。
    out.SetTerrainScale(metersPerUv, 1.0f);
    if (outTopMaterial) {
        for (auto it = out.Layers().rbegin(); it != out.Layers().rend(); ++it) {
            if (it->material != compositor::kNoMaterialAsset) { *outTopMaterial = it->material; break; }
        }
    }
    return true;
}

// 最初の Material 入力（スロット 1）だけを繋ぐ。白線など、レイヤーを持たないメッシュ用。
void AttachMaterial(const NodeGraph& graph, const Node& node, renderer::SceneMesh& mesh) {
    for (const auto& pin : node.inputs) {
        if (pin.valueType != ValueType::Material) continue;
        compositor::MaterialStack stack;
        if (BuildStack(graph, pin, mesh.roadMetersPerUv, stack, &mesh.blendMaterial)) {
            mesh.materialStack = std::move(stack);
        }
        break;
    }
}

// Road のスロット 1〜4 と道路マスク。スロット 2〜4 は材質とマスクの両方が繋がったときだけ有効。
void AttachRoadLayers(const NodeGraph& graph, const Node& node, const RoadNodeSettings& settings,
                      float lengthMeters, renderer::SceneMesh& mesh) {
    std::vector<const Pin*> materialPins;
    std::vector<const Pin*> maskPins;
    for (const auto& pin : node.inputs) {
        if (pin.valueType == ValueType::Material) materialPins.push_back(&pin);
        if (pin.valueType == ValueType::RoadMask) maskPins.push_back(&pin);
    }
    mesh.roadWidthMeters = settings.widthMeters;
    mesh.roadLengthMeters = lengthMeters;
    mesh.roadUvAlongU = settings.uvAlongU;
    mesh.layerBlendRange = settings.layerBlendRange;
    for (int slot = 0; slot < kRoadMaterialSlots; ++slot) {
        mesh.layerWorldUv[slot] = settings.layerWorldUv[slot];
        mesh.layerUvRepeat[slot] = slot == 0 ? settings.uvRepeatMeters : std::max(0.01f, settings.layerUvRepeatMeters[slot]);
    }
    if (!materialPins.empty()) {
        compositor::MaterialStack stack;
        if (BuildStack(graph, *materialPins[0], settings.uvRepeatMeters, stack, &mesh.blendMaterial)) {
            mesh.materialStack = std::move(stack);
        }
    }
    const RoadMaskNodeSettings* channels[3] = {nullptr, nullptr, nullptr};
    bool anyLayer = false;
    for (int layer = 0; layer < 3; ++layer) {
        if (layer + 1 >= static_cast<int>(materialPins.size()) || layer >= static_cast<int>(maskPins.size())) continue;
        const Node* maskNode = graph.FindUpstreamNodeForPin(maskPins[layer]->id);
        const auto* maskSettings = maskNode ? std::get_if<RoadMaskNodeSettings>(&maskNode->settings) : nullptr;
        compositor::MaterialStack stack;
        if (maskSettings == nullptr ||
            !BuildStack(graph, *materialPins[layer + 1], mesh.layerUvRepeat[layer + 1], stack, nullptr))
            continue;
        mesh.layerStacks[layer] = std::move(stack);
        channels[layer] = maskSettings;
        anyLayer = true;
    }
    if (anyLayer) {
        const RoadMaskImage image = BakeRoadMask(channels, settings.widthMeters, lengthMeters);
        mesh.roadMask.width = image.width;
        mesh.roadMask.height = image.height;
        mesh.roadMask.rgba = image.rgba;
    }
}

// Mesh Outputから上流へたどり、Roadを起点に白線などの部品を順に積む。
struct MeshChain {
    std::vector<renderer::SceneMesh> meshes;
    RoadGeometry road;
    // 道路面が chain.meshes の何番目か。白線の押し出し元にする。
    int roadIndex = -1;
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
            mesh.displacementMeters = std::max(0.0f, chain.road.settings.displacementMeters);
            AttachRoadLayers(graph, *node, chain.road.settings,
                             chain.road.rowDistances.empty() ? 0.0f : chain.road.rowDistances.back(), mesh);
            chain.roadIndex = static_cast<int>(chain.meshes.size());
            chain.meshes.push_back(std::move(mesh));
            success = true;
        }
    } else if (const auto* marking = std::get_if<RoadMarkingNodeSettings>(&node->settings)) {
        const Node* upstream = node->inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node->inputs.front().id);
        if (!upstream) {
            error = "Lane MarkingにRoadSurfaceを接続してください";
        } else if (EvaluateMeshChain(graph, upstream, chain, error, visiting)) {
            renderer::SceneMesh mesh;
            if (BuildRoadMarkings(chain.road, *marking, graph.RoadNetwork().leftHandTraffic, mesh.geometry, error)) {
                mesh.material.baseColor = {0.85f, 0.85f, 0.82f};
                mesh.material.roughness = 0.6f;
                mesh.roadMetersPerUv = marking->uvRepeatMeters;
                mesh.roadGridOverlay = false;
                // 道路面と同じハイト・同じ量で押し出し、変位後の路面に貼り付ける。
                mesh.displacementMeters = std::max(0.0f, chain.road.settings.displacementMeters);
                mesh.displacementSource = chain.roadIndex;
                mesh.useBlendMode = true;
                AttachMaterial(graph, *node, mesh);
                chain.meshes.push_back(std::move(mesh));
                success = true;
            }
        }
    } else if (const auto* decal = std::get_if<DecalNodeSettings>(&node->settings)) {
        const Node* upstream = node->inputs.empty() ? nullptr : graph.FindUpstreamNodeForPin(node->inputs.front().id);
        const Node* pathNode = node->inputs.size() > 1 ? graph.FindUpstreamNodeForPin(node->inputs[1].id) : nullptr;
        const auto* pathSettings = pathNode ? std::get_if<PathNodeSettings>(&pathNode->settings) : nullptr;
        if (!upstream) {
            error = "DecalにRoadSurfaceを接続してください";
        } else if (!pathSettings) {
            error = "DecalのPathに面上のPathを接続してください";
        } else if (EvaluateMeshChain(graph, upstream, chain, error, visiting)) {
            renderer::SceneMesh mesh;
            if (BuildDecal(chain.road, pathSettings->path, *decal, mesh.geometry, error)) {
                mesh.material.baseColor = {0.6f, 0.6f, 0.6f};
                mesh.material.roughness = 0.7f;
                mesh.roadMetersPerUv = decal->uvRepeatMeters;
                mesh.roadGridOverlay = false;
                mesh.displacementMeters = std::max(0.0f, chain.road.settings.displacementMeters);
                mesh.displacementSource = chain.roadIndex;
                mesh.useBlendMode = true;
                AttachMaterial(graph, *node, mesh);
                chain.meshes.push_back(std::move(mesh));
                success = true;
            }
        }
    } else {
        error = "Mesh OutputにはRoad / Lane Marking / DecalのRoadSurfaceを接続してください";
    }
    visiting.erase(node->id);
    return success;
}
}  // namespace


RoadSurfacePoint RoadSurfacePointAt(const RoadGeometry& road, float distanceMeters, float lateralMeters) {
    RoadSurfacePoint point{};
    if (road.stride < 2 || road.surface.vertices.size() < road.stride * 2) return point;
    const SurfaceSample sample = SampleRoadSurface(road, distanceMeters, lateralMeters);
    XMStoreFloat3(&point.position, sample.position);
    XMStoreFloat3(&point.normal, sample.normal);
    return point;
}

bool RoadSurfaceCoordinates(const RoadGeometry& road, const XMFLOAT3& world, float& outDistanceMeters,
                            float& outLateralMeters) {
    const auto& v = road.surface.vertices;
    if (road.stride < 2 || v.size() < road.stride * 2) return false;
    const size_t rows = v.size() / road.stride;
    const XMVECTOR p = Load(world);
    float bestError = 1e30f;
    for (size_t row = 0; row + 1 < rows; ++row) {
        const auto rowCenter = [&](size_t r) {
            return XMVectorScale(XMVectorAdd(Load(v[r * road.stride].position), Load(v[r * road.stride + road.stride - 1].position)), 0.5f);
        };
        const auto rowAcross = [&](size_t r) {
            return XMVectorSubtract(Load(v[r * road.stride + road.stride - 1].position), Load(v[r * road.stride].position));
        };
        const XMVECTOR c0 = rowCenter(row);
        const XMVECTOR c1 = rowCenter(row + 1);
        const XMVECTOR along = XMVectorSubtract(c1, c0);
        const float alongLength = XMVectorGetX(XMVector3LengthSq(along));
        if (alongLength < 1e-8f) continue;
        const float t = std::clamp(XMVectorGetX(XMVector3Dot(XMVectorSubtract(p, c0), along)) / alongLength, 0.0f, 1.0f);
        const XMVECTOR center = XMVectorLerp(c0, c1, t);
        const XMVECTOR across = XMVectorLerp(rowAcross(row), rowAcross(row + 1), t);
        const float acrossLength = XMVectorGetX(XMVector3LengthSq(across));
        if (acrossLength < 1e-8f) continue;
        const float s = XMVectorGetX(XMVector3Dot(XMVectorSubtract(p, center), across)) / acrossLength;
        const XMVECTOR nearest = XMVectorAdd(center, XMVectorScale(across, s));
        const float error = XMVectorGetX(XMVector3LengthSq(XMVectorSubtract(p, nearest)));
        if (error < bestError) {
            bestError = error;
            const float d0 = road.rowDistances[row];
            const float d1 = road.rowDistances[row + 1];
            outDistanceMeters = d0 + (d1 - d0) * t;
            outLateralMeters = s * road.settings.widthMeters;
        }
    }
    return bestError < 1e29f;
}

bool RayHitsRoad(const RoadGeometry& road, const XMFLOAT3& origin, const XMFLOAT3& direction, XMFLOAT3& outHit) {
    const auto& v = road.surface.vertices;
    const auto& indices = road.surface.indices;
    const XMVECTOR o = Load(origin);
    const XMVECTOR d = Load(direction);
    float bestT = 1e30f;
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        // Möller–Trumbore。裏面も拾う（面が傾いていても選べるように）。
        const XMVECTOR a = Load(v[indices[i]].position);
        const XMVECTOR e1 = XMVectorSubtract(Load(v[indices[i + 1]].position), a);
        const XMVECTOR e2 = XMVectorSubtract(Load(v[indices[i + 2]].position), a);
        const XMVECTOR pv = XMVector3Cross(d, e2);
        const float det = XMVectorGetX(XMVector3Dot(e1, pv));
        if (std::abs(det) < 1e-9f) continue;
        const float inv = 1.0f / det;
        const XMVECTOR tv = XMVectorSubtract(o, a);
        const float u = XMVectorGetX(XMVector3Dot(tv, pv)) * inv;
        if (u < 0.0f || u > 1.0f) continue;
        const XMVECTOR qv = XMVector3Cross(tv, e1);
        const float w = XMVectorGetX(XMVector3Dot(d, qv)) * inv;
        if (w < 0.0f || u + w > 1.0f) continue;
        const float t = XMVectorGetX(XMVector3Dot(e2, qv)) * inv;
        if (t > 1e-4f && t < bestT) bestT = t;
    }
    if (bestT >= 1e29f) return false;
    XMStoreFloat3(&outHit, XMVectorAdd(o, XMVectorScale(d, bestT)));
    return true;
}

const Node* FindSurfaceRoad(const NodeGraph& graph, const Node& pathNode) {
    if (pathNode.inputs.empty() || pathNode.inputs.front().valueType != ValueType::Mesh) return nullptr;
    const Node* current = graph.FindUpstreamNodeForPin(pathNode.inputs.front().id);
    for (int depth = 0; current != nullptr && depth < 64; ++depth) {
        if (current->kind == NodeKind::Road) return current;
        // Lane Marking / Decal は RoadSurface を素通しする。最初の Mesh 入力をたどる。
        const Pin* meshInput = nullptr;
        for (const auto& pin : current->inputs) if (pin.valueType == ValueType::Mesh) { meshInput = &pin; break; }
        current = meshInput ? graph.FindUpstreamNodeForPin(meshInput->id) : nullptr;
    }
    return nullptr;
}

bool BuildDecal(const RoadGeometry& road, const PathSettings& surfacePath, const DecalNodeSettings& settings,
                renderer::MeshData& result, std::string& error) {
    result = {};
    error.clear();
    const auto fail = [&](const char* message) { error = message; return false; };
    if (road.stride < 2 || road.surface.vertices.size() < road.stride * 2) return fail("道路面が生成されていません");
    if (!surfacePath.worldSpace || !surfacePath.surfaceSpace) return fail("Path の Surface に道路の RoadSurface を繋いでください");
    if (!std::isfinite(settings.widthMeters) || settings.widthMeters < 0.05f || settings.widthMeters > 50.0f ||
        !std::isfinite(settings.liftMeters) || settings.liftMeters < 0.0f || settings.liftMeters > 0.1f ||
        !std::isfinite(settings.uvRepeatMeters) || settings.uvRepeatMeters < 0.05f || settings.uvRepeatMeters > 100.0f)
        return fail("幅は0.05〜50 m、浮かせ量は0〜0.1 m、UV反復長は0.05〜100 mにしてください");
    const auto strands = BuildPathStrands(surfacePath);
    if (strands.empty()) return fail("Path に点を置いてください");
    const uint32_t stride = 2;
    for (const auto& strand : strands) {
        // 道路座標（x = 横位置, z = 実距離）で曲線を割り、約 0.25 m で刻み直す。
        std::vector<XMFLOAT3> points;
        for (const auto& sample : SamplePathStrand(surfacePath, strand, 24)) {
            const XMFLOAT3 p{sample.x, sample.y, sample.z};
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return fail("Path の座標が不正です");
            if (points.empty() || std::hypot(p.x - points.back().x, p.z - points.back().z) > 1e-5f) points.push_back(p);
        }
        if (points.size() < 2) continue;
        std::vector<float> lengths(points.size(), 0.0f);
        for (size_t i = 1; i < points.size(); ++i)
            lengths[i] = lengths[i - 1] + std::hypot(points[i].x - points[i - 1].x, points[i].z - points[i - 1].z);
        const float total = lengths.back();
        if (!(total > 1e-4f) || total > 16000.0f) return fail("デカールのパスが短すぎるか長すぎます");
        const size_t steps = std::max<size_t>(1, static_cast<size_t>(std::ceil(total / 0.25f)));
        if (result.vertices.size() + (steps + 1) * stride > 200000) return fail("デカールの頂点数が多すぎます");
        std::vector<XMFLOAT3> uniform;
        size_t segment = 1;
        for (size_t i = 0; i <= steps; ++i) {
            const float distance = total * static_cast<float>(i) / static_cast<float>(steps);
            while (segment + 1 < lengths.size() && lengths[segment] < distance) ++segment;
            const float span = lengths[segment] - lengths[segment - 1];
            const float t = span > 1e-6f ? (distance - lengths[segment - 1]) / span : 0.0f;
            XMFLOAT3 p;
            XMStoreFloat3(&p, XMVectorLerp(Load(points[segment - 1]), Load(points[segment]), t));
            uniform.push_back(p);
        }
        const uint32_t base = static_cast<uint32_t>(result.vertices.size());
        for (size_t i = 0; i < uniform.size(); ++i) {
            const XMFLOAT3& p = uniform[i];
            const XMFLOAT3& prev = uniform[i == 0 ? 0 : i - 1];
            const XMFLOAT3& next = uniform[std::min(i + 1, uniform.size() - 1)];
            // 道路座標の平面での接線と、その法線（帯の横方向）。
            float tx = next.x - prev.x;
            float tz = next.z - prev.z;
            const float tl = std::hypot(tx, tz);
            if (tl < 1e-6f) { tx = 0.0f; tz = 1.0f; } else { tx /= tl; tz /= tl; }
            const float nx = -tz, nz = tx;
            const float along = total * static_cast<float>(i) / static_cast<float>(steps);
            for (int side = 0; side < 2; ++side) {
                const float offset = (side == 0 ? -0.5f : 0.5f) * settings.widthMeters;
                const float lateral = p.x + nx * offset;
                const float distance = p.z + nz * offset;
                const SurfaceSample sample = SampleRoadSurface(road, distance, lateral);
                renderer::MeshVertex vertex{};
                XMStoreFloat3(&vertex.position, XMVectorAdd(sample.position, XMVectorScale(sample.normal, settings.liftMeters + p.y)));
                XMStoreFloat3(&vertex.normal, sample.normal);
                const XMVECTOR tangent = XMVector3Normalize(XMVectorSubtract(sample.across,
                    XMVectorScale(sample.normal, XMVectorGetX(XMVector3Dot(sample.normal, sample.across)))));
                XMStoreFloat4(&vertex.tangent, tangent);
                vertex.tangent.w = -1.0f;
                vertex.uv = {static_cast<float>(side), along / settings.uvRepeatMeters};
                if (settings.uvAlongU) std::swap(vertex.uv.x, vertex.uv.y);
                vertex.roadUv = {(road.settings.widthMeters * 0.5f + lateral) / road.settings.uvRepeatMeters,
                                 distance / road.settings.uvRepeatMeters};
                if (road.settings.uvAlongU) std::swap(vertex.roadUv.x, vertex.roadUv.y);
                result.vertices.push_back(vertex);
            }
            if (i > 0) {
                const uint32_t a = base + static_cast<uint32_t>(i - 1) * stride;
                const uint32_t tris[2][3] = {{a, a + 2, a + 1}, {a + 1, a + 2, a + 3}};
                for (const auto& tri : tris) {
                    uint32_t x = tri[0], y = tri[1], z = tri[2];
                    // パスが道路を逆走する区間では巻きが反転するので、法線が路面側を向くよう並べ直す。
                    const XMVECTOR n = XMVector3Cross(
                        XMVectorSubtract(Load(result.vertices[y].position), Load(result.vertices[x].position)),
                        XMVectorSubtract(Load(result.vertices[z].position), Load(result.vertices[x].position)));
                    if (XMVectorGetX(XMVector3Dot(n, Load(result.vertices[x].normal))) < 0.0f) std::swap(y, z);
                    result.indices.insert(result.indices.end(), {x, y, z});
                }
            }
        }
    }
    if (result.vertices.empty()) return fail("Path に 2 点以上の線を置いてください");
    return true;
}

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
        const int base = static_cast<int>(compiled.scene.meshes.size());
        for (auto& mesh : chain.meshes) {
            if (mesh.displacementSource >= 0) mesh.displacementSource += base;
            compiled.scene.meshes.push_back(std::move(mesh));
        }
    }
    return compiled;
}
}  // namespace tg::graph
