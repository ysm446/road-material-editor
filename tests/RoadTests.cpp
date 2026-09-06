#include "TestSupport.h"
#include "graph/Road.h"
#include "graph/RoadProfile.h"
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
    // 進行方向 +Z に向かって右は -X（右手系 Y-up）。
    Check(road.left.worldSpace && road.left.points.back().y == 2 &&
          road.right.points.back().x == -3 && road.left.points.back().x == 3,
          "boundaries preserve world coordinates, height, and handedness");
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
    std::get<graph::RoadNodeSettings>(graph.FindMutableNode(roadId)->settings).displacementMeters = 0.05f;
    compiled = graph::CompileMeshGraph(graph);
    Check(compiled.scene.meshes.size() == 1 && compiled.scene.meshes[0].displacementMeters == 0.05f,
          "road displacement reaches the scene mesh");
    std::get<graph::RoadNodeSettings>(graph.FindMutableNode(roadId)->settings).displacementMeters = 0.0f;
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
    Check(graph::EvaluateRoad(graph, child, road, error) && road.surface.vertices[0].position.x == 1,
          "left boundary (x = +4) is a usable downstream path");
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

    tests::Section("Lane marking strips");
    graph::RoadGeometry straight;
    Check(graph::BuildRoad(path, settings, straight, error) && straight.stride == 7, "road exposes row stride");
    graph::RoadMarkingNodeSettings marking;
    marking.arrows = false;
    renderer::MeshData lines;
    Check(graph::BuildRoadMarkings(straight, marking, true, lines, error), "default centre and edge lines build");
    if (!error.empty()) std::printf("Marking error: %s\n", error.c_str());
    const size_t rows = straight.surface.vertices.size() / straight.stride;
    Check(lines.vertices.size() == rows * 6 && lines.indices.size() == (rows - 1) * 18,
          "three strips with two vertices per road row");
    if (lines.vertices.size() == rows * 6) {
        Check(std::abs(lines.vertices[0].position.x + 0.075f) < 1e-5f &&
              std::abs(lines.vertices[1].position.x - 0.075f) < 1e-5f, "centre line is 15 cm wide at x = 0");
        Check(std::abs(lines.vertices[rows*2].position.x + 2.575f) < 1e-5f &&
              std::abs(lines.vertices[rows*4+1].position.x - 2.575f) < 1e-5f, "edge lines sit 0.5 m inside the road edge");
        Check(lines.vertices[0].uv.x == 0.0f && lines.vertices[1].uv.x == 1.0f &&
              std::abs(lines.vertices[rows*2-1].uv.y - std::sqrt(104.0f)) < 1e-4f,
              "U spans the strip width and V measures distance");
        Check(lines.vertices[0].position.y > straight.surface.vertices[0].position.y &&
              lines.vertices[0].position.y < 0.01f, "strip is lifted slightly above the surface");
        renderer::MeshScene lineScene;
        lineScene.meshes.push_back({lines, {}});
        Check(renderer::ValidateMeshScene(lineScene), "marking mesh has valid indices and tangents");
    }
    marking.uvRepeatMeters = 2.0f;
    Check(graph::BuildRoadMarkings(straight, marking, true, lines, error) &&
          std::abs(lines.vertices[rows*2-1].uv.y - std::sqrt(104.0f) * 0.5f) < 1e-4f, "UV repeat scales V");
    marking = {};
    marking.arrows = false;
    marking.edgeInsetMeters = 3.0f;
    Check(!graph::BuildRoadMarkings(straight, marking, true, lines, error), "edge line past the centre is rejected");
    marking = {};
    marking.arrows = false;
    marking.centerLine = false;
    marking.edgeLines = false;
    Check(!graph::BuildRoadMarkings(straight, marking, true, lines, error), "no enabled lines is rejected");
    marking = {};
    marking.arrows = false;
    Check(graph::BuildRoad(curve, settings, straight, error) &&
          graph::BuildRoadMarkings(straight, marking, true, lines, error), "curved road markings build");
    {
        renderer::MeshScene curvedScene;
        curvedScene.meshes.push_back({lines, {}});
        Check(renderer::ValidateMeshScene(curvedScene), "curved marking mesh is valid");
    }
    graph::NodeGraph chain;
    const auto chainPath = chain.CreateNode(graph::NodeKind::Path);
    const auto chainRoad = chain.CreateNode(graph::NodeKind::Road);
    const auto chainMarking = chain.CreateNode(graph::NodeKind::RoadMarking);
    const auto chainOut = chain.CreateNode(graph::NodeKind::MeshOutput);
    std::get<graph::PathNodeSettings>(chain.FindMutableNode(chainPath)->settings).path = path;
    chain.CreateLink(chain.FindNode(chainPath)->outputs[0].id, chain.FindNode(chainRoad)->inputs[0].id);
    chain.CreateLink(chain.FindNode(chainMarking)->outputs[0].id, chain.FindNode(chainOut)->inputs[0].id);
    compiled = graph::CompileMeshGraph(chain);
    Check(compiled.active && !compiled.error.empty() && compiled.scene.meshes.empty(),
          "marking without a road reports an error");
    Check(chain.CreateLink(chain.FindNode(chainRoad)->outputs[0].id, chain.FindNode(chainMarking)->inputs[0].id),
          "RoadSurface connects to Lane Marking");
    compiled = graph::CompileMeshGraph(chain);
    Check(compiled.error.empty() && compiled.scene.meshes.size() == 2, "road and markings reach one Mesh Output");
    if (compiled.scene.meshes.size() == 2) {
        Check(compiled.scene.meshes[0].roadGridOverlay && !compiled.scene.meshes[1].roadGridOverlay,
              "grid overlay is only on the road surface");
        Check(compiled.scene.meshes[1].material.baseColor.x > 0.8f && !compiled.scene.meshes[1].materialStack,
              "unconnected marking is white");
    }
    const auto paint = chain.CreateNode(graph::NodeKind::Surface);
    chain.CreateLink(chain.FindNode(paint)->outputs[0].id, chain.FindNode(chainMarking)->inputs[1].id);
    compiled = graph::CompileMeshGraph(chain);
    Check(compiled.scene.meshes.size() == 2 && compiled.scene.meshes[1].materialStack &&
          !compiled.scene.meshes[0].materialStack, "marking material does not leak to the road");

    tests::Section("Vertical curve and bank angle");
    {
        graph::PathSettings profile;
        profile.worldSpace = true;
        auto p0 = graph::AddPathPoint(profile, 0, 0, 0);
        auto p1 = graph::AddPathPoint(profile, 0, 100, p0);
        profile.FindPoint(p1)->y = 10;
        graph::ProfileCurve centerline;
        std::string profileError;
        Check(graph::BuildPathCenterline(profile, centerline, &profileError) &&
              std::abs(centerline.TotalLength() - std::sqrt(100.0f * 100.0f + 100.0f)) < 1e-3f,
              "centerline measures the base curve");
        const auto vid = graph::AddVerticalPoint(profile, 0.5f);
        graph::FindVerticalPoint(profile, vid)->offsetMeters = 4.0f;
        graph::FindVerticalPoint(profile, vid)->vclMeters = 40.0f;
        graph::RoadGeometry profiled;
        Check(graph::BuildRoad(profile, settings, profiled, error), "road with a vertical point builds");
        if (!error.empty()) std::printf("Profile error: %s\n", error.c_str());
        const size_t profiledRows = profiled.surface.vertices.size() / profiled.stride;
        float midY = 0.0f; float quarterY = 0.0f;
        for (size_t row = 0; row < profiledRows; ++row) {
            const auto& v = profiled.surface.vertices[row * profiled.stride];
            if (std::abs(v.position.z - 50.0f) < 0.51f) midY = v.position.y;
            if (std::abs(v.position.z - 25.0f) < 0.51f) quarterY = v.position.y;
        }
        // 放物線は PVI を通らず、(i1 - i2) L / 8 だけ下がる。
        {
            const float total = centerline.TotalLength();
            const float i1 = 9.0f / (total * 0.5f);
            const float i2 = 1.0f / (total * 0.5f);
            const float expectedMid = 9.0f - (i1 - i2) * 40.0f / 8.0f;
            Check(std::abs(midY - expectedMid) < 0.15f, "vertical point raises the profile by its offset minus the parabola drop");
        }
        Check(quarterY > 2.5f + 0.5f, "tangent segments climb toward the raised point");
        Check(std::abs(profiled.surface.vertices.front().position.y) < 1e-4f &&
              std::abs(profiled.surface.vertices[(profiledRows - 1) * profiled.stride].position.y - 10.0f) < 1e-3f,
              "end heights stay at the control points");
        // 曲率が続く円弧状の線形で自動バンクを確認する。
        graph::PathSettings arc;
        arc.worldSpace = true;
        graph::PathElementId arcLast = 0;
        for (int i = 0; i <= 24; ++i) {
            const float angle = static_cast<float>(i) / 24.0f * 1.5707963f;
            arcLast = graph::AddPathPoint(arc, 40.0f * std::sin(angle), -40.0f * std::cos(angle), arcLast);
        }
        arc.bankEnabled = true;
        arc.designSpeedKmh = 60.0f;
        graph::ProfileCurve arcCurve;
        Check(graph::BuildPathCenterline(arc, arcCurve, &profileError), "arc centerline builds");
        const float autoBank = graph::EvaluateBankAngleRadians(arc, arcCurve, arcCurve.TotalLength() * 0.5f);
        const float expected = graph::ComputeAutoBankRadians(40.0f, 60.0f, 0.15f);
        Check(expected > 0.05f && std::abs(std::abs(autoBank) - expected) < 0.05f,
              "auto bank matches the design speed formula for the arc radius");
        // 進行方向 +X から +Z へ曲がる。右手系 Y-up で右は (-dz, 0, dx) = +Z なので右カーブ = 正。
        Check(autoBank > 0.0f, "turning toward the right gives a positive bank");
        graph::RoadGeometry banked;
        Check(graph::BuildRoad(arc, settings, banked, error), "banked road builds");
        bool leftHigher = true;
        for (size_t row = 3; row + 3 < banked.surface.vertices.size() / banked.stride; ++row) {
            const auto& right = banked.surface.vertices[row * banked.stride];
            const auto& left = banked.surface.vertices[row * banked.stride + banked.stride - 1];
            leftHigher &= left.position.y > right.position.y + 0.1f;
        }
        Check(leftHigher, "positive bank raises the outer Left boundary above the inner Right boundary");
        renderer::MeshScene bankedScene;
        bankedScene.meshes.push_back({banked.surface, {}});
        Check(renderer::ValidateMeshScene(bankedScene), "banked mesh is valid");
        renderer::MeshData bankedLines;
        Check(graph::BuildRoadMarkings(banked, graph::RoadMarkingNodeSettings{}, true, bankedLines, error) &&
              bankedLines.vertices[bankedLines.vertices.size() / 2].position.y != 0.0f,
              "markings follow the banked surface");
        arc.bankEnabled = false;
        Check(graph::BuildRoad(arc, settings, banked, error) &&
              std::abs(banked.surface.vertices[5 * banked.stride].position.y -
                       banked.surface.vertices[5 * banked.stride + banked.stride - 1].position.y) < 1e-4f,
              "disabled bank keeps the surface level");
        arc.bankEnabled = true;
        const auto bid = graph::AddBankPoint(arc, 0.5f);
        graph::FindBankPoint(arc, bid)->manual = true;
        graph::FindBankPoint(arc, bid)->angleDegrees = -20.0f;
        const float manual = graph::EvaluateBankAngleRadians(arc, arcCurve, arcCurve.TotalLength() * 0.5f);
        Check(std::abs(manual + 20.0f * 3.14159265f / 180.0f) < 1e-4f, "manual point overrides the angle at its position");
        Check(graph::EvaluateBankAngleRadians(arc, arcCurve, arcCurve.TotalLength() * 0.9f) < 0.0f,
              "manual negative angle holds past the last point");
        Check(std::abs(graph::EvaluateBankAngleRadians(arc, arcCurve, arcCurve.TotalLength() * 0.25f) - manual) < 1e-4f,
              "first manual point holds before its position");
        const auto autoId = graph::AddBankPoint(arc, 0.1f);
        const float between = graph::EvaluateBankAngleRadians(arc, arcCurve, arcCurve.TotalLength() * 0.3f);
        const float atAuto = graph::EvaluateBankAngleRadians(arc, arcCurve, arcCurve.TotalLength() * 0.1f);
        Check(atAuto > 0.0f && between > std::min(atAuto, manual) + 1e-4f && between < std::max(atAuto, manual) - 1e-4f,
              "auto point before a manual point interpolates toward it");
        graph::DeleteProfilePoint(arc, autoId);
        arc.smoothBank = true;
        arc.bankSmoothMeters = 20.0f;
        const float smoothed = graph::EvaluateBankAngleRadians(arc, arcCurve, arcCurve.TotalLength() * 0.5f);
        Check(std::isfinite(smoothed) && smoothed < 0.0f, "smoothing keeps the sign of the manual point");
        const auto frame = graph::EvaluateProfileFrame(arc, arcCurve, 0.5f);
        Check(std::abs(frame.up.y) > 0.9f && std::abs(frame.right.y) < 1e-4f, "profile frame is horizontal before banking");
        Check(graph::DeleteProfilePoint(arc, bid) && !graph::DeleteProfilePoint(arc, bid), "profile points delete once");
    }


    tests::Section("Traffic side and arrows");
    {
        graph::RoadGeometry straightRoad;
        Check(graph::BuildRoad(path, settings, straightRoad, error), "straight road for arrows");
        graph::RoadMarkingNodeSettings arrowsOnly;
        arrowsOnly.centerLine = arrowsOnly.edgeLines = false;
        arrowsOnly.arrows = true;
        arrowsOnly.arrowIntervalMeters = 5.0f;
        arrowsOnly.arrowLengthMeters = 3.0f;
        // 進行方向 +Z。左側通行では左（+X）の車線が +Z へ、右（-X）の車線が -Z へ向く。
        const auto tipDirection = [&](const renderer::MeshData& mesh, bool leftLane) {
            float bestZ = leftLane ? -1e9f : 1e9f;
            float tipX = 0.0f;
            bool any = false;
            for (const auto& vertex : mesh.vertices) {
                const bool onLeft = vertex.position.x > 0.0f;
                if (onLeft != leftLane) continue;
                any = true;
                // 先端は車線の中央（x = ±1.5）にある唯一の頂点。左車線なら最大 z、右車線なら最小 z を見る。
                if (leftLane ? vertex.position.z > bestZ : vertex.position.z < bestZ) {
                    bestZ = vertex.position.z;
                    tipX = vertex.position.x;
                }
            }
            return any && std::abs(std::abs(tipX) - 1.5f) < 1e-3f;
        };
        renderer::MeshData arrows;
        Check(graph::BuildRoadMarkings(straightRoad, arrowsOnly, true, arrows, error) && !arrows.vertices.empty(),
              "arrow markings build");
        if (!error.empty()) std::printf("Arrow error: %s\n", error.c_str());
        Check(arrows.vertices.size() % 7 == 0 && arrows.indices.size() == arrows.vertices.size() / 7 * 9,
              "each arrow is seven vertices and three triangles");
        Check(tipDirection(arrows, true) && tipDirection(arrows, false),
              "left-hand traffic: left lane points forward, right lane points backward");
        renderer::MeshScene arrowScene;
        arrowScene.meshes.push_back({arrows, {}});
        Check(renderer::ValidateMeshScene(arrowScene), "arrow mesh is valid");
        bool upward = true;
        for (size_t i = 0; i < arrows.indices.size(); i += 3) {
            const auto& pa = arrows.vertices[arrows.indices[i]].position;
            const auto& pb = arrows.vertices[arrows.indices[i + 1]].position;
            const auto& pc = arrows.vertices[arrows.indices[i + 2]].position;
            const float ny = (pb.z - pa.z) * (pc.x - pa.x) - (pb.x - pa.x) * (pc.z - pa.z);
            upward &= ny > 0.0f;
        }
        Check(upward, "arrow triangles wind upward in both directions");
        renderer::MeshData rightHand;
        Check(graph::BuildRoadMarkings(straightRoad, arrowsOnly, false, rightHand, error) &&
              !tipDirection(rightHand, true) && !tipDirection(rightHand, false),
              "right-hand traffic flips both lanes");
        graph::NodeGraph network;
        Check(network.RoadNetwork().leftHandTraffic, "new graphs default to left-hand traffic");
        graph::RoadNetworkSettings rhs;
        rhs.leftHandTraffic = false;
        const auto revision = network.Revision();
        network.SetRoadNetwork(rhs);
        Check(!network.RoadNetwork().leftHandTraffic && network.Revision() != revision,
              "changing the traffic side marks the graph dirty");
        DocumentSnapshot trafficBefore;
        trafficBefore.roadNetwork = graph::RoadNetworkSettings{};
        UndoHistory trafficHistory;
        trafficHistory.Push(trafficBefore, 0);
        DocumentSnapshot trafficAfter;
        trafficAfter.roadNetwork = rhs;
        Check(trafficHistory.Undo(trafficAfter).roadNetwork.leftHandTraffic, "undo restores the traffic side");
    }
}
