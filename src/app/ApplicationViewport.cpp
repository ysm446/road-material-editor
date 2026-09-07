// ビューポートパネルと、その上の入力（軌道 / ライトドラッグ / パス編集 / 選択）、
// 重ねて描くギズモ類。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "core/FileDialog.h"
#include "core/Log.h"
#include "io/ProjectIo.h"
#include "ui/UiStyle.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace tg {

void Application::HandleMeshSelection(bool hovered, const ImVec2& viewportMin,
                                      const ImVec2& viewportMax) {
    auto& state = m_meshSelection;
    const auto& meshes = m_renderer.Scene().meshes;
    std::erase_if(state.selected, [&](size_t index) { return index >= meshes.size(); });
    const auto& io = ImGui::GetIO();
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        state.pending = true;
        state.dragging = false;
        state.additive = io.KeyShift;
        state.start = state.end = io.MousePos;
        state.previous = state.selected;
    }
    if (state.pending && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        state.selected = state.previous;
        state.pending = state.dragging = false;
    }
    if (state.pending) {
        state.end = {std::clamp(io.MousePos.x, viewportMin.x, viewportMax.x),
                     std::clamp(io.MousePos.y, viewportMin.y, viewportMax.y)};
        state.dragging |= ImGui::IsMouseDragging(ImGuiMouseButton_Left, ui::Scaled(4.0f));
        if (state.dragging) state.selected = state.additive ? state.previous : std::vector<size_t>{};
    }
    const ImVec2 lo(std::min(state.start.x, state.end.x), std::min(state.start.y, state.end.y));
    const ImVec2 hi(std::max(state.start.x, state.end.x), std::max(state.start.y, state.end.y));
    const auto& camera = m_renderer.GetCamera();
    const auto viewProjection = camera.ViewMatrix() * camera.ProjectionMatrix();
    const ImVec2 size(viewportMax.x - viewportMin.x, viewportMax.y - viewportMin.y);
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(viewportMin, viewportMax, true);
    for (size_t index = 0; index < meshes.size(); ++index) {
        ImVec2 min{};
        ImVec2 max{};
        bool valid = false;
        for (const auto& vertex : meshes[index].geometry.vertices) {
            const auto point = ProjectToViewport(viewProjection, vertex.position, viewportMin, size);
            if (!point.visible) continue;
            if (!valid) { min = max = point.screen; valid = true; }
            min.x = std::min(min.x, point.screen.x);
            min.y = std::min(min.y, point.screen.y);
            max.x = std::max(max.x, point.screen.x);
            max.y = std::max(max.y, point.screen.y);
        }
        if (!valid) continue;
        // メッシュは投影した境界矩形と選択枠の交差で選ぶ。点の選択とは区別する。
        if (state.pending && state.dragging && min.x <= hi.x && max.x >= lo.x &&
            min.y <= hi.y && max.y >= lo.y &&
            std::find(state.selected.begin(), state.selected.end(), index) == state.selected.end()) {
            state.selected.push_back(index);
        }
        if (std::find(state.selected.begin(), state.selected.end(), index) != state.selected.end()) {
            draw->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_PlotLinesHovered), 0.0f, 0, ui::Scaled(2.0f));
        }
    }
    if (state.pending && state.dragging) {
        draw->AddRectFilled(lo, hi, ImGui::GetColorU32(ImGuiCol_TextSelectedBg, 0.4f));
        draw->AddRect(lo, hi, ImGui::GetColorU32(ImGuiCol_PlotLinesHovered));
    }
    if (state.pending && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (!state.dragging && !state.additive) state.selected.clear();
        state.pending = state.dragging = false;
    }
    if (!state.selected.empty()) {
        char text[64];
        std::snprintf(text, sizeof(text), "選択: %zu メッシュ", state.selected.size());
        draw->AddText(ImVec2(viewportMin.x + ui::Scaled(12.0f), viewportMin.y + ui::Scaled(50.0f)),
                      ImGui::GetColorU32(ImGuiCol_Text), text);
    }
    draw->PopClipRect();
}

// 3 桁ごとに区切る。**桁数の多い数はそのままだと読めない。**
std::string GroupDigits(uint64_t value) {
    std::string digits = std::to_string(value);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) {
        digits.insert(static_cast<size_t>(i), ",");
    }
    return digits;
}

// ビューポートに重ねる操作。表示モードの切り替えと、重ねる情報の切り替え。
//
// トップメニューではなくビューポートの中に置く。見ている場所から目を離さずに
// 切り替えられ、いまどの表示なのかも常に見える。
void Application::DrawViewportOverlay(const ImVec2& viewportMin, const ImVec2& viewportMax) {
    const float margin = ui::Scaled(10.0f);
    ImGui::SetCursorScreenPos(ImVec2(viewportMin.x + margin, viewportMin.y + margin));

    renderer::DebugView& current = m_renderer.Debug();
    const char* label = kDebugViewLabels[static_cast<size_t>(current)];

    // 既定以外の表示は見落としやすいので、ボタンの文字を強調する。
    const bool highlighted = (current != renderer::DebugView::Shaded);
    if (highlighted) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WarnColor());
    }
    if (ImGui::Button(label)) {
        ImGui::OpenPopup("##viewportViewMenu");
    }
    if (highlighted) {
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("ビューポートに何を表示するか");
    }

    if (ImGui::BeginPopup("##viewportViewMenu")) {
        for (int i = 0; i < IM_ARRAYSIZE(kDebugViewLabels); ++i) {
            const auto view = static_cast<renderer::DebugView>(i);
            if (ImGui::Selectable(kDebugViewLabels[i], current == view)) {
                current = view;
            }
        }
        ImGui::EndPopup();
    }

    // --- 重ねる情報の切り替え ------------------------------------------------
    // FPS / 統計 / グリッド。どれもビューポートに重ねて出すものなので、
    // トップメニューではなくここに置く。切り替えたその場で設定に覚える。
    ImGui::SameLine();
    if (ImGui::Button("表示")) {
        ImGui::OpenPopup("##viewportDisplayMenu");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("ビューポートに重ねる情報");
    }
    if (ImGui::BeginPopup("##viewportDisplayMenu")) {
        io::DisplaySettings& settings = m_settings.Display();
        bool changed = false;
        changed |= ImGui::MenuItem("FPS", nullptr, &settings.showFps);
        changed |= ImGui::MenuItem("統計", nullptr, &settings.showStats);
        changed |= ImGui::MenuItem("グリッド（50 m × 50 m / 1 m間隔）", nullptr, &settings.showReferenceGrid);
        changed |= ImGui::MenuItem("道路の1 mグリッド", nullptr, &settings.showRoadGrid);
        changed |= ImGui::MenuItem("UVチェッカー", nullptr, &settings.showUvChecker);
        changed |= ImGui::MenuItem("ワイヤーフレーム（分割後）", nullptr, &settings.showWireframe);
        if (changed) {
            m_settings.Save();
        }
        ImGui::EndPopup();
    }

    // --- FPS と描画の量 ------------------------------------------------------
    // **右上へ置く。** 左上は表示モードの切り替えとライトの数値で埋まっている。
    // ボタンではなく描き込みにする。押すものではないので、枠を持たせない。
    const io::DisplaySettings& display = m_settings.Display();
    if (!display.showFps && !display.showStats) {
        return;
    }

    // 行を組み立ててから 1 つの下地にまとめて描く。**枠を 2 つ並べない。**
    // FPS と統計で別々の箱にすると、片方だけ出したときに位置が揃わない。
    std::vector<std::string> lines;
    if (display.showFps) {
        // 1 桁台では小数まで出す。整数だけだと 0.8fps が「0 FPS」になり、
        // 止まっているのか極端に遅いのかが読めない。
        const float framerate = ImGui::GetIO().Framerate;
        char text[32] = {};
        std::snprintf(text, sizeof(text), (framerate < 10.0f) ? "%.1f FPS" : "%.0f FPS",
                      framerate);
        lines.emplace_back(text);
    }
    if (display.showStats) {
        const renderer::RenderStats& stats = m_renderer.Stats();
        char text[96] = {};
        std::snprintf(text, sizeof(text), "ドローコール %u", stats.drawCalls);
        lines.emplace_back(text);
        std::snprintf(text, sizeof(text), "頂点 %s", GroupDigits(stats.vertices).c_str());
        lines.emplace_back(text);
        // テセレーション中は、三角形はドメインシェーダが決めるので CPU では分からない。
        // **数えられないものを数えたふりをしない。** 投入したパッチ数と上限を出す。
        if (stats.tessellation) {
            std::snprintf(text, sizeof(text), "パッチ %s (x%.0f まで)",
                          GroupDigits(stats.patches).c_str(), stats.tessellationFactor);
        } else {
            std::snprintf(text, sizeof(text), "三角形 %s", GroupDigits(stats.triangles).c_str());
        }
        lines.emplace_back(text);
        // VRAM はプロセス全体の使用量とバジェット。合成の解像度を上げたときに
        // どれだけ余裕が残っているかを、その場で見えるようにする。
        const rhi::Device::VideoMemory vram = m_device.QueryVideoMemory();
        constexpr double kMegaBytes = 1024.0 * 1024.0;
        std::snprintf(text, sizeof(text), "VRAM %.0f / %.0f MB",
                      static_cast<double>(vram.usage) / kMegaBytes,
                      static_cast<double>(vram.budget) / kMegaBytes);
        lines.emplace_back(text);
    }

    const ImVec2 padding(ui::Scaled(8.0f), ui::Scaled(4.0f));
    const float lineHeight = ImGui::GetTextLineHeight();
    const float spacing = ImGui::GetStyle().ItemSpacing.y * 0.5f;
    float widest = 0.0f;
    for (const std::string& line : lines) {
        widest = std::max(widest, ImGui::CalcTextSize(line.c_str()).x);
    }
    const float height = lineHeight * static_cast<float>(lines.size()) +
                         spacing * static_cast<float>(lines.size() - 1);

    const ImVec2 boxMax(viewportMax.x - margin,
                        viewportMin.y + margin + height + padding.y * 2.0f);
    const ImVec2 boxMin(boxMax.x - widest - padding.x * 2.0f, viewportMin.y + margin);

    // 明るい素材の上でも読めるように、暗い下地を敷く。
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(boxMin, boxMax, IM_COL32(8, 10, 12, 190), ui::Scaled(4.0f));

    float y = boxMin.y + padding.y;
    for (const std::string& line : lines) {
        // **右へ揃える。** 桁数が変わるたびに数字の頭が動くと、目で追えない。
        const float width = ImGui::CalcTextSize(line.c_str()).x;
        drawList->AddText(ImVec2(boxMax.x - padding.x - width, y),
                          IM_COL32(235, 235, 235, 255), line.c_str());
        y += lineHeight + spacing;
    }
}

// L + 左ドラッグでライトの向きを変える。
//
// 修飾キー（Ctrl / Shift / Alt）は付けない。Alt は軌道、Ctrl は数値の直接入力に
// 使っているので、それらと重ならないようにする。
bool Application::HandleLightDrag(bool itemActive) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool shortcut =
        ImGui::IsKeyDown(ImGuiKey_L) && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt;
    if (!shortcut || !itemActive || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        m_lightDragActive = false;
        return false;
    }

    m_lightDragActive = true;
    m_lightGizmoUntil = ImGui::GetTime() + kLightGizmoFadeSeconds;

    renderer::LightSettings& light = m_renderer.Light();
    const float step = DegreesToRadians(kLightDegreesPerPixel);
    // 方位角は一周させる。仰角は UI のスライダーと同じ範囲に収める。
    light.azimuth = WrapAngle(light.azimuth + io.MouseDelta.x * step);
    // 真下からの光も見たいので、下は -89 度まで許す。
    light.elevation = std::clamp(light.elevation - io.MouseDelta.y * step,
                                 DegreesToRadians(-89.0f), DegreesToRadians(89.0f));
    return true;
}

// F でメッシュを画面の中心へ戻し、A でさらに全体が収まる距離まで引く。
// DCC の「選択をフレーム / 全体をフレーム」に倣った割り当て。
//
// 修飾キーは付けない（Ctrl は数値の直接入力、Alt は軌道に使っている）。
// カーソルがビューポートの上にあるときだけ効かせ、
// **テキスト入力中は無視する**。レイヤー名を打っている最中に視点が飛ぶのを防ぐ。
void Application::HandleCameraShortcuts(bool itemHovered) {
    const ImGuiIO& io = ImGui::GetIO();
    if (!itemHovered || io.WantTextInput || io.KeyCtrl || io.KeyShift || io.KeyAlt) {
        return;
    }

    // プレビューのメッシュはどれも原点中心（モデル行列は単位行列）。
    constexpr DirectX::XMFLOAT3 kMeshCenter{0.0f, 0.0f, 0.0f};
    renderer::Camera& camera = m_renderer.GetCamera();

    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        camera.Focus(kMeshCenter);
    } else if (ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        camera.Frame(kMeshCenter, m_settings.Display().showReferenceGrid
            ? std::max(m_renderer.BoundingRadius(), renderer::PreviewRenderer::kReferenceGridRadius)
            : m_renderer.BoundingRadius());
    }
}

// ライトの向きを示すギズモ。地面のリング、水平方向、仰角の弧、光が来る向きの矢印。
//
// 色はテーマから引かない。座標軸ギズモと同じく「意味を持つ色」として固定する。
void Application::DrawLightGizmo(const ImVec2& viewportMin, const ImVec2& viewportMax) {
    const double now = ImGui::GetTime();
    if (!m_lightDragActive && now >= m_lightGizmoUntil) {
        return;
    }
    const float fade =
        m_lightDragActive
            ? 1.0f
            : static_cast<float>(std::clamp((m_lightGizmoUntil - now) / kLightGizmoFadeSeconds,
                                            0.0, 1.0));
    if (fade <= 0.001f) {
        return;
    }

    using namespace DirectX;
    const renderer::Camera& camera = m_renderer.GetCamera();
    const XMMATRIX viewProjection = camera.ViewMatrix() * camera.ProjectionMatrix();
    const ImVec2 size(viewportMax.x - viewportMin.x, viewportMax.y - viewportMin.y);
    if (size.x <= 0.0f || size.y <= 0.0f) {
        return;
    }

    const renderer::LightSettings& light = m_renderer.Light();
    const XMFLOAT3 direction = light.Direction();
    // **カメラの注視点に、画面へ収まる大きさで置く。** 原点固定・実寸固定だと、
    // パンやズームで注視点を移した先で見えなくなったり、画面からはみ出したりする。
    // 半径は注視点までの距離と縦画角から決め、リングと矢印が縦の視野の中に収まる比にする。
    const XMFLOAT3 origin = camera.Target();
    const XMFLOAT3 eye = camera.Position();
    const float distance = std::sqrt((eye.x - origin.x) * (eye.x - origin.x) +
                                     (eye.y - origin.y) * (eye.y - origin.y) +
                                     (eye.z - origin.z) * (eye.z - origin.z));
    const float gizmoRadius = std::max(distance * std::tan(camera.FovY() * 0.5f) * 0.45f, 1e-3f);
    const XMFLOAT3 horizontal{std::sin(light.azimuth), 0.0f, std::cos(light.azimuth)};

    const auto color = [fade](int r, int g, int b, int a) {
        return IM_COL32(r, g, b, static_cast<int>(static_cast<float>(a) * fade));
    };
    const auto offset = [](const XMFLOAT3& base, const XMFLOAT3& dir, float amount) {
        return XMFLOAT3{base.x + dir.x * amount, base.y + dir.y * amount,
                        base.z + dir.z * amount};
    };
    const auto project = [&](const XMFLOAT3& world) {
        return ProjectToViewport(viewProjection, world, viewportMin, size);
    };

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(viewportMin, viewportMax, true);

    const auto drawWorldLine = [&](const XMFLOAT3& a, const XMFLOAT3& b, ImU32 lineColor,
                                   float thickness) {
        const ProjectedPoint pa = project(a);
        const ProjectedPoint pb = project(b);
        if (pa.visible && pb.visible) {
            drawList->AddLine(pa.screen, pb.screen, lineColor, thickness);
        }
    };

    // 地面のリング。方位角の目安になる。
    constexpr int kRingSegments = 72;
    ProjectedPoint previous;
    for (int i = 0; i <= kRingSegments; ++i) {
        const float t = (static_cast<float>(i) / kRingSegments) * 2.0f * 3.14159265f;
        const ProjectedPoint current = project(XMFLOAT3{
            origin.x + std::sin(t) * gizmoRadius, origin.y, origin.z + std::cos(t) * gizmoRadius});
        if (i > 0 && previous.visible && current.visible) {
            drawList->AddLine(previous.screen, current.screen, color(150, 160, 175, 130), 1.6f);
        }
        previous = current;
    }

    // 水平方向への投影と、そこから仰角ぶんの弧。
    drawWorldLine(origin, offset(origin, horizontal, gizmoRadius), color(150, 160, 175, 170), 1.8f);

    constexpr int kArcSegments = 32;
    ProjectedPoint previousArc;
    for (int i = 0; i <= kArcSegments; ++i) {
        const float angle = light.elevation * (static_cast<float>(i) / kArcSegments);
        const float ring = std::cos(angle) * gizmoRadius;
        const ProjectedPoint current = project(XMFLOAT3{origin.x + horizontal.x * ring,
                                                        origin.y + std::sin(angle) * gizmoRadius,
                                                        origin.z + horizontal.z * ring});
        if (i > 0 && previousArc.visible && current.visible) {
            drawList->AddLine(previousArc.screen, current.screen, color(255, 206, 112, 150), 1.6f);
        }
        previousArc = current;
    }

    // 光が来る向きの矢印。ライトの位置から原点へ向ける。
    const ProjectedPoint arrowStart = project(offset(origin, direction, gizmoRadius));
    const ProjectedPoint arrowEnd = project(offset(origin, direction, gizmoRadius * 0.22f));
    if (arrowStart.visible && arrowEnd.visible) {
        const ImU32 lightColor = color(255, 188, 76, 245);
        ImVec2 screenDir(arrowEnd.screen.x - arrowStart.screen.x,
                         arrowEnd.screen.y - arrowStart.screen.y);
        const float length = std::sqrt(screenDir.x * screenDir.x + screenDir.y * screenDir.y);
        if (length > 0.001f) {
            screenDir.x /= length;
            screenDir.y /= length;
            const ImVec2 side(-screenDir.y, screenDir.x);
            const float head = ui::Scaled(14.0f);
            const float halfWidth = ui::Scaled(6.0f);
            const ImVec2 base(arrowEnd.screen.x - screenDir.x * head,
                              arrowEnd.screen.y - screenDir.y * head);
            drawList->AddLine(arrowStart.screen, base, lightColor, ui::Scaled(3.5f));
            drawList->AddTriangleFilled(
                arrowEnd.screen, ImVec2(base.x + side.x * halfWidth, base.y + side.y * halfWidth),
                ImVec2(base.x - side.x * halfWidth, base.y - side.y * halfWidth), lightColor);
        }
    }

    if (const ProjectedPoint center = project(origin); center.visible) {
        drawList->AddCircle(center.screen, ui::Scaled(5.0f), color(200, 210, 220, 200), 20, 1.6f);
    }

    // いまの値。掴んだまま数字を確かめられるようにする。
    char text[64] = {};
    std::snprintf(text, sizeof(text), "方位角 %.0f 度   仰角 %.0f 度",
                  RadiansToDegrees(light.azimuth), RadiansToDegrees(light.elevation));
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const ImVec2 padding(ui::Scaled(8.0f), ui::Scaled(5.0f));
    // 左上には表示モードのボタンがあるので、その下へ置く。
    const ImVec2 textMin(viewportMin.x + ui::Scaled(10.0f),
                         viewportMin.y + ui::Scaled(10.0f) + ImGui::GetFrameHeight() +
                             ui::Scaled(6.0f));
    const ImVec2 textMax(textMin.x + textSize.x + padding.x * 2.0f,
                         textMin.y + textSize.y + padding.y * 2.0f);
    drawList->AddRectFilled(textMin, textMax, color(8, 10, 12, 190), ui::Scaled(4.0f));
    drawList->AddText(ImVec2(textMin.x + padding.x, textMin.y + padding.y),
                      color(235, 235, 235, 255), text);

    drawList->PopClipRect();
}

void Application::DrawViewportPanel() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    // ホイールでウィンドウがスクロールしないようにする（ズームに使うため）。
    const bool open = ImGui::Begin("ビューポート", nullptr,
                                   ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    if (open) {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        // 8 の倍数に丸め、ドラッグ中の作り直しを減らす。
        const auto snap = [](float value) {
            const int clamped = std::clamp(static_cast<int>(value), 64, 4096);
            return static_cast<uint32_t>((clamped / 8) * 8);
        };
        m_requestedViewportWidth = snap(available.x);
        m_requestedViewportHeight = snap(available.y);

        if (m_renderer.HasOutput()) {
            // テクスチャの実サイズではなくコンテンツ領域に合わせて描く。
            // 実サイズで描くとパネルからはみ出し、スクロールバーの出入りで
            // 要求サイズが振動してしまう。作り直しは 1 フレーム遅れる。
            const ImVec2 imageOrigin = ImGui::GetCursorScreenPos();
            ImGui::Image(static_cast<ImTextureID>(m_renderer.OutputHandle().ptr), available);

            // ImGui::Image は入力を消費しないため、そのままだと画像上のドラッグが
            // 「ウィンドウの余白のドラッグ」と解釈されてパネルごと動いてしまう。
            // 同じ矩形に不可視ボタンを重ねてドラッグを受け止める。
            ImGui::SetCursorScreenPos(imageOrigin);
            // ビューポート内に重ねるボタン（表示モード）へ入力を譲る。
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("##viewportInput", available,
                                   ImGuiButtonFlags_MouseButtonLeft |
                                       ImGuiButtonFlags_MouseButtonMiddle |
                                       ImGuiButtonFlags_MouseButtonRight);

            const ImGuiIO& io = ImGui::GetIO();
            renderer::Camera& camera = m_renderer.GetCamera();
            const bool itemActive = ImGui::IsItemActive();
            const bool itemHovered = ImGui::IsItemHovered();

            // L + 左ドラッグはライトの向き。軌道より先に見る。
            const bool lightDragging = HandleLightDrag(itemActive);

            // Path ノードを選んでいる間は、左クリック / ドラッグと右クリックがパスの編集。
            // 視点は Alt を押している間だけ動く。
            const ImVec2 imageMax(imageOrigin.x + available.x, imageOrigin.y + available.y);
            graph::Node* pathNode = CurrentPathNode();
            const bool pathEnabled = (pathNode != nullptr) && !lightDragging;
            if (pathEnabled && !io.KeyAlt) {
                HandlePathInput(*pathNode, itemActive, itemHovered, imageOrigin, imageMax);
            } else {
                m_pathEdit.boxPending = m_pathEdit.boxSelecting = false;
                m_pathEdit.dragging = false;
                m_pathEdit.dragPoint = 0;
            }

            if (pathNode == nullptr && m_renderer.HasMeshScene() && !lightDragging && !io.KeyAlt) {
                HandleMeshSelection(itemHovered, imageOrigin, imageMax);
            } else {
                m_meshSelection.pending = m_meshSelection.dragging = false;
            }

            // 視点操作は Alt を押している間だけ受ける（Maya と同じ割り当て）。
            //
            // Alt なしのドラッグは、将来の選択や範囲選択のために空けてある。
            // Alt を押している間はライトも無効になる（HandleLightDrag が !io.KeyAlt を見る）ので、
            // ここで競合は起きない。
            if (itemActive && io.KeyAlt) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    camera.Orbit(io.MouseDelta.x * 0.006f, io.MouseDelta.y * 0.006f);
                } else if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
                    camera.Pan(io.MouseDelta.x, io.MouseDelta.y);
                } else if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                    // 右へ引くと寄る。縦は見ない（斜めに引いたときに暴れるため）。
                    camera.Dolly(io.MouseDelta.x);
                }
            }

            if (itemHovered && io.MouseWheel != 0.0f) {
                camera.Zoom(io.MouseWheel);
            }

            HandleCameraShortcuts(itemHovered);

            DrawAxisGizmo(camera, imageOrigin, imageMax);
            DrawLightGizmo(imageOrigin, imageMax);
            if (pathNode != nullptr) {
                DrawPathOverlay(*pathNode, imageOrigin, imageMax);
            }

            // ビューポートに重ねる操作。左上に表示モードの切り替え、右上に FPS。
            DrawViewportOverlay(imageOrigin, imageMax);

        }
    }
    ImGui::End();
}

}  // namespace tg
