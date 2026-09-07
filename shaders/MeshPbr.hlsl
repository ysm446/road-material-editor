// マテリアルプレビューのメッシュ描画。
// 出力はトーンマップ前の線形放射輝度で、露出は後段の TonemapPass で掛ける。
//
// 2 枚目のレンダーターゲットへマテリアル UV を書き出す。ペイントのブラシパスが
// 「画面のこの画素はマテリアルのどこか」を引くために使う。CPU へ読み戻さずに
// 済ませるため、ID バッファではなく UV をそのまま持たせている。

#include "Brdf.hlsli"
#include "CompositeCommon.hlsli"
#include "EnvCommon.hlsli"

struct MeshConstants
{
    float4x4 viewProjection;
    // カメラ空間で法線を見るための、投影を掛ける前のビュー行列。
    float4x4 view;
    float4x4 model;
    float4x4 normalMatrix;

    float3 cameraPosition;
    float pad0;

    float3 lightDirection;   // サーフェスから光源へ向かう方向
    float lightIlluminance;  // lux 相当

    float3 lightColor;
    float pad1;

    float3 baseColor;
    float roughness;

    float metallic;
    float iblIntensity;
    uint prefilteredMipCount;
    float pad2;

    uint irradianceIndex;    // irradiance キューブの SRV
    uint prefilteredIndex;   // プリフィルタ済みキューブの SRV
    uint brdfLutIndex;       // 環境 BRDF の LUT
    uint useMaterialTextures;  // 0 なら UI の単色パラメータを使う

    // 合成結果のチャンネル（bindless）
    uint materialBaseColorIndex;
    uint materialNormalIndex;
    uint materialSurfaceIndex;
    uint materialHeightIndex;

    // ビューポートに何を出すか（0 = シェーディング結果）。TG_VIEW_* と一致させる。
    uint debugView;
    // ハイトを形状に反映する量。0 なら押し出さない。
    float displacementScale;
    float roadMetersPerUv;
    uint meshDisplayFlags;

    float4x4 lightViewProjection;

    uint shadowIndex;  // 0xFFFFFFFF なら影を落とさない
    float shadowTexelSize;
    float shadowBias;
    float pad5;

    // テセレーションの分割量を画面上の辺の長さから決めるために使う。
    // **シャドウパスでも本描画と同じ値を渡す。** 分割が違うと形がずれ、
    // 自分の影が自分に落ちて縞（シャドウアクネ）になる。
    float4x4 tessellationViewProjection;
    float2 viewportSize;
    float tessellationMaxFactor;
    // 1 辺をおよそ何ピクセルに保つか。小さいほど細かく割る。
    float tessellationTargetPixels;

    // マスクのプレビューで、0 か 1 に張り付いた所へ斜線を引く。
    // maskPreviewLow / High は、マスク 0 / 1 に対応するベースカラー。
    uint maskPreviewHatch;
    float maskPreviewLow;
    float maskPreviewHigh;
    float pad7;

    // 押し出しに使うハイト。displacementUseRoadUv が 1 なら、displacementHeightIndex の
    // テクスチャを道路 UV で読む（白線が道路面と同じ量だけ動く）。0 なら自分の材質のハイトを uv で読む。
    uint displacementHeightIndex;
    uint displacementUseRoadUv;
    // 不透明度の扱い。0 = 不透明、1 = マスク抜き、2 = 半透明。
    uint opacityMode;
    float opacityThreshold;

    // 道路のレイヤー（スロット 1〜4）。C++ の MeshConstants と同じ並び。
    uint4 layerBaseColorIndex;
    uint4 layerNormalIndex;
    uint4 layerSurfaceIndex;
    uint4 layerHeightIndex;
    uint4 layerWorldUv;
    float4 layerUvRepeat;
    uint roadMaskIndex;
    uint layerCount;
    float layerBlendRange;
    uint roadUvAlongU;
    float2 roadMaskScale;
    float roadUvMetersPerUv;
    uint shadeLayers;
    // 下地のハイトで絞る。0 = 使わない、1 = 下地の高い所、2 = 下地の低い所。
    uint4 layerHeightGate;
    float4 layerHeightGateThreshold;
    float4 layerHeightGateSoftness;
    // 混ぜ方。0 = マスクどおり、1 = ハイトで競合。
    uint4 layerBlendMode;
};


// 「ハイト（ローカル）」で周りの平均を取る半径（合成テクセル）と、
// 引いた差を 0〜1 へ伸ばす倍率。素材の凹凸が見える強さとして選んである。
static const float kLocalHeightRadiusTexels = 6.0f;
static const float kLocalHeightGain = 16.0f;

// ビューポートの表示モード。C++ 側の renderer::DebugView と一致させること。
#define TG_VIEW_SHADED          0
#define TG_VIEW_BASECOLOR       1
#define TG_VIEW_NORMAL_VIEW     2
#define TG_VIEW_NORMAL_WORLD    3
#define TG_VIEW_ROUGHNESS       4
#define TG_VIEW_METALLIC        5
#define TG_VIEW_AO              6
#define TG_VIEW_HEIGHT          7
#define TG_VIEW_HEIGHT_LOCAL    8
#define TG_VIEW_WIREFRAME       9
#define TG_VIEW_CLAY            10

ConstantBuffer<MeshConstants> g_mesh : register(b1);

static const uint kNoTextureIndex = 0xFFFFFFFFu;

// --- 道路のレイヤー -------------------------------------------------------
// docs/design/road-material-layers.md。道路 UV から道路座標（横位置, 実距離）を作り、
// スロットごとに道路 UV かワールド XZ でタイルを引き、道路空間マスクの重みとハイトで競合させる。

// 道路座標（m）。x = 列 0（Right 端）からの横距離、y = 始点からの実距離。
float2 RoadMetersFromUv(float2 roadUv)
{
    const float2 meters = roadUv * g_mesh.roadUvMetersPerUv;
    return (g_mesh.roadUvAlongU != 0u) ? meters.yx : meters;
}

float2 LayerUv(uint slot, float2 meters, float3 worldPosition)
{
    const float repeat = max(g_mesh.layerUvRepeat[slot], 1e-3f);
    if (g_mesh.layerWorldUv[slot] != 0u)
    {
        return worldPosition.xz / repeat;
    }
    const float2 uv = meters / repeat;
    return (g_mesh.roadUvAlongU != 0u) ? uv.yx : uv;
}

// スロット 1〜4 の被覆率。マスクが無ければスロット 1 だけ。
float4 LayerCoverage(float2 meters)
{
    float4 weights = float4(1.0f, 0.0f, 0.0f, 0.0f);
    if (g_mesh.roadMaskIndex == kNoTextureIndex)
    {
        return weights;
    }
    Texture2D<float4> mask = ResourceDescriptorHeap[g_mesh.roadMaskIndex];
    const float3 coverage = mask.SampleLevel(g_samplerLinearClamp, meters * g_mesh.roadMaskScale, 0.0f).rgb;
    weights.y = (g_mesh.layerHeightIndex.y != kNoTextureIndex) ? coverage.x : 0.0f;
    weights.z = (g_mesh.layerHeightIndex.z != kNoTextureIndex) ? coverage.y : 0.0f;
    weights.w = (g_mesh.layerHeightIndex.w != kNoTextureIndex) ? coverage.z : 0.0f;
    weights.x = saturate(1.0f - (weights.y + weights.z + weights.w));
    return weights;
}

// 下地のハイトで被覆率を絞る。Road Mask が「だいたいこの辺」、下地の凹凸が「その中のどこ」。
// 下地（スロット 1）の重みは残りで埋め直す。
float4 ApplyHeightGate(float4 coverage, float baseHeight)
{
    [unroll]
    for (uint slot = 1; slot < 4; ++slot)
    {
        const uint mode = g_mesh.layerHeightGate[slot];
        if (mode == 0u || coverage[slot] <= 0.0f)
        {
            continue;
        }
        const float softness = max(g_mesh.layerHeightGateSoftness[slot], 1e-3f);
        const float signedDelta = (mode == 1u) ? (baseHeight - g_mesh.layerHeightGateThreshold[slot])
                                               : (g_mesh.layerHeightGateThreshold[slot] - baseHeight);
        coverage[slot] *= saturate(signedDelta / softness + 0.5f);
    }
    coverage.x = saturate(1.0f - (coverage.y + coverage.z + coverage.w));
    return coverage;
}

bool AnyHeightGate()
{
    return (g_mesh.layerHeightGate.y | g_mesh.layerHeightGate.z | g_mesh.layerHeightGate.w) != 0u;
}

// スロットの重み。
//   マスクどおり（layerBlendMode = 0）: 被覆率がそのまま重み。境界は下地とのハイト差 × ブレンド幅だけ崩す。
//   ハイトで競合（layerBlendMode = 1）: 被覆率をハイトに足して、最大からブレンド幅の範囲を混ぜる。
// マスクどおりのスロットが先に取り、残りを下地とハイト競合のスロットで分ける。
float4 LayerHeightBlend(float4 coverage, float4 heights)
{
    float4 maskWeights = 0.0f;
    float4 heightCoverage = coverage;
    [unroll]
    for (uint slot = 1; slot < 4; ++slot)
    {
        if (g_mesh.layerBlendMode[slot] == 0u)
        {
            const float w = saturate(coverage[slot] + (heights[slot] - heights[0]) * g_mesh.layerBlendRange);
            maskWeights[slot] = w * step(1e-6f, coverage[slot]);
            heightCoverage[slot] = 0.0f;
        }
    }
    float maskTotal = maskWeights.y + maskWeights.z + maskWeights.w;
    if (maskTotal > 1.0f)
    {
        maskWeights /= maskTotal;
        maskTotal = 1.0f;
    }
    const float remaining = 1.0f - maskTotal;
    heightCoverage.x = saturate(1.0f - (heightCoverage.y + heightCoverage.z + heightCoverage.w));
    const float4 score = heights + heightCoverage;
    const float peak = max(max(score.x, score.y), max(score.z, score.w));
    float4 blend = max(score - peak + max(g_mesh.layerBlendRange, 1e-3f), 0.0f);
    blend *= step(1e-6f, heightCoverage);
    const float total = blend.x + blend.y + blend.z + blend.w;
    const float4 heightBlend = (total > 1e-5f) ? blend / total : float4(1.0f, 0.0f, 0.0f, 0.0f);
    return maskWeights + heightBlend * remaining;
}

float LayerHeightLevel(uint slot, float2 uv)
{
    Texture2D<float> heightMap = ResourceDescriptorHeap[g_mesh.layerHeightIndex[slot]];
    return heightMap.SampleLevel(g_samplerAnisoWrap, uv, 0.0f);
}

// 頂点 / ドメインシェーダ用。ブレンド後のハイト。
float BlendedHeightLevel(float2 roadUv, float3 worldPosition)
{
    const float2 meters = RoadMetersFromUv(roadUv);
    float4 coverage = LayerCoverage(meters);
    float4 heights = 0.5f;
    // 下地のハイトは絞りに使うので、被覆率に関わらず先に読む。
    if (g_mesh.layerHeightIndex[0] != kNoTextureIndex)
    {
        heights[0] = LayerHeightLevel(0, LayerUv(0, meters, worldPosition));
        if (AnyHeightGate())
        {
            coverage = ApplyHeightGate(coverage, heights[0]);
        }
    }
    [unroll]
    for (uint slot = 1; slot < 4; ++slot)
    {
        if (coverage[slot] > 0.0f && g_mesh.layerHeightIndex[slot] != kNoTextureIndex)
        {
            heights[slot] = LayerHeightLevel(slot, LayerUv(slot, meters, worldPosition));
        }
    }
    const float4 blend = LayerHeightBlend(coverage, heights);
    return dot(blend, heights);
}

// --- 合成結果のサンプリング ------------------------------------------------
// 道路は実距離UVを反復し、旧平面プレビューは端をクランプする。
float4 SampleMaterialColor(Texture2D<float4> map, float2 uv)
{
    if (g_mesh.roadMetersPerUv > 0.0f) return map.Sample(g_samplerAnisoWrap, uv);
    return map.Sample(g_samplerAnisoClamp, uv);
}

float2 SampleMaterialNormal(Texture2D<float2> map, float2 uv)
{
    if (g_mesh.roadMetersPerUv > 0.0f) return map.Sample(g_samplerAnisoWrap, uv);
    return map.Sample(g_samplerAnisoClamp, uv);
}

float SampleMaterialScalar(Texture2D<float> map, float2 uv)
{
    if (g_mesh.roadMetersPerUv > 0.0f) return map.Sample(g_samplerAnisoWrap, uv);
    return map.Sample(g_samplerAnisoClamp, uv);
}

// 頂点 / ドメインシェーダ用（微分が無いので SampleLevel）。
float SampleMaterialScalarLevel(Texture2D<float> map, float2 uv)
{
    // 道路は実距離 UV でタイルを繰り返す。
    if (g_mesh.roadMetersPerUv > 0.0f) return map.SampleLevel(g_samplerAnisoWrap, uv, 0.0f);
    return map.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
}

struct VsInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 tangent  : TANGENT;
    float2 uv       : TEXCOORD0;
    float2 roadUv   : TEXCOORD1;
};

struct VsOutput
{
    float4 clipPosition  : SV_Position;
    float3 worldPosition : WORLDPOSITION;
    float3 worldNormal   : NORMAL;
    float3 worldTangent  : TANGENT;
    float tangentSign    : TANGENTSIGN;
    float2 uv            : TEXCOORD0;
    float2 roadUv : TEXCOORD1;
};

// ライトから見た深度と比べて、この画素が影の中かを返す（1 = 当たっている）。
//
// 深度は普通の Texture2D として読む（比較サンプラは使わない）。
// 3x3 のポイントサンプルで平均を取り、境界のジャギーを和らげる。
float SampleShadow(float3 worldPosition, float nDotL, uint shadowIndex, float texelSize,
                   float bias, float4x4 lightViewProjection)
{
    if (shadowIndex == 0xFFFFFFFFu)
    {
        return 1.0f;
    }

    const float4 lightClip = mul(lightViewProjection, float4(worldPosition, 1.0f));
    if (lightClip.w <= 0.0f)
    {
        return 1.0f;
    }
    const float3 ndc = lightClip.xyz / lightClip.w;
    const float2 uv = ndc.xy * float2(0.5f, -0.5f) + 0.5f;
    // 範囲の外は影を落とさない（シャドウマップが覆っていない）。
    if (any(uv < 0.0f) || any(uv > 1.0f) || ndc.z > 1.0f)
    {
        return 1.0f;
    }

    // 斜めに当たっているほど自己遮蔽しやすいので、下駄を増やす。
    const float slopeBias = bias * (1.0f + 3.0f * (1.0f - saturate(nDotL)));

    Texture2D<float> shadowMap = ResourceDescriptorHeap[shadowIndex];
    float visibility = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float2 offset = float2(x, y) * texelSize;
            const float depth = shadowMap.SampleLevel(g_samplerPointClamp, uv + offset, 0.0f);
            visibility += (ndc.z - slopeBias <= depth) ? 1.0f : 0.0f;
        }
    }
    return visibility / 9.0f;
}

// --- ディスプレイスメント -------------------------------------------------
// 合成した Height を読み、ワールド空間の法線方向へ押し引きする。
// **VsMain と DsMain の両方がこの関数を通る。** 別々の式を書くと、
// モデル行列を入れたときにテセレーションの ON / OFF で形が変わってしまう。
// 頂点 / ドメインシェーダには微分が無いので SampleLevel を使う。
float3 ApplyDisplacement(float3 worldPosition, float3 worldNormal, float2 uv, float2 roadUv)
{
    if (g_mesh.displacementScale == 0.0f)
    {
        return worldPosition;
    }
    float height = 0.5f;
    if (g_mesh.layerCount > 0u)
    {
        // 道路面と、その上の帯（白線）。同じ道路座標からブレンド後のハイトを読む。
        height = BlendedHeightLevel(roadUv, worldPosition);
    }
    else if (g_mesh.useMaterialTextures != 0u)
    {
        Texture2D<float> heightMap = ResourceDescriptorHeap[g_mesh.materialHeightIndex];
        height = SampleMaterialScalarLevel(heightMap, uv);
    }
    else
    {
        return worldPosition;
    }
    // 高さの中央（0.5）を基準にする。全体が膨らまないようにするため。
    return worldPosition + worldNormal * ((height - 0.5f) * g_mesh.displacementScale);
}

VsOutput VsMain(VsInput input)
{
    VsOutput output;

    const float3 worldNormal = mul((float3x3)g_mesh.normalMatrix, input.normal);
    float3 worldPosition = mul(g_mesh.model, float4(input.position, 1.0f)).xyz;
    worldPosition = ApplyDisplacement(worldPosition, normalize(worldNormal), input.uv, input.roadUv);

    output.worldPosition = worldPosition;
    output.clipPosition = mul(g_mesh.viewProjection, float4(worldPosition, 1.0f));
    output.worldNormal = worldNormal;
    output.worldTangent = mul((float3x3)g_mesh.model, input.tangent.xyz);
    output.tangentSign = input.tangent.w;
    output.uv = input.uv;
    output.roadUv = input.roadUv;

    return output;
}

// --- テセレーション -------------------------------------------------------
//
// 分割量は**画面上の辺の長さ**から決める。細かいメッシュではそのまま 1 になり、
// 近づいて 1 辺が伸びたときだけ細かく割る。ディスプレイスメントは
// ドメインシェーダで掛ける（分割後の点で高さを引くため）。

// ハードウェアの分割上限。
static const float kTessellationHardwareMax = 64.0f;

struct HsControlPoint
{
    float3 worldPosition : WORLDPOSITION;
    float3 worldNormal   : NORMAL;
    float3 worldTangent  : TANGENT;
    float tangentSign    : TANGENTSIGN;
    float2 uv            : TEXCOORD0;
    float2 roadUv        : TEXCOORD1;
};

struct HsPatchConstants
{
    float edges[3]  : SV_TessFactor;
    float inside    : SV_InsideTessFactor;
};

// 投影も変位もせず、ワールド空間の制御点を出すだけ。
HsControlPoint VsControl(VsInput input)
{
    HsControlPoint output;
    output.worldPosition = mul(g_mesh.model, float4(input.position, 1.0f)).xyz;
    output.worldNormal = mul((float3x3)g_mesh.normalMatrix, input.normal);
    output.worldTangent = mul((float3x3)g_mesh.model, input.tangent.xyz);
    output.tangentSign = input.tangent.w;
    output.uv = input.uv;
    output.roadUv = input.roadUv;
    return output;
}

// ワールド空間の 2 点が画面上で何ピクセル離れるか。
float ScreenEdgeFactor(float3 a, float3 b)
{
    const float4 clipA = mul(g_mesh.tessellationViewProjection, float4(a, 1.0f));
    const float4 clipB = mul(g_mesh.tessellationViewProjection, float4(b, 1.0f));
    // カメラの後ろに回った辺は判断できないので、最大まで割る。
    if (clipA.w <= 0.0f || clipB.w <= 0.0f)
    {
        return min(g_mesh.tessellationMaxFactor, kTessellationHardwareMax);
    }

    const float2 screenA = (clipA.xy / clipA.w) * 0.5f * g_mesh.viewportSize;
    const float2 screenB = (clipB.xy / clipB.w) * 0.5f * g_mesh.viewportSize;
    const float pixels = length(screenA - screenB);
    return clamp(pixels / max(g_mesh.tessellationTargetPixels, 1.0f), 1.0f,
                 min(g_mesh.tessellationMaxFactor, kTessellationHardwareMax));
}

HsPatchConstants HsConstant(InputPatch<HsControlPoint, 3> patch)
{
    HsPatchConstants output;
    // SV_TessFactor[i] は「制御点 i の向かい側の辺」に対応する。
    output.edges[0] = ScreenEdgeFactor(patch[1].worldPosition, patch[2].worldPosition);
    output.edges[1] = ScreenEdgeFactor(patch[2].worldPosition, patch[0].worldPosition);
    output.edges[2] = ScreenEdgeFactor(patch[0].worldPosition, patch[1].worldPosition);
    output.inside = (output.edges[0] + output.edges[1] + output.edges[2]) / 3.0f;
    return output;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("HsConstant")]
HsControlPoint HsMain(InputPatch<HsControlPoint, 3> patch, uint id : SV_OutputControlPointID)
{
    return patch[id];
}

[domain("tri")]
VsOutput DsMain(HsPatchConstants patchConstants, float3 barycentric : SV_DomainLocation,
                const OutputPatch<HsControlPoint, 3> patch)
{
    VsOutput output;

    float3 worldPosition = patch[0].worldPosition * barycentric.x +
                           patch[1].worldPosition * barycentric.y +
                           patch[2].worldPosition * barycentric.z;
    const float3 worldNormal = normalize(patch[0].worldNormal * barycentric.x +
                                         patch[1].worldNormal * barycentric.y +
                                         patch[2].worldNormal * barycentric.z);
    const float3 worldTangent = patch[0].worldTangent * barycentric.x +
                                patch[1].worldTangent * barycentric.y +
                                patch[2].worldTangent * barycentric.z;
    const float2 uv = patch[0].uv * barycentric.x + patch[1].uv * barycentric.y +
                      patch[2].uv * barycentric.z;
    const float2 roadUv = patch[0].roadUv * barycentric.x + patch[1].roadUv * barycentric.y +
                          patch[2].roadUv * barycentric.z;

    // 分割後の点で高さを引いて押し出す。式は VsMain と共通の ApplyDisplacement。
    worldPosition = ApplyDisplacement(worldPosition, worldNormal, uv, roadUv);

    output.worldPosition = worldPosition;
    output.clipPosition = mul(g_mesh.viewProjection, float4(worldPosition, 1.0f));
    output.worldNormal = worldNormal;
    output.worldTangent = worldTangent;
    output.tangentSign = patch[0].tangentSign;
    output.uv = uv;
    output.roadUv = roadUv;
    return output;
}

struct PsOutput
{
    float4 color : SV_Target0;
    // xy: マテリアル UV（タイル 1 枚ぶんに畳んだもの）、z: メッシュに当たったか
    float4 materialUv : SV_Target1;
};

// 距離場を画面微分でなだらかにし、遠方の細いグリッドのちらつきを抑える。
float GridLine(float2 coordinate)
{
    float2 footprint = max(fwidth(coordinate), 1e-5f);
    float2 distance = abs(frac(coordinate + 0.5f) - 0.5f);
    float2 coverage = 1.0f - smoothstep(0.4f, 1.4f, distance / footprint);
    coverage *= saturate(1.0f - footprint);
    return max(coverage.x, coverage.y);
}
float3 ApplyRoadGrid(float3 color, float2 uv)
{
    if ((g_mesh.meshDisplayFlags & 1u) != 0u && g_mesh.roadMetersPerUv > 0.0f)
        color *= lerp(1.0f, 0.12f, GridLine(uv * g_mesh.roadMetersPerUv));
    return color;
}

// ワイヤーフレームの重ね描き。トーンマップ後の表示用テクスチャへ表示色のまま描く。
// 本描画と同じ VS / HS / DS を通るので、テセレーションと変位の後の辺が出る。
float4 PsWireframe(VsOutput input) : SV_Target0
{
    return float4(0.55f, 0.85f, 1.0f, 0.85f);
}

PsOutput PsMain(VsOutput input)
{
    if ((g_mesh.meshDisplayFlags & 2u) != 0u)
    {
        // 1 UVタイルを2×2の市松模様で表示。赤がU、緑がV方向の目印。
        float2 cell = floor(input.uv * 2.0f);
        float checker = fmod(abs(cell.x + cell.y), 2.0f);
        float fade = saturate(1.0f - max(fwidth(input.uv.x), fwidth(input.uv.y)) * 2.0f);
        float3 color = lerp(0.45f.xxx, lerp(0.18f.xxx, 0.75f.xxx, checker), fade);
        float2 local = frac(input.uv);
        color = lerp(color, float3(0.8f,0.12f,0.08f), step(local.y,0.07f)*fade);
        color = lerp(color, float3(0.08f,0.65f,0.18f), step(local.x,0.07f)*fade);
        color *= lerp(1.0f,0.25f,GridLine(input.uv));
        PsOutput result;
        result.color = float4(ApplyRoadGrid(color,input.uv),1.0f);
        result.materialUv = float4(frac(input.uv),1.0f,0.0f);
        return result;
    }
    const float3 geometricNormal = normalize(input.worldNormal);
    const float3 viewDirection = normalize(g_mesh.cameraPosition - input.worldPosition);

    float3 baseColor = g_mesh.baseColor;
    float roughnessValue = g_mesh.roughness;
    float metallicValue = g_mesh.metallic;
    float ambientOcclusion = 1.0f;
    float opacity = 1.0f;
    float3 normal = geometricNormal;

    // **クレイ表示**は、形（変位）はそのままで陰影だけをテクスチャ抜きにする。
    // 合成の色 / 法線 / サーフェスを読まず、単色マテリアルと面の向きで塗る。
    const bool clay = (g_mesh.debugView == TG_VIEW_CLAY);
    const bool useMaterialShading = (g_mesh.useMaterialTextures != 0u) && !clay;

    if (clay)
    {
        // **面から法線を起こす。** 平面メッシュの頂点法線は押し出しても上を向いた
        // ままなので、そのまま陰影を付けると形が出ない。画面微分から取れば
        // 実際に描かれた三角形の向きになり、分割の粗さが面として見える
        // （メッシュの確認にはこれが要る）。
        const float3 faceNormal =
            normalize(cross(ddx(input.worldPosition), ddy(input.worldPosition)));
        // 三角形の巻き方によって裏返るので、視線の側へ向ける。
        normal = (dot(faceNormal, viewDirection) < 0.0f) ? -faceNormal : faceNormal;
    }

    if (useMaterialShading && g_mesh.shadeLayers != 0u)
    {
        // 道路面。スロット 1〜4 を道路空間マスクの被覆率とハイトで競合させて混ぜる。
        const float2 meters = RoadMetersFromUv(input.roadUv);
        float4 coverage = LayerCoverage(meters);
        float2 uvs[4];
        float4 heights = 0.5f;
        [unroll]
        for (uint slot = 0; slot < 4; ++slot)
        {
            uvs[slot] = LayerUv(slot, meters, input.worldPosition);
        }
        // 下地のハイトは絞りに使うので、被覆率に関わらず先に読む。
        if (g_mesh.layerHeightIndex[0] != kNoTextureIndex)
        {
            Texture2D<float> baseHeightMap = ResourceDescriptorHeap[g_mesh.layerHeightIndex[0]];
            heights[0] = baseHeightMap.Sample(g_samplerAnisoWrap, uvs[0]);
            if (AnyHeightGate())
            {
                coverage = ApplyHeightGate(coverage, heights[0]);
            }
        }
        [unroll]
        for (uint slot = 1; slot < 4; ++slot)
        {
            if (coverage[slot] > 0.0f && g_mesh.layerHeightIndex[slot] != kNoTextureIndex)
            {
                Texture2D<float> heightMap = ResourceDescriptorHeap[g_mesh.layerHeightIndex[slot]];
                heights[slot] = heightMap.Sample(g_samplerAnisoWrap, uvs[slot]);
            }
        }
        const float4 blend = LayerHeightBlend(coverage, heights);
        float3 blendedColor = 0.0f;
        float2 blendedNormal = 0.0f;
        float4 blendedSurface = 0.0f;
        [unroll]
        for (uint slot2 = 0; slot2 < 4; ++slot2)
        {
            if (blend[slot2] <= 0.0f || g_mesh.layerBaseColorIndex[slot2] == kNoTextureIndex)
            {
                continue;
            }
            Texture2D<float4> baseColorMap = ResourceDescriptorHeap[g_mesh.layerBaseColorIndex[slot2]];
            Texture2D<float2> normalMap    = ResourceDescriptorHeap[g_mesh.layerNormalIndex[slot2]];
            Texture2D<float4> surfaceMap   = ResourceDescriptorHeap[g_mesh.layerSurfaceIndex[slot2]];
            blendedColor += baseColorMap.Sample(g_samplerAnisoWrap, uvs[slot2]).rgb * blend[slot2];
            blendedNormal += normalMap.Sample(g_samplerAnisoWrap, uvs[slot2]) * blend[slot2];
            blendedSurface += surfaceMap.Sample(g_samplerAnisoWrap, uvs[slot2]) * blend[slot2];
        }
        baseColor = blendedColor;
        roughnessValue = blendedSurface.r;
        metallicValue = blendedSurface.g;
        ambientOcclusion = blendedSurface.b;
        opacity = 1.0f;
        const float3 tangentNormal = DecodeTangentNormal(blendedNormal);
        const float3 tangent =
            normalize(input.worldTangent - geometricNormal * dot(geometricNormal, input.worldTangent));
        const float3 bitangent = cross(geometricNormal, tangent) * input.tangentSign;
        normal = normalize(tangent * tangentNormal.x + bitangent * tangentNormal.y +
                           geometricNormal * tangentNormal.z);
    }
    else if (useMaterialShading)
    {
        Texture2D<float4> baseColorMap = ResourceDescriptorHeap[g_mesh.materialBaseColorIndex];
        Texture2D<float2> normalMap    = ResourceDescriptorHeap[g_mesh.materialNormalIndex];
        Texture2D<float4> surfaceMap   = ResourceDescriptorHeap[g_mesh.materialSurfaceIndex];

        const float2 uv = input.uv;

        baseColor = SampleMaterialColor(baseColorMap, uv).rgb;

        const float4 surface = SampleMaterialColor(surfaceMap, uv);
        roughnessValue = surface.r;
        metallicValue = surface.g;
        ambientOcclusion = surface.b;
        // 不透明度は Surface の A。マスク抜きはここで捨て、半透明は最後にアルファへ載せる。
        opacity = surface.a;
        if (g_mesh.opacityMode == 1u && opacity < g_mesh.opacityThreshold)
        {
            discard;
        }

        // タンジェント空間法線をワールド空間へ移す。
        const float3 tangentNormal = DecodeTangentNormal(SampleMaterialNormal(normalMap, uv));
        const float3 tangent =
            normalize(input.worldTangent - geometricNormal * dot(geometricNormal, input.worldTangent));
        const float3 bitangent = cross(geometricNormal, tangent) * input.tangentSign;
        normal = normalize(tangent * tangentNormal.x + bitangent * tangentNormal.y +
                           geometricNormal * tangentNormal.z);
    }

    // --- マスクのプレビューで、飽和した所へ斜線を引く ----------------------
    //
    // マスクのプレビューは「0 の色」と「1 の色」の間を塗るだけなので、
    // ベースカラーから元のマスクへ戻せる。**0 か 1 に張り付いている所**へ
    // 1px 幅・4px 周期の斜線を中間の灰色で重ね、飽和していることを見せる。
    // 濃淡が付いている所と、上限に当たって潰れた所は、絵では見分けが付かない。
    //
    // 画素の座標は **x と y を別々に切り捨ててから足す**。float のまま足して
    // 丸めると桁落ちで縞の位相が揺れ、太いバンドに見える（terrain-editor で踏んだ）。
    if (g_mesh.maskPreviewHatch != 0u && useMaterialShading)
    {
        const float low = g_mesh.maskPreviewLow;
        const float high = g_mesh.maskPreviewHigh;
        const float mask = saturate((baseColor.r - low) / max(high - low, 1e-4f));
        if (mask >= 0.99f || mask <= 0.01f)
        {
            const int2 pixel = int2(int(input.clipPosition.x), int(input.clipPosition.y));
            if (((pixel.x + pixel.y) & 3) == 3)
            {
                const float stripe = lerp(low, high, 0.5f);
                baseColor = float3(stripe, stripe, stripe);
            }
        }
    }

    // --- チャンネルを覗く表示 ----------------------------------------------
    // チャンネルの中身をそのまま出す。露出もトーンマップも掛けない
    // （後段の TonemapPass が素通しする）。**クレイはここへ来ない。**
    // 陰影を付ける表示なので、下のシェーディングをそのまま通す。
    if (g_mesh.debugView != TG_VIEW_SHADED && !clay)
    {
        float3 debugColor = float3(0.0f, 0.0f, 0.0f);
        if (g_mesh.debugView == TG_VIEW_BASECOLOR)
        {
            // ベースカラーはリニアで持っているので、見た目を合わせて sRGB で出す。
            debugColor = LinearToSrgb(saturate(baseColor));
        }
        else if (g_mesh.debugView == TG_VIEW_NORMAL_VIEW)
        {
            // 陰影に使う向きを**カメラ空間**で見る。ビュー行列は回転と平行移動だけ
            // なので、上 3x3 を掛ければ向きが移る（正規化は数値誤差の始末）。
            // カメラは -Z を向く（右手系）ので、正面を向いた面が +Z＝水色になり、
            // 法線マップと同じ読み方（平らなら水色）ができる。
            const float3 viewNormal = normalize(mul((float3x3)g_mesh.view, normal));
            debugColor = viewNormal * 0.5f + 0.5f;
        }
        else if (g_mesh.debugView == TG_VIEW_NORMAL_WORLD)
        {
            // 陰影に実際に使う向き。法線マップを当てたあとのワールド空間法線。
            debugColor = normal * 0.5f + 0.5f;
        }
        else if (g_mesh.debugView == TG_VIEW_ROUGHNESS)
        {
            debugColor = roughnessValue.xxx;
        }
        else if (g_mesh.debugView == TG_VIEW_METALLIC)
        {
            debugColor = metallicValue.xxx;
        }
        else if (g_mesh.debugView == TG_VIEW_AO)
        {
            debugColor = ambientOcclusion.xxx;
        }
        else if (g_mesh.debugView == TG_VIEW_WIREFRAME)
        {
            // 線だけを見る表示。塗りではないので単色で描く。
            debugColor = float3(0.66f, 0.72f, 0.78f);
        }
        else if (g_mesh.debugView == TG_VIEW_HEIGHT)
        {
            float height = 0.0f;
            if (g_mesh.useMaterialTextures != 0u)
            {
                Texture2D<float> heightMap = ResourceDescriptorHeap[g_mesh.materialHeightIndex];
                height = SampleMaterialScalar(heightMap, input.uv);
            }
            debugColor = saturate(height).xxx;
        }
        else if (g_mesh.debugView == TG_VIEW_HEIGHT_LOCAL)
        {
            // **その場の起伏だけ**を見る。地形の大きな高さ（標高差 600m の傾き）を
            // 周りの平均として引き、残りを 0.5 中心へ伸ばす。
            // 素材のハイトマップをそのまま貼ったような見た目になる。
            float local = 0.5f;
            if (g_mesh.useMaterialTextures != 0u)
            {
                Texture2D<float> heightMap = ResourceDescriptorHeap[g_mesh.materialHeightIndex];
                const float center = SampleMaterialScalar(heightMap, input.uv);

                // 周りの平均。半径は合成テクセル基準で固定する
                // （解像度を変えても「どのくらい大きな形を引くか」が変わらない）。
                float2 size = float2(1.0f, 1.0f);
                heightMap.GetDimensions(size.x, size.y);
                const float2 texel = 1.0f / max(size, float2(1.0f, 1.0f));
                const float radius = kLocalHeightRadiusTexels;
                float sum = 0.0f;
                [unroll]
                for (int i = 0; i < 8; ++i)
                {
                    const float angle = (float(i) / 8.0f) * 6.28318530718f;
                    const float2 offset = float2(cos(angle), sin(angle)) * radius * texel;
                    sum += SampleMaterialScalar(heightMap, input.uv + offset);
                }
                local = 0.5f + (center - sum / 8.0f) * kLocalHeightGain;
            }
            debugColor = saturate(local).xxx;
        }

        PsOutput debugOutput;
        debugOutput.color = float4(ApplyRoadGrid(debugColor, input.uv), 1.0f);
        debugOutput.materialUv = float4(frac(input.uv), 1.0f, 0.0f);
        return debugOutput;
    }

    float3 diffuseColor;
    float3 f0;
    SplitBaseColor(baseColor, metallicValue, diffuseColor, f0);

    const float roughness = clamp(roughnessValue, kMinPerceptualRoughness, 1.0f);

    const float3 lightDirection = normalize(g_mesh.lightDirection);
    // 影は直接光にだけ掛ける。環境光（IBL）は別に扱う。
    const float shadow = SampleShadow(input.worldPosition, dot(normal, lightDirection),
                                      g_mesh.shadowIndex, g_mesh.shadowTexelSize,
                                      g_mesh.shadowBias, g_mesh.lightViewProjection);

    float3 radiance = ShadeDirectionalLight(normal, viewDirection, lightDirection,
                                            g_mesh.lightColor, g_mesh.lightIlluminance,
                                            diffuseColor, f0, roughness) *
                      shadow;

    // --- IBL（分割和近似） -------------------------------------------------
    // saturate + 加算だと最大 1.00001 になり、FresnelSchlickRoughness の
    // pow(1 - nDotV, 5) が負の底で NaN になる。clamp で上限も守る。
    const float nDotV = clamp(dot(normal, viewDirection), 1e-4f, 1.0f);

    TextureCube<float4> irradianceMap = ResourceDescriptorHeap[g_mesh.irradianceIndex];
    TextureCube<float4> prefilteredMap = ResourceDescriptorHeap[g_mesh.prefilteredIndex];
    Texture2D<float2> brdfLut = ResourceDescriptorHeap[g_mesh.brdfLutIndex];

    // irradiance マップには E / pi（平均放射輝度）が入っているので、
    // diffuseColor を掛けるだけでよい。
    const float3 irradiance = irradianceMap.SampleLevel(g_samplerLinearClamp, normal, 0.0f).rgb;

    const float3 fresnel = FresnelSchlickRoughness(f0, nDotV, roughness);
    const float3 kD = 1.0f - fresnel;
    const float3 diffuseIbl = kD * diffuseColor * irradiance;

    const float3 reflectionDirection = reflect(-viewDirection, normal);
    const float mipLevel = roughness * float(max(g_mesh.prefilteredMipCount, 1u) - 1u);
    const float3 prefiltered =
        prefilteredMap.SampleLevel(g_samplerLinearClamp, reflectionDirection, mipLevel).rgb;

    const float2 environmentBrdf =
        brdfLut.SampleLevel(g_samplerLinearClamp, float2(nDotV, roughness), 0.0f);
    const float3 specularIbl = prefiltered * (f0 * environmentBrdf.x + environmentBrdf.y);

    radiance += (diffuseIbl + specularIbl) * g_mesh.iblIntensity * ambientOcclusion;

    PsOutput output;
    // シーンカラーは R16G16B16A16_FLOAT。half の上限（65504）を超えると Inf になり、
    // トーンマップを経て NaN → ハイライト中心の黒点になる。上限手前でクランプする。
    output.color = float4(ApplyRoadGrid(min(radiance, 60000.0f), input.uv),
                          (g_mesh.opacityMode == 2u) ? opacity : 1.0f);
    // ペイントマスクはタイル 1 枚ぶんのテクスチャなので、UV も畳んで書き出す。
    output.materialUv = float4(frac(input.uv), 1.0f, 0.0f);
    return output;
}
