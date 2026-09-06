#include "TestSupport.h"
#include "io/MeshSceneIo.h"
#include "renderer/AxisProjection.h"

#include <cmath>
#include <limits>

void RunMeshSceneTests() {
    using namespace tg;
    tests::Section("Mesh scene");
    renderer::SceneMesh mesh;
    mesh.geometry.vertices = {
        {{0, 0, 0}, {0, 1, 0}, {1, 0, 0, -1}, {0, 0}},
        {{0, 0, 4}, {0, 1, 0}, {1, 0, 0, -1}, {0, 1}},
        {{3, 0, 4}, {0, 1, 0}, {1, 0, 0, -1}, {1, 1}}};
    mesh.geometry.indices = {0, 1, 2};
    renderer::MeshScene scene{{mesh, mesh}};
    scene.meshes[1].material.metallic = 1.0f;
    tests::Check(renderer::ValidateMeshScene(scene), "Multiple meshes are valid");
    tests::Check(std::abs(renderer::MeshSceneRadius(scene) - 5.0f) < 0.0001f,
                 "Radius includes world coordinates");
    const auto json = io::WriteMeshScene(scene);
    renderer::MeshScene loaded;
    tests::Check(io::ReadMeshScene(json, loaded) && io::WriteMeshScene(loaded) == json,
                 "Geometry and independent materials survive roundtrip");
    auto invalid = json;
    invalid["meshes"][0]["indices"][0] = 99;
    tests::Check(!io::ReadMeshScene(invalid, loaded) && io::WriteMeshScene(loaded) == json,
                 "Invalid index rejected without changing output");
    invalid = json;
    invalid["meshes"][0]["vertices"][0][0] = 1e100;
    tests::Check(!io::ReadMeshScene(invalid, loaded), "Float overflow rejected");
    invalid = json;
    invalid["meshes"][0]["indices"][0] = -1;
    tests::Check(!io::ReadMeshScene(invalid, loaded), "Negative index rejected");
    invalid = json;
    invalid["meshes"][0]["indices"][0] = 0.5;
    tests::Check(!io::ReadMeshScene(invalid, loaded), "Fractional index rejected");
    invalid = json;
    invalid["meshes"][0]["material"][3] = "rough";
    tests::Check(!io::ReadMeshScene(invalid, loaded), "Wrong material type rejected");
    scene.meshes[0].geometry.vertices[0].normal = {0, 0, 0};
    tests::Check(!renderer::ValidateMeshScene(scene), "Zero normal rejected");
    scene.meshes[0] = mesh;
    scene.meshes[0].geometry.vertices[0].position.x = std::numeric_limits<float>::quiet_NaN();
    tests::Check(!renderer::ValidateMeshScene(scene), "Nonfinite position rejected");
    tests::Check(io::ReadMeshScene({{"meshes", nlohmann::json::array()}}, loaded) &&
                 loaded.meshes.empty(), "Empty scene is distinct from legacy terrain");

    tests::Section("Move axis projection");
    using namespace DirectX;
    const auto projection = XMMatrixPerspectiveFovRH(0.8f, 1.5f, 0.01f, 100.0f);
    const XMFLOAT3 center{0,0,0};
    for (float yaw : {0.001f, 0.5f, 1.57f, 3.14f}) {
        for (float pitch : {0.001f, 0.5f, 1.55f}) {
            const XMVECTOR eye = XMVectorSet(10*std::cos(pitch)*std::sin(yaw),
                10*std::sin(pitch), 10*std::cos(pitch)*std::cos(yaw), 1);
            const auto vp = XMMatrixLookAtRH(eye, XMVectorZero(), XMVectorSet(0,1,0,0))*projection;
            const auto axes = renderer::ProjectMoveAxes(vp, center, 900, 600, 64);
            for (int i = 0; i < 3; ++i) {
                const auto delta = axes.delta[i];
                tests::Check(std::isfinite(delta.x) && std::isfinite(delta.y) &&
                             std::hypot(delta.x,delta.y) <= 64.001f, "axes remain finite and bounded");
                const XMVECTOR step = XMVectorSet(i==0 ? 0.001f:0, i==2 ? 0.001f:0, i==1 ? 0.001f:0, 1);
                const XMVECTOR screen = XMVector3TransformCoord(step,vp);
                const float dx=XMVectorGetX(screen)*450, dy=-XMVectorGetY(screen)*300;
                tests::Check(delta.x*dx+delta.y*dy >= -1e-5f, "positive world axis keeps projected direction");
            }
        }
    }
    const auto view = XMMatrixLookAtRH(XMVectorSet(0,0,0.5f,1), XMVectorZero(), XMVectorSet(0,1,0,0));
    const auto closeAxes = renderer::ProjectMoveAxes(view*projection, {0,0,0},900,600,64);
    tests::Check(std::hypot(closeAxes.delta[1].x,closeAxes.delta[1].y) < 0.001f,
                 "view-aligned axis is not stretched to full length");
    tests::Check(closeAxes.delta[0].x > 63 && closeAxes.delta[2].y < -63,
                 "near camera plane preserves X and Y orientation");
    const auto behind = renderer::ProjectMoveAxes(view*projection, {0,0,1},900,600,64);
    tests::Check(behind.pixelsPerMeter[0] == 0, "axes behind camera are rejected");
}
