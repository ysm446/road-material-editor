// 合成の Height を小さなグリッドへ落とす（CPU へ読み戻すため）。
//
// ビューポートでパスを地形に沿って編集するのに、CPU 側でも地形の高さが要る
// （クリック位置の地形への投影、点の表示位置）。合成解像度をそのまま読み戻すと
// 重いので、セルの平均（DownsampleHeight）で 512² 程度へ落としてから写す。

#include "CompositeCommon.hlsli"

struct DownsampleConstants
{
    uint sourceIndex;  // 入力（Texture2D<float>）
    uint outputIndex;  // 出力（RWTexture2D<float>）
    uint resolution;   // 出力の一辺
    uint pad0;
};

ConstantBuffer<DownsampleConstants> g_downsample : register(b1);

[numthreads(8, 8, 1)]
void CsDownsample(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    const uint resolution = g_downsample.resolution;
    if (cell.x >= resolution || cell.y >= resolution)
    {
        return;
    }
    Texture2D<float> source = ResourceDescriptorHeap[g_downsample.sourceIndex];
    RWTexture2D<float> output = ResourceDescriptorHeap[g_downsample.outputIndex];
    output[cell] = DownsampleHeight(source, cell, resolution);
}
