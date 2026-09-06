#include "TestSupport.h"
#include "io/MeshSceneIo.h"

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
}
