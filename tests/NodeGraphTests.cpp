// ノードグラフから評価用レイヤー列へのコンパイルを確かめる。
// GPU 評価の前段だけを対象にし、入力を外したときに古い結果を残さない規則を固定する。

#include "graph/NodeGraph.h"

#include "TestSupport.h"

#include <array>
#include <variant>
#include <cmath>

namespace {

using tg::graph::NodeGraph;
using tg::graph::NodeKind;
using tg::tests::Check;
using tg::tests::Section;

bool IsNeutralPlane(const tg::graph::CompiledGraph& compiled) {
    if (compiled.layers.size() != 1) {
        return false;
    }
    const tg::compositor::MaterialLayer& layer = compiled.layers.front();
    return layer.enabled && !tg::compositor::IsHeightOperationKind(layer.kind) &&
           layer.heightSource == tg::compositor::ValueSource::Constant &&
           layer.heightBase == tg::compositor::kHeightPivot;
}

}  // namespace

void RunNodeGraphTests() {
    Section("ノードグラフ — 既定の下地と Surface の鎖");
    {
        NodeGraph graph = NodeGraph::CreateDefault();
        Check(graph.Nodes().size() == 1 && graph.Nodes().front().kind == NodeKind::Surface,
              "既定のグラフは Surface 1 つ");
        Check(IsNeutralPlane(graph.CompileLayers()),
              "出力ノードは無いので、既定のレイヤー列は変位 0 の平面になる");

        // Surface → Surface の鎖は、下から上のレイヤー列になる。
        const tg::graph::GraphId baseId = graph.Nodes().front().id;
        const tg::graph::GraphId topId = graph.CreateNode(NodeKind::Surface);
        const tg::graph::Node* base = graph.FindNode(baseId);
        const tg::graph::Node* top = graph.FindNode(topId);
        const bool linked = base != nullptr && top != nullptr && !base->outputs.empty() &&
                            !top->inputs.empty() &&
                            graph.CreateLink(base->outputs.front().id, top->inputs.front().id);
        const tg::graph::CompiledGraph compiled = graph.CompileLayersTo(topId);
        Check(linked && compiled.layers.size() == 2 && compiled.layerSources.size() == 2 &&
                  compiled.layerSources[0] == baseId && compiled.layerSources[1] == topId,
              "Surface の鎖は下から上のレイヤー列になり、元ノードを控える");
        Check(compiled.maskOps.empty(), "マスクを出すノードは無いので op の列は空");
    }

    Section("パス — 実寸カーブの範囲外編集");
    {
        NodeGraph graph;
        const auto id = graph.CreateNode(NodeKind::Path);
        auto& path = std::get<tg::graph::PathNodeSettings>(graph.FindMutableNode(id)->settings).path;
        Check(path.worldSpace, "新規Pathは実寸座標");
        const auto a = tg::graph::AddPathPoint(path, -30.0f, -12.0f, 0);
        path.FindPoint(a)->y = 3.0f;
        const auto b = tg::graph::AddPathPoint(path, 40.0f, 15.0f, a);
        Check(path.FindPoint(a)->x == -30.0f && path.FindPoint(b)->x == 40.0f &&
              path.FindPoint(b)->y == 3.0f, "グリッド外の点を追加し起点の高さを継ぐ");
        tg::graph::MovePathPoints(path, {a,b}, 20.0f, -50.0f);
        Check(path.FindPoint(a)->x == -10.0f && path.FindPoint(b)->z == -35.0f,
              "範囲外へまとめて動かしても形を保つ");
        tg::graph::PathClip clip;
        tg::graph::ExtractPathClip(path, {a,b}, {}, clip);
        std::vector<tg::graph::PathElementId> pasted;
        Check(tg::graph::PastePathClip(path, clip, 100.0f, 100.0f, &pasted, nullptr) &&
              pasted.size() == 2 && path.FindPoint(pasted.front())->x == 90.0f,
              "貼り付けでも座標を丸めない");
        const auto strands = tg::graph::BuildPathStrands(path);
        const auto samples = tg::graph::SamplePathStrand(path, strands.front(), 16);
        Check(!samples.empty() && samples.front().y == 3.0f &&
              std::abs(samples.back().x - samples.front().x) == 70.0f,
              "道路の入力となるカーブ標本は実寸と高さを保持");
    }

    Section("パス — XYZカーブの補間");
    {
        using namespace tg::graph;
        PathSettings vertical;
        vertical.worldSpace = true;
        const auto a = AddPathPoint(vertical, 0.0f, 0.0f, 0);
        const auto b = AddPathPoint(vertical, 0.0f, 0.0f, a);
        vertical.FindPoint(b)->y = 10.0f;
        const auto inserted = InsertPathPointOnEdge(vertical, vertical.edges.front().id, 0.5f);
        Check(inserted != 0 && std::abs(vertical.FindPoint(inserted)->y - 5.0f) < 1e-5f,
              "垂直エッジの中点も3次元の距離で挿入する");
        for (const auto curve : {PathCurve::Quadratic, PathCurve::Cubic}) {
            PathSettings path;
            path.worldSpace = true;
            const auto p0 = AddPathPoint(path, 0.0f, 0.0f, 0);
            const auto p1 = AddPathPoint(path, 2.0f, 3.0f, p0);
            const auto p2 = AddPathPoint(path, 9.0f, 1.0f, p1);
            path.FindPoint(p1)->y = 10.0f;
            path.FindPoint(p2)->y = -2.0f;
            for (auto& edge : path.edges) edge.curve = curve;
            auto rotated = path;
            for (auto& point : rotated.points) std::swap(point.x, point.y);
            const auto samples = SamplePathStrand(path, BuildPathStrands(path).front(), 16);
            const auto transformed = SamplePathStrand(rotated, BuildPathStrands(rotated).front(), 16);
            bool consistent = samples.size() == transformed.size();
            for (size_t i = 0; consistent && i < samples.size(); ++i) {
                consistent = std::abs(samples[i].x - transformed[i].y) < 1e-5f &&
                             std::abs(samples[i].y - transformed[i].x) < 1e-5f &&
                             std::abs(samples[i].z - transformed[i].z) < 1e-5f;
            }
            Check(consistent, "ベジェ/Bスプラインは軸を入れ替えても同じ3次元曲線になる");
        }
    }

    Section("パス — まとめて動かす / コピーと貼り付け");
    {
        using tg::graph::PathClip;
        using tg::graph::PathElementId;
        using tg::graph::PathSettings;
        PathSettings path;
        const PathElementId a = tg::graph::AddPathPoint(path, 0.2f, 0.2f, 0);
        const PathElementId b = tg::graph::AddPathPoint(path, 0.4f, 0.2f, a);
        const PathElementId c = tg::graph::AddPathPoint(path, 0.4f, 0.4f, b);
        const PathElementId lone = tg::graph::AddPathPoint(path, 0.9f, 0.9f, 0);
        const tg::graph::PathEdge* ab = path.FindEdgeBetween(a, b);
        const tg::graph::PathEdge* bc = path.FindEdgeBetween(b, c);
        Check(ab != nullptr && bc != nullptr && lone != 0, "3 点の鎖と孤立点を作れる");

        // まとめて動かす。0〜1 へ丸める。
        const bool moved = tg::graph::MovePathPoints(path, {a, b, c}, 0.1f, -0.3f);
        const tg::graph::PathPoint* pa = path.FindPoint(a);
        const tg::graph::PathPoint* pc = path.FindPoint(c);
        Check(moved && pa != nullptr && pc != nullptr && std::abs(pa->x - 0.3f) < 1e-5f &&
                  pa->z == 0.0f && std::abs(pc->x - 0.5f) < 1e-5f && std::abs(pc->z - 0.1f) < 1e-5f,
              "MovePathPoints は指定した点だけを動かし、0〜1 へ丸める");
        float cu = 0.0f;
        float cv = 0.0f;
        Check(tg::graph::PathPointsCentroid(path, {a, b, c}, cu, cv) &&
                  std::abs(cu - (0.3f + 0.5f + 0.5f) / 3.0f) < 1e-5f,
              "PathPointsCentroid は重心を返す");

        // 鎖を切り出す。エッジは両端の点を連れていき、内部点は持ち越さない。
        if (ab != nullptr && bc != nullptr) {
            tg::graph::PathEdge* mutableAb = const_cast<tg::graph::PathEdge*>(ab);
            mutableAb->routed = true;
            mutableAb->waypoints.push_back({0.35f, 0.1f});
            mutableAb->curve = tg::graph::PathCurve::Cubic;
        }
        PathClip clip;
        const bool extracted = tg::graph::ExtractPathClip(path, {}, {ab->id, bc->id}, clip);
        Check(extracted && clip.points.size() == 3 && clip.edges.size() == 2 &&
                  !clip.edges.front().routed && clip.edges.front().waypoints.empty() &&
                  clip.edges.front().curve == tg::graph::PathCurve::Cubic,
              "ExtractPathClip は鎖の点とエッジを切り出し、内部点は捨てて曲線の性質は残す");

        // 点の集合から切り出すと、その間のエッジだけが付いてくる。
        PathClip pointClip;
        Check(tg::graph::ExtractPathClip(path, {a, b, lone}, {}, pointClip) &&
                  pointClip.points.size() == 3 && pointClip.edges.size() == 1,
              "点の集合の ExtractPathClip は点どうしを結ぶエッジだけを拾う");

        // 貼り付け。ID は振り直され、ずらした位置に同じ形で入る。
        const size_t pointsBefore = path.points.size();
        const size_t edgesBefore = path.edges.size();
        std::vector<PathElementId> pastedPoints;
        std::vector<PathElementId> pastedEdges;
        const bool pasted =
            tg::graph::PastePathClip(path, clip, 0.2f, 0.5f, &pastedPoints, &pastedEdges);
        bool idsFresh = true;
        for (const PathElementId id : pastedPoints) {
            idsFresh &= (id != a && id != b && id != c && id != lone);
        }
        const tg::graph::PathPoint* firstPasted =
            pastedPoints.empty() ? nullptr : path.FindPoint(pastedPoints.front());
        Check(pasted && path.points.size() == pointsBefore + 3 &&
                  path.edges.size() == edgesBefore + 2 && pastedEdges.size() == 2 && idsFresh &&
                  firstPasted != nullptr && std::abs(firstPasted->x - 0.5f) < 1e-5f &&
                  std::abs(firstPasted->z - 0.5f) < 1e-5f &&
                  path.FindEdgeBetween(pastedPoints[0], pastedPoints[1]) != nullptr,
              "PastePathClip は新しい ID で同じ形を、ずらした位置に貼る");
        Check(tg::graph::BuildPathStrands(path).size() == 2,
              "貼った鎖は元の鎖と別の鎖になる");
    }

    Section("パス — 面の線分列");
    {
        using tg::graph::PathElementId;
        using tg::graph::PathSettings;
        // 開いた鎖だけなら面の線分は無い。
        PathSettings open;
        const PathElementId o1 = tg::graph::AddPathPoint(open, 0.2f, 0.2f, 0);
        const PathElementId o2 = tg::graph::AddPathPoint(open, 0.8f, 0.2f, o1);
        tg::graph::AddPathPoint(open, 0.8f, 0.8f, o2);
        Check(tg::graph::BuildPathAreaSegments(open).empty(),
              "開いた鎖だけの BuildPathAreaSegments は空");

        // 三角形の輪。線分は輪を一周して先頭へ戻る。
        PathSettings loop;
        const PathElementId a = tg::graph::AddPathPoint(loop, 0.2f, 0.2f, 0);
        const PathElementId b = tg::graph::AddPathPoint(loop, 0.8f, 0.2f, a);
        const PathElementId c = tg::graph::AddPathPoint(loop, 0.5f, 0.8f, b);
        tg::graph::ConnectPathPoints(loop, c, a);
        const auto segments = tg::graph::BuildPathAreaSegments(loop);
        bool chained = !segments.empty();
        for (size_t i = 0; i + 1 < segments.size(); ++i) {
            chained &= (segments[i].bx == segments[i + 1].ax && segments[i].by == segments[i + 1].ay);
        }
        const bool closed = !segments.empty() && segments.back().bx == segments.front().ax &&
                            segments.back().by == segments.front().ay;
        Check(segments.size() == 3 && chained && closed,
              "閉じた鎖の BuildPathAreaSegments は輪を一周して先頭へ戻る");
    }

    Section("ノードグラフ — Path の入力");
    {
        NodeGraph graph;
        const tg::graph::GraphId surfaceId = graph.CreateNode(NodeKind::Surface);
        const tg::graph::GraphId pathId = graph.CreateNode(NodeKind::Path);
        const tg::graph::Node* surface = graph.FindNode(surfaceId);
        const tg::graph::Node* path = graph.FindNode(pathId);
        Check(IsNeutralPlane(graph.CompileLayersTo(pathId)),
              "Path はレイヤー列を持たず、変位 0 の平面になる");

        // Path の入力は Surface（Mesh 型）。材質（Material 型）は繋げない。
        const bool pathRejects =
            surface != nullptr && path != nullptr && !surface->outputs.empty() &&
            !path->inputs.empty() &&
            !graph.CanCreateLink(surface->outputs.front().id, path->inputs.front().id);
        Check(pathRejects && path->inputs.front().valueType == tg::graph::ValueType::Mesh,
              "Path の Surface 入力は Mesh 型で、材質（Material）は繋げない");
    }
}
