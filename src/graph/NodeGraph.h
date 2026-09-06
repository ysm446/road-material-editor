#pragma once

#include "compositor/MaterialLayer.h"
#include "compositor/MaskGraph.h"
#include "graph/Path.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// ノードグラフのデータモデル。terrain-editor から「仕組み」を移植したもの。
//
//   - ノード・ピン・リンクは共通の単一 ID 空間から採番する
//     （imgui-node-editor の NodeId / PinId / LinkId にそのまま流用できる）。
//   - ノードの設定は種類ごとの構造体を std::variant で持つ。
//     terrain-editor の「全種類の設定を 1 構造体に持つファット構造体」はやめた。
//   - 評価は既存の GPU 評価器を使う。グラフは CompileLayers() でレイヤー列
//     （下から上）へ落とし、MaterialStack として評価する。
//     見た目はレイヤー時代と完全に一致する。
//
// UI / D3D12 には依存しない（compositor のデータ構造にだけ依存する）。
namespace tg::graph {

using GraphId = int;

enum class PinKind : uint32_t {
    Input,
    Output,
};

// ピンを流れる値の型。同じ型どうしだけ接続できる。
enum class ValueType : uint32_t {
    Material = 0,
    // マスク（0〜1 の 1 チャンネル）。レイヤーの「どこに乗せるか」を外から与える。
    Mask = 1,
    // パス（地形の上に引いた向き付きの線）。Path ノードが出し、Mask Path が読む。
    Path = 2,
    Mesh = 3,
    // 道路空間マスク（横位置 × 実距離）。Road Mask ノードが出し、Road のマスク入力が読む。
    RoadMask = 4,
};

enum class NodeKind : uint32_t {
    Road = 24,
    MeshOutput = 25,
    // 道路面の上に白線の帯ポリゴンを生成する。RoadSurfaceを受け、道路と白線をまとめて出す。
    RoadMarking = 26,
    // 道路空間マスク。轍・端の減衰・長さ方向ノイズを Road のスロット 2〜4 の被覆率にする。
    RoadMask = 27,
    // 面上のパスに沿って帯を貼るデカール。ひび・補修跡・汚れなどの模様。
    Decal = 28,
    // 路肩。Road の Left / Right（または別の路肩の Outer）の境界の頂点を起点に、外側へ断面を押し出す。
    Shoulder = 29,
    // 複数の Mesh の枝を 1 つの RoadSurface にまとめる。同じノード由来のメッシュは 1 回だけ積む。
    Merge = 30,
    Surface = 0,
    Shape = 1,
    Liquid = 2,
    Output = 3,
    // ハイトマップの読み込み。**入力を持たないソース**で、地形そのものを定義する。
    // 中身はシェイプと同じ（下地への加算）だが、チェーンの先頭に置く前提なので
    // 下地が無く、加算はそのまま地形の形になる。
    Heightmap = 4,
    // ハイトをぼかす加工。合成せず、下地のハイトを分離型ガウスでならす。
    Blur = 5,
    // 画像 1 枚をマスクとして出す**マスクのソース**。入力を持たない。
    MaskImage = 6,
    // 下地の川筋（フロー累積）をマスクとして出す。
    MaskFluvial = 7,
    // 下地の傾斜をマスクとして出す（角度の範囲を 0〜1 へ）。
    MaskSlope = 8,
    // マスクの黒点 / 白点 / ガンマ / 反転。
    MaskLevels = 9,
    // マスク 2 枚の合成。**グラフが合流する唯一のノード。**
    MaskBlend = 10,
    // 土砂を重力で再分配する加工（terrain-editor の Sediment）。
    Sediment = 11,
    // 発生源から岩屑を崩し落とす加工（terrain-editor の Crumbling）。
    Crumbling = 12,
    // ノイズ 1 枚をマスクとして出す**マスクのソース**。入力を持たない。
    MaskNoise = 13,
    // 下地の曲率（周りより高い / 低い）をマスクとして出す。
    MaskCurvature = 14,
    // 雪を降らせ、急な雪面から低い所へ滑らせて積もらせる加工
    // （terrain-editor の Snow）。
    Snow = 15,
    // 下地の標高帯をマスクとして出す。
    MaskHeight = 16,
    // 川筋から河床を掘り、下流へ単調に下がる水面を張る加工。
    // 水面 / 河原 / 水深の 3 つの Mask も出す。
    River = 17,
    // 地形の上に引いた向き付きの線。道路 / 川 / 氷河のような「方向のあるもの」の
    // ガイド。ビューポートで編集する。Base 入力は「どの時点の地形に沿うか」。
    Path = 18,
    // パスの足跡をマスクにする（中心線からの距離を幅とフェザーで 0〜1 へ）。
    MaskPath = 19,
    // 水滴侵食。水滴を落として斜面を下らせ、削って運んで積む加工。
    // 流量（水の通った量）と堆積量の 2 つの Mask も出す。
    Droplet = 20,
    // 散布。単純な形（半球 / 円錐）をばら撒く加工。分布の Mask と、
    // 個体ごとに違う値を持つ Unique Mask も出す（terrain-editor の Scatter）。
    Scatter = 21,
    // パスの閉じた鎖の内側をマスクにする（面。エリア選択）。輪の中の輪は穴。
    MaskArea = 23,
    // マスクをぼかす。境界をなだらかにして、乗せたものを馴染ませる
    // （terrain-editor の Mask Blur）。
    MaskBlur = 22,
};

struct PinDefinition {
    PinKind kind = PinKind::Input;
    ValueType valueType = ValueType::Material;
    const char* label = "";
};

// ノードの静的な定義。種類・保存名・表示名・ピン構成をテーブルで持ち、
// CreateNode() がここからピンを生成する。
struct NodeDefinition {
    NodeKind kind = NodeKind::Surface;
    const char* name = "";   // 保存名（ファイルには enum の数値ではなくこれを書く）
    const char* title = "";  // 表示名
    std::span<const PinDefinition> pins;
};

struct Pin {
    GraphId id = 0;
    GraphId nodeId = 0;
    PinKind kind = PinKind::Input;
    ValueType valueType = ValueType::Material;
    std::string label;
};

// --- ノードの設定 ---------------------------------------------------------
//
// ノードを増やすときは、(1) 設定構造体を定義し、(2) NodeSettings へ足し、
// (3) NodeGraph.cpp の定義テーブルへ登録し、(4) 保存とプロパティ UI の
// 対応を足す。それ以外の場所を触る必要がないように保つ。

// ジオメトリの実寸（m）。**ソース（Heightmap）だけが持つ。**
//
// 「この地形は一辺 2048m、標高差 604m」を**読み込むときに一度だけ**決める。
// プレビュー設定ではなくノードに置くのは、実寸がプレビューの都合ではなく
// 読み込んだデータそのものの性質だから。後から触るものではない。
//
// **メートルなのはジオメトリだけ。** ハイトは 0〜1 の正規化値のままで、
// heightMeters はその全幅が何 m かを表す（[design/rendering.md]）。
struct TerrainScale {
    float sizeMeters = 1024.0f;
    float heightMeters = 200.0f;
};

// サーフェス / シェイプ / 水面 / ハイトマップ。既存のレイヤーそのもの
// （kind も layer が持つ）。scale はソースのときだけ意味を持つ。
struct LayerNodeSettings {
    compositor::MaterialLayer layer;
    TerrainScale scale;
};

// マスクのソース。レイヤーの Mask 入力へ繋ぐと、そのレイヤーは
// **白い所にだけ**乗る。どちらを使うかはノードの種類で決まる。
//   MaskImage   : map（画像 1 枚 + 読むチャンネル）
//   MaskFluvial : fluvial（下地の川筋）
struct MaskNodeSettings {
    compositor::MapSlot map;
    compositor::NoiseParams noise;
    compositor::FluvialParams fluvial;
    compositor::HeightParams height;
    compositor::SlopeParams slope;
    compositor::CurvatureParams curvature;
    compositor::LevelsParams levels;
    compositor::BlendParams blend;
    compositor::MaskBlurParams blur;
    compositor::PathMaskParams pathMask;
    compositor::AreaMaskParams areaMask;
};

// パス（Path ノード）。点と向き付きのエッジ。中身は graph/Path.h。
struct PathNodeSettings {
    PathSettings path;
};

// 道路の材質スロット数。スロット 1 が下地、2〜4 は道路マスクの R / G / B で被覆する。
inline constexpr int kRoadMaterialSlots = 4;

struct RoadNodeSettings {
    float widthMeters = 6.0f;
    float uvRepeatMeters = 1.0f;
    // 車線数。線形の向きへ進む車線（forward）と対向車線（backward）。車線幅は全幅÷合計。
    // どちら側に並ぶかは走行側（RoadNetworkSettings）で決まる。対向 0 で一方通行。
    uint32_t lanesForward = 1;
    uint32_t lanesBackward = 1;
    // Material のハイトで路面を押し出す量（m）。ハイト 0〜1 の全幅がこの高さになる。
    float displacementMeters = 0.0f;
    // 真なら道路の長さ方向を U にする（既定は V）。横長のテクスチャを道路に沿わせるとき。
    bool uvAlongU = false;
    // スロットごとのテクスチャ座標。真ならワールド XZ 平面、偽なら道路 UV。
    bool layerWorldUv[kRoadMaterialSlots] = {false, false, false, false};
    // スロット 2〜4 の UV 反復長（m）。スロット 1 は uvRepeatMeters。
    float layerUvRepeatMeters[kRoadMaterialSlots] = {1.0f, 1.0f, 1.0f, 1.0f};
    // ハイトで競合させるときの境界の柔らかさ（ハイト 0〜1 の単位）。
    float layerBlendRange = 0.2f;
};

// 道路空間マスクの形。
enum class RoadMaskShape : uint32_t {
    WheelTracks = 0,  // 轍。車線中央 ± タイヤ間隔/2 の帯
    EdgeFalloff = 1,  // 道路端からの距離で減衰
    LengthNoise = 2,  // 長さ方向のノイズをしきい値で切る
    Constant = 3,     // 一様
};

// デカール。Path（Surface に道路を繋いだ面上のパス）に沿った幅 widthMeters の帯を、
// 道路面と一体で押し出される帯メッシュとして貼る。材質の不透明度で模様をくり抜く。
struct DecalNodeSettings {
    float widthMeters = 1.0f;
    float liftMeters = 0.008f;
    float uvRepeatMeters = 1.0f;
    bool uvAlongU = false;
};

// 路肩。境界の点列（Road の Left / Right、路肩の Outer）をそのまま内側の列にして、
// 外側へ widthMeters 押し出した格子。境界の頂点を共有するので道路と水密になる。
// 列 0 が内側（境界）、列末尾が外側（Outer）。進行方向の左右や走行側には依存しない。
struct ShoulderNodeSettings {
    float widthMeters = 1.5f;
    // 横断勾配（%）。正なら外側へ向かって下がる。
    float crossSlopePercent = 4.0f;
    // 舗装端の段差（m）。0 より大きいと、境界の直後に stepWidthMeters の面取り列を 1 つ挟み、そこで段差ぶん下げる。
    float stepHeightMeters = 0.0f;
    float stepWidthMeters = 0.05f;
    float uvRepeatMeters = 1.0f;
    bool uvAlongU = false;
    // 材質のレイヤー構造は Road と同じ（スロット × 4、Mask 2〜4、変位）。
    // 路肩の横位置は境界（列 0）が Right、外側が Left。Road Mask の「側」はその向きで読む。
    float displacementMeters = 0.0f;
    bool layerWorldUv[kRoadMaterialSlots] = {true, true, true, true};
    float layerUvRepeatMeters[kRoadMaterialSlots] = {1.0f, 1.0f, 1.0f, 1.0f};
    float layerBlendRange = 0.2f;
};

// Merge。設定は持たない。Mesh 1〜4 に繋いだ枝を順に積み、下流の白線・Decal は最初の枝の面に乗る。
struct MergeNodeSettings {};

// 端の減衰をどちらの端に出すか。左右は Path の進行方向基準（Road の Left / Right と同じ）。走行側には依存しない。
enum class RoadMaskSide : uint32_t {
    Both = 0,
    Left = 1,
    Right = 2,
};

struct RoadMaskNodeSettings {
    RoadMaskShape shape = RoadMaskShape::WheelTracks;
    // 轍。車線中央の中心線からの距離、タイヤ間隔、帯の幅、縁のぼかし。
    float laneOffsetMeters = 1.5f;
    float trackSpacingMeters = 1.5f;
    float trackWidthMeters = 0.35f;
    float featherMeters = 0.25f;
    // 真なら Road の車線数から各車線の中央に置く（laneOffsetMeters は使わない）。
    // 旧ファイルにキーが無ければ偽（手入力のまま）。路肩など車線の無い面では手入力に落ちる。
    bool tracksFromLanes = true;
    // 対向車線にも置くか（車線に合わせるとき）。手入力のときは中心線の左右両方に置くか。
    bool bothLanes = true;
    // 端の減衰。端で 1 になる幅と、その内側のぼかし幅。側を選ぶと片側の端だけになる。
    float edgeWidthMeters = 0.3f;
    RoadMaskSide edgeSide = RoadMaskSide::Both;
    // 長さ方向ノイズ。
    float noiseScaleMeters = 4.0f;
    float threshold = 0.5f;
    float softness = 0.2f;
    uint32_t seed = 1u;
    // 長さ方向のムラ（どの形にも掛かる）。0 で一様。
    float breakupAmount = 0.3f;
    float breakupScaleMeters = 3.0f;
    float strength = 1.0f;
    bool invert = false;
};

// 道路網に共通の設定。走行側は道路ごとではなくプロジェクトで 1 つ。
// 車線の進行方向・矢印・標識の向きの判定に使う。
struct RoadNetworkSettings {
    bool leftHandTraffic = true;
};

// 白線（Lane Marking）。寸法は m。外側線は道路端から中心線側へ edgeInsetMeters の位置に置く。
struct RoadMarkingNodeSettings {
    float lineWidthMeters = 0.15f;
    // 中央線（進行方向と対向の境）。一方通行なら出ない。
    bool centerLine = true;
    bool edgeLines = true;
    float edgeInsetMeters = 0.5f;
    // 車線境界線。同方向の車線の間に破線で引く。
    bool laneLines = true;
    float dashLengthMeters = 5.0f;
    float dashGapMeters = 5.0f;
    // 停止線。Path の点に付けた停止線を、その向きの車線の幅いっぱいに引く。
    bool stopLines = true;
    float stopLineWidthMeters = 0.45f;
    // 進行方向の矢印。左右の車線の中央に一定間隔で置き、走行側に応じて向きを決める。
    bool arrows = true;
    float arrowIntervalMeters = 30.0f;
    float arrowLengthMeters = 5.0f;
    // 路面との重なりによるちらつきを避けるため、法線方向へ浮かせる量。
    float liftMeters = 0.005f;
    // 帯の長さ方向でVが1増える実距離。幅方向のUは帯の左端0〜右端1。
    float uvRepeatMeters = 1.0f;
    // 真なら長さ方向を U、幅方向を V にする（横長の白線テクスチャ向け）。
    bool uvAlongU = false;
};

// グラフを評価器の入力へ落とした結果。レイヤー列と、マスクの op の列。
struct CompiledGraph {
    std::vector<compositor::MaterialLayer> layers;
    compositor::MaskProgram maskOps;
    // op ごとの出どころ（ノード ID と、そのノードの何番目の Mask 出力か）。
    // 添字は maskOps と同じ。グラフパネルがノードにマスクのサムネイルを出すのに使う。
    struct MaskOpSource {
        GraphId nodeId = 0;
        size_t outputIndex = 0;
    };
    std::vector<MaskOpSource> maskOpSources;
    // レイヤーごとの元ノードの ID（添字は layers と同じ。プレビュー用の塗りレイヤーは 0）。
    // グラフパネルがノードに合成結果のサムネイルを出すのに使う。
    std::vector<GraphId> layerSources;
};

// 出力。ここに繋いだチェーンがプレビューのマテリアルになる。
struct OutputNodeSettings {};

using NodeSettings =
    std::variant<LayerNodeSettings, MaskNodeSettings, OutputNodeSettings, PathNodeSettings, RoadNodeSettings,
                 RoadMarkingNodeSettings, RoadMaskNodeSettings, DecalNodeSettings, ShoulderNodeSettings,
                 MergeNodeSettings>;

struct Node {
    GraphId id = 0;
    NodeKind kind = NodeKind::Surface;
    std::vector<Pin> inputs;
    std::vector<Pin> outputs;
    NodeSettings settings;
    // エディタ上の位置。UI が読み書きし、保存にも含める。
    // positionValid が偽の間は UI が初期位置を与える。
    float posX = 0.0f;
    float posY = 0.0f;
    bool positionValid = false;
};

struct Link {
    GraphId id = 0;
    GraphId startPin = 0;  // 出力ピン
    GraphId endPin = 0;    // 入力ピン
};

class NodeGraph {
public:
    // 「サーフェス（ベース）→ 出力」を繋いだ最小構成。
    static NodeGraph CreateDefault();

    const std::vector<Node>& Nodes() const { return m_nodes; }
    std::vector<Node>& MutableNodes() { return m_nodes; }
    const std::vector<Link>& Links() const { return m_links; }

    const Pin* FindPin(GraphId pinId) const;
    const Node* FindNode(GraphId nodeId) const;
    Node* FindMutableNode(GraphId nodeId);
    // 入力ピンに繋がっている上流ノード。無ければ nullptr。
    const Node* FindUpstreamNodeForPin(GraphId inputPinId) const;

    // 接続できるか。別ノード・型一致・入出力の組み合わせに加えて、
    // **循環ができる接続は弾く**（評価が回らなくなるため）。
    bool CanCreateLink(GraphId startPin, GraphId endPin) const;
    // 接続する。入力ピンに既にある接続は置き換える。
    bool CreateLink(GraphId startPin, GraphId endPin);
    bool DeleteLink(GraphId linkId);
    // 定義テーブルからピンを生成してノードを足す。設定は既定値。
    GraphId CreateNode(NodeKind kind);
    bool DeleteNode(GraphId nodeId);

    // 読み込み用。ID はファイルの値をそのまま使い、次の採番を max+1 に合わせる。
    void Replace(std::vector<Node> nodes, std::vector<Link> links);

    // 道路網の共通設定（走行側）。変えたら再生成の対象なので改版する。
    const RoadNetworkSettings& RoadNetwork() const { return m_roadNetwork; }
    void SetRoadNetwork(const RoadNetworkSettings& settings) { m_roadNetwork = settings; MarkDirty(); }

    // グラフをレイヤー列（下から上）とマスクの op の列へ落とす。
    // 出力ノードの「下地」チェーンを遡る。
    // チェーンが空なら下地 1 枚（MaterialStack::MakeBaseLayer と同じもの）を返す。
    CompiledGraph CompileLayers() const;
    // 指定したノード**まで**。ノードを選んでプレビューするときに使う。
    // outputPin は**どの出力を見ているか**。0 なら最初の出力（レイヤーなら Result）。
    // マスクの出力を見ているときは、その結果を白黒で貼ったプレビューになる
    // （堆積のように Result と Mask を両方出すノードは、ピンで見分ける）。
    CompiledGraph CompileLayersTo(GraphId nodeId, GraphId outputPin = 0) const;

    // そのマスクが下地のハイトを見るか。見るなら地形の上に貼らないと意味が読めない。
    // 川筋 / 傾斜 / 堆積 / 崩落が真で、画像 / ノイズは偽。
    // レベルとブレンドは入力を辿る（1 つでも見ていれば真）。
    bool MaskDependsOnHeight(const Node& maskNode, int depth = 0) const;

    // 出どころ（堆積 / 崩落）をチェーンへ差し込む位置。返り値は layerNodes の添字で、
    // **その直後**へ差し込む。合流する所が無ければ -1。
    int FindMaskSpliceIndex(const Node& source,
                            const std::vector<const Node*>& layerNodes) const;

    // Mask 入力に繋いだ出どころが、実際にマスクとして効くか。
    //
    // 堆積 / 崩落の Mask 出力は、**そのレイヤーを合成した時点の作業用テクスチャ**
    // から焼く。だから出どころが consumer の下地チェーンの中にいないと、
    // 繋いでも結果が残っておらず、マスクが無いのと同じ扱いになる。
    // 繋いでいない場合と、他の種類のマスクは常に true。
    bool MaskSourceResolves(const Node& consumer) const;

    // チェーンの根にあるソース（Heightmap）の実寸。無ければ nullptr。
    // プレビューの平面のサイズと変位量はこれに従う。
    // nodeId が 0 なら出力ノードのチェーンを見る。
    const TerrainScale* FindChainScale(GraphId nodeId) const;

    // 変更があったことを記録する。Application はこれを見て再コンパイルする。
    void MarkDirty() { ++m_revision; }
    // 入力数が可変のノード（Merge）の入力を整える。繋がった入力を順に残し、末尾に空きを 1 本置く。
    // リンクやノードを変えた後と読み込み後に呼ぶ。
    void NormalizeVariablePins();
    uint64_t Revision() const { return m_revision; }

private:
    GraphId AllocateGraphId() { return m_nextGraphId++; }
    RoadNetworkSettings m_roadNetwork;
    void RebuildNextGraphId();
    // top から「下地」チェーンを遡る（上から下の順）。
    std::vector<const Node*> ChainFrom(const Node* top) const;
    // CompileChainFrom の途中経過。マスクのプレビューで、チェーンと同じ
    // 解決（Height の起点・焼いた op の共有）を続けるために要る。
    // マスクの出どころ。**どのノードの、何番目の Mask 出力か**まで持つ。
    // 崩落のように Mask 出力を 2 本持つノードがあるので、ノードだけでは足りない。
    struct MaskSourceRef {
        const Node* node = nullptr;
        size_t outputIndex = 0;  // そのノードの Mask 出力のうち何番目か
    };
    // 焼いた op の記録。**添字は ops の添字と一致する**（必ず一緒に push する）。
    struct EmittedMaskOp {
        const Node* node = nullptr;
        int heightLayer = 0;
        size_t outputIndex = 0;
    };
    struct ChainTrace {
        // layers と 1 対 1 で並ぶ元ノード。マスクがチェーンのどこを読むかの解決に使う。
        std::vector<const Node*> layerNodes;
        // 焼いた op（添字が op の添字と一致する）。
        std::vector<EmittedMaskOp> emitted;
    };
    // 焼いた op の記録を、コンパイル結果の「出どころ」へ写す（ノードのポインタは外へ出さない）。
    static void RecordMaskOpSources(const std::vector<EmittedMaskOp>& emitted,
                                    CompiledGraph& compiled);
    static void RecordLayerSources(const std::vector<const Node*>& layerNodes,
                                   CompiledGraph& compiled);
    // top から「下地」チェーンを遡ってレイヤー列（下から上）にする共通部。
    CompiledGraph CompileChainFrom(const Node* top, ChainTrace* trace = nullptr) const;
    // マスクの木を辿って、**レイヤーでもある出どころ**（堆積 / 崩落 / 積雪）を集める。
    // Mask Levels / Mask Blend の先にいても見つける。この 3 つは
    // 「そのレイヤーを合成した時点」の作業用テクスチャから焼くので、
    // チェーンの中で走っていないと結果が残らない。
    void CollectLayerMaskSources(const Node& maskNode, std::vector<const Node*>& out,
                                 int depth) const;
    // マスクのノードを op の列へ落とす。返り値は結果の op の添字（-1 は未接続）。
    // 同じノード（かつ同じ Height の起点・同じ出力ピン）は 1 つの op を共有する。
    int EmitMaskOps(const MaskSourceRef& source, int defaultHeightLayer,
                    const std::vector<const Node*>& layerNodes, compositor::MaskProgram& ops,
                    std::vector<EmittedMaskOp>& emitted, int depth) const;
    // ノードの入力ピン（型を指定）に繋がっている上流ノード。無ければ nullptr。
    const Node* UpstreamOf(const Node& node, ValueType type, size_t which = 0) const;
    // Mask 入力に繋がっている出どころ。node が nullptr なら未接続。
    MaskSourceRef UpstreamMaskOf(const Node& node, size_t which = 0) const;
    // 出力ノードへ繋がっている一番上のノード。無ければ nullptr。
    const Node* ChainTop() const;
    // プレビュー対象（nodeId が 0 なら出力チェーン）の一番上のノード。
    const Node* PreviewTop(GraphId nodeId) const;
    // producer の出力を辿って target に届くか（循環チェック用）。
    bool ReachesDownstream(GraphId fromNodeId, GraphId targetNodeId) const;

    std::vector<Node> m_nodes;
    std::vector<Link> m_links;
    GraphId m_nextGraphId = 1;
    uint64_t m_revision = 1;
};

std::span<const NodeDefinition> NodeDefinitions();
const NodeDefinition* FindNodeDefinition(NodeKind kind);
const NodeDefinition* FindNodeDefinitionByName(std::string_view name);
// レイヤー設定を持つ種類か（サーフェス / シェイプ / 水面 / ハイトマップ）。
bool IsLayerNodeKind(NodeKind kind);
// 入力を持たないソースか。下地が無いのでマスクも効かない。
bool IsSourceNodeKind(NodeKind kind);
// マスクを出すノードか。
bool IsMaskNodeKind(NodeKind kind);
// 下地の Height を読むマスクか（川筋 / 傾斜 / 曲率 / 標高）。
bool IsHeightMaskNodeKind(NodeKind kind);
// **レイヤーでもありマスクの出どころでもある**種類か（堆積 / 崩落 / 積雪）。
// この 3 つの Mask は「そのレイヤーを合成した時点の作業用テクスチャ」から焼くので、
// 出どころがチェーンの中で走っていないと結果が残らない。
bool IsLayerMaskSourceKind(NodeKind kind);
// 道路メッシュの鎖を成す種類か（Road / Lane Marking / Decal / Shoulder / Merge）。Mesh Output は含まない。
// 出力ピンを選ぶと、そのノードまでの鎖がメッシュシーンに出る。
bool IsMeshNodeKind(NodeKind kind);
// 選ぶとプレビューの対象になる種類か。レイヤーに加えて、
// **川筋（マスクを目で見て調整するもの）**もプレビューできる。
// Path は Base に繋いだ地形（パスが沿う面）をプレビューする。
// 道路メッシュのノードはそのノードまでの鎖をメッシュシーンに出す。
bool IsPreviewableNodeKind(NodeKind kind);
// 種類に対応するレイヤー種別（レイヤー設定を持つ種類のみ意味を持つ）。
compositor::LayerKind LayerKindFor(NodeKind kind);

}  // namespace tg::graph
