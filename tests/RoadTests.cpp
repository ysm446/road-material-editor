#include "TestSupport.h"
#include "graph/Road.h"
#include "app/UndoHistory.h"
#include <cmath>

void RunRoadTests() {
    using namespace tg;
    using tests::Check;
    tests::Section("Road geometry and graph");
    graph::PathSettings path;
    path.worldSpace = true;
    auto a = graph::AddPathPoint(path, 0, 0, 0);
    auto b = graph::AddPathPoint(path, 0, 10, a);
    path.FindPoint(b)->y = 2;
    graph::RoadNodeSettings settings;
    graph::RoadGeometry road;
    std::string error;
    Check(graph::BuildRoad(path, settings, road, error), "sloped road builds");
    Check(road.surface.vertices.size() == 84 && road.surface.indices.size() == 396, "one metre rows and columns");
    if (road.surface.vertices.size() != 84) return;
    Check(std::abs(road.surface.vertices[0].position.x + 3) < 1e-5f &&
          std::abs(road.surface.vertices[6].position.x - 3) < 1e-5f, "six metre width and left/right orientation");
    Check(std::abs(road.surface.vertices.back().uv.y - std::sqrt(104.0f)) < 1e-4f,
          "longitudinal UV measures 3D distance");
    renderer::MeshScene scene;
    scene.meshes.push_back({road.surface,{}});
    Check(renderer::ValidateMeshScene(scene), "finite orthonormal mesh with valid indices");
    Check(road.left.worldSpace && road.left.points.back().y == 2 &&
          road.right.points.back().x == 3, "boundaries preserve world coordinates and height");
    bool metreCells = true;
    for (size_t i = 0; i < road.surface.vertices.size(); ++i) {
        if (i % 7 != 6) metreCells &= std::abs(road.surface.vertices[i+1].position.x-road.surface.vertices[i].position.x) <= 1.001f;
        if (i + 7 < road.surface.vertices.size()) metreCells &= road.surface.vertices[i+7].uv.y-road.surface.vertices[i].uv.y <= 1.001f;
    }
    Check(metreCells, "straight cells are at most one metre in both axes");
    const float endUv = road.surface.vertices.back().uv.y;
    graph::PathSettings dense;
    dense.worldSpace = true;
    auto first = graph::AddPathPoint(dense, 0, 0, 0);
    auto mid = graph::AddPathPoint(dense, 0, 5, first);
    dense.FindPoint(mid)->y = 1;
    auto last = graph::AddPathPoint(dense, 0, 10, mid);
    dense.FindPoint(last)->y = 2;
    Check(graph::BuildRoad(dense, settings, road, error) &&
          std::abs(road.surface.vertices.back().uv.y - endUv) < 1e-4f,
          "collinear point density does not change UV scale");
    graph::AddPathPoint(dense, 5, 5, mid);
    Check(!graph::BuildRoad(dense, settings, road, error), "branch is rejected");
    path.FindPoint(b)->z = 0;
    Check(!graph::BuildRoad(path, settings, road, error), "vertical section is rejected");
    path.FindPoint(b)->z = 10;
    path.worldSpace = false;
    Check(!graph::BuildRoad(path, settings, road, error), "legacy UV path requires conversion");
    path.worldSpace = true;
    graph::PathSettings curve;
    curve.worldSpace = true;
    graph::PathElementId prev = 0;
    const float positions[][3] = {{-6,0,-20},{-6,1,-10},{6,2,0},{6,3,16}};
    for (auto& pos : positions) {
        prev = graph::AddPathPoint(curve, pos[0], pos[2], prev);
        curve.FindPoint(prev)->y = pos[1];
    }
    for (auto& edge : curve.edges) edge.curve = graph::PathCurve::Cubic;
    Check(graph::BuildRoad(curve, settings, road, error), "gentle cubic road builds");
    if (!error.empty()) std::printf("Road error: %s\n", error.c_str());
    auto corner = path;
    graph::AddPathPoint(corner, 10, 10, b);
    Check(graph::BuildRoad(corner, settings, road, error), "subdivision preserves a right angle miter");
    auto narrow = settings;
    narrow.widthMeters = 2.5f;
    Check(graph::BuildRoad(path,narrow,road,error) && std::abs(road.surface.vertices[3].position.x-1.25f)<1e-5f,
          "fractional width is preserved with sub-metre columns");
    graph::NodeGraph graph;
    auto pathId = graph.CreateNode(graph::NodeKind::Path);
    auto roadId = graph.CreateNode(graph::NodeKind::Road);
    auto outId = graph.CreateNode(graph::NodeKind::MeshOutput);
    std::get<graph::PathNodeSettings>(graph.FindMutableNode(pathId)->settings).path = path;
    Check(graph.CreateLink(graph.FindNode(pathId)->outputs[0].id, graph.FindNode(roadId)->inputs[0].id), "Path connects to Road");
    Check(!graph.CanCreateLink(graph.FindNode(pathId)->outputs[0].id, graph.FindNode(outId)->inputs[0].id), "Path cannot connect to Mesh input");
    Check(graph.CreateLink(graph.FindNode(roadId)->outputs[0].id, graph.FindNode(outId)->inputs[0].id), "RoadSurface connects to Mesh Output");
    auto compiled = graph::CompileMeshGraph(graph);
    Check(compiled.active && compiled.error.empty() && compiled.scene.meshes.size() == 1, "mesh graph compiles road output");
    Check(!compiled.scene.meshes[0].materialStack, "unconnected road keeps constant material");
    const auto surfaceId = graph.CreateNode(graph::NodeKind::Surface);
    auto& surface = std::get<graph::LayerNodeSettings>(graph.FindMutableNode(surfaceId)->settings);
    surface.layer.roughness = 0.23f;
    Check(graph.CreateLink(graph.FindNode(surfaceId)->outputs[0].id, graph.FindNode(roadId)->inputs[1].id),
          "material output connects to road material");
    Check(!graph.CanCreateLink(graph.FindNode(pathId)->outputs[0].id, graph.FindNode(roadId)->inputs[1].id),
          "path cannot connect to material input");
    compiled = graph::CompileMeshGraph(graph);
    Check(compiled.scene.meshes[0].materialStack &&
          compiled.scene.meshes[0].materialStack->Layers().back().roughness == 0.23f &&
          compiled.scene.meshes[0].materialStack->SizeMeters() == 1.0f,
          "road compiles connected material at UV tile scale");
    DocumentSnapshot before;
    before.graphNodes = graph.Nodes(); before.graphLinks = graph.Links();
    UndoHistory history;
    history.Push(before, 0);
    std::get<graph::RoadNodeSettings>(graph.FindMutableNode(roadId)->settings).widthMeters = 8;
    compiled = graph::CompileMeshGraph(graph);
    Check(compiled.scene.meshes.size() == 1 && compiled.scene.meshes[0].geometry.vertices[8].position.x == 4,
          "width changes regenerate geometry");
    DocumentSnapshot after;
    after.graphNodes = graph.Nodes(); after.graphLinks = graph.Links();
    auto restored = history.Undo(after);
    graph.Replace(restored.graphNodes, restored.graphLinks);
    compiled = graph::CompileMeshGraph(graph);
    Check(compiled.scene.meshes[0].geometry.vertices[6].position.x == 3, "undo regenerates original width");
    restored = history.Redo(before);
    graph.Replace(restored.graphNodes, restored.graphLinks);
    Check(std::get<graph::RoadNodeSettings>(graph.FindNode(roadId)->settings).widthMeters == 8, "redo restores road settings");
    auto child = graph.CreateNode(graph::NodeKind::Road);
    graph.CreateLink(graph.FindNode(roadId)->outputs[1].id, graph.FindNode(child)->inputs[0].id);
    Check(graph::EvaluateRoad(graph, child, road, error) && road.surface.vertices[0].position.x == -7,
          "left boundary is a usable downstream path");
    Check(!graph.CanCreateLink(graph.FindNode(child)->outputs[1].id, graph.FindNode(roadId)->inputs[0].id), "road dependency cycle is rejected");
    const auto childOut = graph.CreateNode(graph::NodeKind::MeshOutput);
    graph.CreateLink(graph.FindNode(child)->outputs[0].id, graph.FindNode(childOut)->inputs[0].id);
    compiled = graph::CompileMeshGraph(graph);
    Check(compiled.scene.meshes.size() == 2 && compiled.scene.meshes[0].materialStack &&
          !compiled.scene.meshes[1].materialStack, "separate roads do not inherit each others material");
    graph::GraphId materialLink = 0;
    for (const auto& link : graph.Links())
        if (link.endPin == graph.FindNode(roadId)->inputs[1].id) materialLink = link.id;
    graph.DeleteLink(materialLink);
    compiled = graph::CompileMeshGraph(graph);
    Check(!compiled.scene.meshes[0].materialStack, "disconnect restores constant material");
}
