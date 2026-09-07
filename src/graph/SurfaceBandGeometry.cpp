#include "graph/SurfaceBandGeometry.h"
#include <algorithm>
#include <cmath>

namespace tg::graph {
namespace {
struct Profile {
    SurfaceId id;
    const SurfacePreset* preset;
    std::vector<float> knots;
};
DirectX::XMFLOAT2 SampleProfile(const Profile& profile, float t) {
    const auto& points = profile.preset->section;
    const auto upper = std::upper_bound(profile.knots.begin(), profile.knots.end(), t);
    const size_t i = std::clamp(size_t(upper - profile.knots.begin()), size_t(1), points.size() - 1);
    const float u = std::clamp((t - profile.knots[i - 1]) / (profile.knots[i] - profile.knots[i - 1]), 0.0f, 1.0f);
    return {std::lerp(points[i - 1].across, points[i].across, u),
            std::lerp(points[i - 1].height, points[i].height, u)};
}
}

bool CreateRoadsideExample(SurfaceLayoutDocument& document, const NodeGraph& graph, GraphId roadId,
                          SurfaceSide side, std::string& error) {
    error.clear();
    if (side == SurfaceSide::Road) { error = "左または右の沿道を選んでください"; return false; }
    auto next = document;
    RoadGeometry road;
    if (!EvaluateRoad(graph, roadId, road, error)) return false;
    const float length = road.rowDistances.back();
    if (length < 0.1f || length > 50) { error = "試作は長さ0.1〜50 mに対応します"; return false; }
    auto layout = std::find_if(next.layouts.begin(), next.layouts.end(), [&](const auto& l) { return l.roadNode == roadId; });
    if (layout == next.layouts.end()) {
        RoadLayout created; created.id = next.AllocateId(); created.roadNode = roadId;
        SurfaceBand roadBand; roadBand.id = next.AllocateId();
        created.bands.push_back(roadBand); next.layouts.push_back(created); layout = next.layouts.end() - 1;
    }
    for (const auto& band : layout->bands) if (band.side == side) {
        error = "この側には既に沿道の記述があります"; return false;
    }
    SurfacePreset ground, sidewalk;
    ground.id = next.AllocateId(); ground.name = "仮路肩";
    ground.section = {{next.AllocateId(), 0, 0}, {next.AllocateId(), 2, -0.06f}};
    ground.materials.emplace_back();
    sidewalk.id = next.AllocateId(); sidewalk.name = "仮歩道"; sidewalk.role = SurfaceRole::Sidewalk;
    sidewalk.section = {{next.AllocateId(), 0, 0}, {next.AllocateId(), 0, 0.15f}, {next.AllocateId(), 2, 0.15f}};
    sidewalk.materials.emplace_back(); sidewalk.materials[0].baseColor = {0.5f, 0.5f, 0.5f};
    sidewalk.boundaries[0].mode = BoundaryMode::KeepStep; sidewalk.boundaries[0].preserveOutline = true;
    SurfaceBand band; band.id = next.AllocateId(); band.side = side;
    SurfaceSpan a, b;
    a.id = next.AllocateId(); a.preset = ground.id; a.endMeters = length * 0.5f;
    b.id = next.AllocateId(); b.preset = sidewalk.id; b.startMeters = a.endMeters; b.endMeters = length;
    a.blendOutMeters = b.blendInMeters = std::min(2.0f, length * 0.25f);
    band.spans = {a, b}; layout->bands.push_back(band);
    next.presets.push_back(ground); next.presets.push_back(sidewalk);
    renderer::MeshData mesh;
    if (!BuildSurfaceBandGeometry(road, next, layout->bands.back(), mesh, error)) return false;
    document = std::move(next);
    return true;
}

bool BuildSurfaceBandGeometry(const RoadGeometry& road, const SurfaceLayoutDocument& document,
                              const SurfaceBand& band, renderer::MeshData& result, std::string& error) {
    using namespace DirectX;
    error.clear();
    const auto fail = [&](const char* message) { error = message; return false; };
    if (!ValidateSurfaceLayouts(document, error)) return false;
    // 検証対象と生成対象の食い違いを防ぐ。文書内の帯を渡す。
    bool belongs = false;
    for (const auto& layout : document.layouts) for (const auto& candidate : layout.bands)
        if (&candidate == &band) belongs = true;
    if (!belongs || band.side == SurfaceSide::Road || band.spans.empty())
        return fail("沿道生成には文書内の左または右の帯が必要です");
    if (road.stride < 2 || road.rowDistances.size() < 2 ||
        road.surface.vertices.size() != size_t(road.stride) * road.rowDistances.size())
        return fail("沿道生成には道路格子が必要です");
    const float length = road.rowDistances.back();
    if (!std::isfinite(length) || length < 0.1f || length > 50 || road.rowDistances.front() != 0)
        return fail("沿道生成は長さ0.1〜50 mに対応します");
    for (size_t row = 0; row < road.rowDistances.size(); ++row) {
        if (!std::isfinite(road.rowDistances[row]) || (row && road.rowDistances[row] <= road.rowDistances[row - 1]))
            return fail("道路の実距離が不正です");
    }
    for (const auto& vertex : road.surface.vertices) {
        const auto& p = vertex.position;
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return fail("道路の座標が不正です");
    }
    float end = 0;
    std::vector<Profile> profiles;
    std::vector<float> knots{0, 1};
    std::vector<float> distances = road.rowDistances;
    for (const auto& span : band.spans) {
        if (span.startMeters != end) return fail("沿道生成には空白のない区間列が必要です");
        end = span.endMeters;
        const float half = (span.endMeters - span.startMeters) * 0.5f;
        distances.insert(distances.end(), {span.startMeters, span.endMeters,
            span.startMeters + std::min(half, span.blendInMeters), span.endMeters - std::min(half, span.blendOutMeters)});
        if (std::any_of(profiles.begin(), profiles.end(), [&](const auto& p) { return p.id == span.preset; })) continue;
        const auto preset = std::find_if(document.presets.begin(), document.presets.end(), [&](const auto& p) { return p.id == span.preset; });
        if (preset->section.front().across != 0 || preset->section.front().height != 0 || preset->section.back().across <= 0)
            return fail("沿道断面は道路端の(0, 0)から始め、外側へ幅を持たせてください");
        Profile profile{span.preset, &*preset, {0}};
        float arc = 0;
        for (size_t i = 1; i < preset->section.size(); ++i) {
            arc += std::hypot(preset->section[i].across - preset->section[i - 1].across,
                              preset->section[i].height - preset->section[i - 1].height);
            profile.knots.push_back(arc);
        }
        for (auto& knot : profile.knots) { knot /= arc; knots.push_back(knot); }
        profiles.push_back(std::move(profile));
    }
    if (std::abs(end - length) > 0.001f) return fail("沿道区間を道路全長に合わせてください");
    // 急な形状変更を勝手に斜面へ置き換えない。段差の端面生成は後続。
    for (size_t i = 1; i < band.spans.size(); ++i) {
        const auto& a = band.spans[i - 1]; const auto& b = band.spans[i];
        if (a.preset != b.preset && a.blendOutMeters + b.blendInMeters <= 0)
            return fail("異なる沿道プリセットの境界には移行距離が必要です");
    }
    const uint32_t steps = static_cast<uint32_t>(std::ceil(length / 0.25f));
    for (uint32_t i = 0; i <= steps; ++i) distances.push_back(length * float(i) / float(steps));
    const auto sortUnique = [](auto& values) {
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
    };
    sortUnique(distances); sortUnique(knots);
    if (knots.size() > 256 || distances.size() > 8192 || knots.size() * distances.size() > 262144)
        return fail("沿道断面または区間の分割数が多すぎます");
    std::vector<XMFLOAT3> positions;
    for (float distance : distances) {
        const auto upper = std::upper_bound(road.rowDistances.begin(), road.rowDistances.end(), distance);
        const size_t row = std::clamp(size_t(upper - road.rowDistances.begin()), size_t(1), road.rowDistances.size() - 1);
        const float t = std::clamp((distance - road.rowDistances[row - 1]) /
            (road.rowDistances[row] - road.rowDistances[row - 1]), 0.0f, 1.0f);
        const auto edge = [&](uint32_t column) {
            return XMVectorLerp(XMLoadFloat3(&road.surface.vertices[(row - 1) * road.stride + column].position),
                                XMLoadFloat3(&road.surface.vertices[row * road.stride + column].position), t);
        };
        const auto right = edge(0), left = edge(road.stride - 1);
        const bool isLeft = band.side == SurfaceSide::Left;
        const auto delta = XMVectorSubtract(left, right);
        if (XMVectorGetX(XMVector3LengthSq(delta)) < 1e-8f) return fail("道路の幅方向が縮退しています");
        const auto outward = XMVectorScale(XMVector3Normalize(delta), isLeft ? 1.0f : -1.0f);
        const auto origin = isLeft ? left : right;
        const auto samples = SampleSurfaceBand(document, band, distance);
        if (samples.empty()) return fail("沿道区間を道路全長に合わせてください");
        for (float knot : knots) {
            float across = 0, height = 0;
            for (const auto& sample : samples) {
                const auto profile = std::find_if(profiles.begin(), profiles.end(), [&](const auto& p) { return p.id == sample.preset; });
                const auto p = SampleProfile(*profile, knot);
                across += p.x * sample.weight; height += p.y * sample.weight;
            }
            XMFLOAT3 p;
            XMStoreFloat3(&p, XMVectorAdd(origin, XMVectorAdd(XMVectorScale(outward, across), XMVectorSet(0, height, 0, 0))));
            positions.push_back(p);
        }
    }
    renderer::MeshData mesh;
    // 断面の稜線を保つため面ごとに頂点を持つ。隣接面の位置は共通の標本から取る。
    const size_t columns = knots.size();
    for (size_t row = 1; row < distances.size(); ++row) for (size_t col = 1; col < columns; ++col) {
        const size_t ids[] = {(row - 1) * columns + col - 1, row * columns + col - 1,
                              (row - 1) * columns + col, row * columns + col};
        const auto origin = XMLoadFloat3(&positions[ids[0]]);
        const auto across = XMVectorSubtract(XMLoadFloat3(&positions[ids[2]]), origin);
        const auto along = XMVectorSubtract(XMLoadFloat3(&positions[ids[1]]), origin);
        auto normal = XMVector3Cross(along, across);
        if (band.side == SurfaceSide::Right) normal = XMVectorNegate(normal);
        if (XMVectorGetX(XMVector3LengthSq(normal)) < 1e-16f) return fail("沿道の面が縮退しています");
        normal = XMVector3Normalize(normal);
        const uint32_t first = static_cast<uint32_t>(mesh.vertices.size());
        for (size_t i = 0; i < 4; ++i) {
            renderer::MeshVertex vertex{};
            vertex.position = positions[ids[i]];
            XMStoreFloat3(&vertex.normal, normal);
            XMStoreFloat4(&vertex.tangent, XMVector3Normalize(across));
            vertex.tangent.w = band.side == SurfaceSide::Left ? 1.0f : -1.0f;
            vertex.uv = {knots[col - 1 + i / 2], distances[row - 1 + i % 2]};
            vertex.roadUv = vertex.uv;
            mesh.vertices.push_back(vertex);
        }
        if (band.side == SurfaceSide::Left) mesh.indices.insert(mesh.indices.end(), {first, first + 1, first + 2, first + 2, first + 1, first + 3});
        else mesh.indices.insert(mesh.indices.end(), {first, first + 2, first + 1, first + 2, first + 3, first + 1});
    }
    result = std::move(mesh);
    return true;
}
}  // namespace tg::graph
