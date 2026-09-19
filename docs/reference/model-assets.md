# model-assets — 3D モデル（FBX）とマテリアルスロット

作成日時: 2026-09-19 17:30
更新日時: 2026-09-19 21:58

FBX を読み込んで共有アセット `.tgmodel` にし、FBX のマテリアルごと（スロット）に共有マテリアル `.tgmat` を割り当てる。
terrain-graph のモデル機能（`ModelAsset` / `ModelPreview` / `ApplicationModelPanel`）を移植したもの。
配置（Model Scatter ノードとインスタンス描画）と、専用の「モデル」一覧パネルは持ってこない。
一覧はアセットの帯が兼ねる。ビューポートへはグラフのモデル系のノード（Model / Transform / Model Merge）で置く（下記）。

実装: [ModelAsset](../../src/renderer/ModelAsset.h)、[ModelPreview](../../src/renderer/ModelPreview.h)、
[ModelPreview.hlsl](../../shaders/ModelPreview.hlsl)、[ApplicationModelPanel.cpp](../../src/app/ApplicationModelPanel.cpp)、
[ApplicationModelPlacement.cpp](../../src/app/ApplicationModelPlacement.cpp)（モデル系のノード）。

## 読み込み

- ライブラリは ufbx（`ports/ufbx` の overlay port。terrain-graph と同じ v0.17.1）。ASCII / バイナリの FBX 6.x〜7.x を読む。
- 右手系 Y-up・メートルへ変換する（`target_axes = right_handed_y_up`、`target_unit_meters = 1`）。
  法線が無ければ生成し、アニメーションと埋め込みテクスチャは読まない。
- 面は三角形に分け、**FBX のマテリアル（`typed_id`）ごとに 1 スロット**。同じ名前でも別のマテリアルなら別スロット。
  LOD グループがあれば子の順を LOD 番号にする。
- UV の V は画像の行（下向き）へ反転する。接線は面ごとに UV から求め、法線へ直交化する。
  負のスケールで裏返った面は、変換後の法線に合わせて頂点順を入れ替える。
- 形状（`ModelGeometry`）は不変で `shared_ptr` 共有。アンドゥのスナップショットへ複製しても頂点は複製されない。

### 倍率

`.tgmodel` の `scale`（既定 1）。**単位の宣言と中身が食い違う FBX を実寸へ直す**ためのもの。
例: XNA の F16（`data/Models/f16/`）は cm と宣言した FBX に m の値が入っており、読むと全長 0.15 m になる。倍率 100 で 14.97 m。
形状には焼き込まず、寸法の表示とビューポートへの配置で掛ける。プレビューは形状の大きさで収めるので見た目は変わらない。

## マテリアルスロット

- スロットごとに `MaterialAssetId`。未割り当て（なし）は灰色の既定マテリアルで描く。
- 割り当てはモデルプレビューの窓のスロット行（コンボ、またはマテリアルのサムネイルのドラッグ）。
- **「FBX のマテリアルから作成」**: 未割り当てのスロットに、FBX のマテリアル名で `.tgmat` を作って割り当てる。
  - ベースカラーは FBX の拡散色のテクスチャ。場所は FBX からの相対パス → 書かれた絶対パス → FBX と同じフォルダの同名、の順に探す
    （書かれた絶対パスは作者の PC のものであることが多い）。テクスチャが無ければ拡散色をティントにする。
  - FBX の不透明度が 1 未満なら半透明。**テクスチャのアルファに 1 未満（250/255 未満）の画素があれば、アルファを不透明度にして半透明**
    （FBX の透明度が 1 のままアルファで抜いているガラスなど）。
  - 置き場所は `.tgmodel` の隣（未保存なら FBX の隣）。ラフネス・メタル・AO・法線は作らない（手で割り当てる）。
- マテリアルを削除したら、それを指すスロットは「なし」へ戻す。

## 描画（ModelPreview）

- 窓用は 1024px、帯の未読み込みのサムネイル用は 256px（保存は 128px）。深度付きのラスタライズ。
- **照らし方はビューポートと同じ**（適用中の天球の IBL + 太陽 + 露出 + トーンマップ）。窓とサムネイルでは影・大気は無い。
- マテリアルの合成モードを見る。マスク抜きはしきい値未満を捨て、半透明は不透明の後に両面・深度書き込みなしのアルファ合成で重ねる
  （半透明どうしの並べ替えはしない）。
- 読み込み済みのモデルは 1 つの出力を窓とサムネイルで共有する。窓に出しているものは毎フレーム、
  それ以外は 1 度だけ描き、スロットやマテリアルが変わったら描き直す。

## アセットの帯

- `.tgmodel` と `.fbx` を並べる。`.fbx` のダブルクリックは隣に `.tgmodel` を作ってシーンへ読み込み、窓を開く
  （同じ FBX を読み込み済みならそのモデルを開く）。`.tgmodel` のダブルクリックはシーンへ読み込み（参照するマテリアルも）、窓を開く。
- ルート外の FBX（エクスプローラからのドロップ、「ファイルを読み込む…」）は表示中のフォルダへコピーする。
  FBX が参照する拡散色のテクスチャも隣へコピーする。
- 右クリックの「シーンから外す」はファイルを残してシーンのモデル一覧から除く。
- 未読み込みの `.tgmodel` / `.fbx` のサムネイルは AssetThumbnailCache が一時の領域へ読んで描き、`.terrain-graph/thumbnails` へ残す。

## 保存

- シーン（版 28）の Model ノードは `graph.nodes[]` の `model: {model: models[].id | null, position: [x, y, z], rotation: [x, y, z], scale}`、
  Transform は `transform: {position, rotation, scale}`、Model Merge は設定を持たない。
- シーン（版 27）の `models[]` は `{id, name, path, scale, materials: [番号 | null]}`。`.tgscene` では `.tgmodel` へ分け、
  `{id, asset: {uid, path}}` になる。形式は [file-format.md](file-format.md)。
- `.tgmodel`（`terrain-graph.model-asset` 版 1）: `{name, uid, source: {uid, path}, scale, materials: [{uid, path} | null]}`。
  FBX は `.meta` の固定 ID で参照する。見つからない FBX はパスのまま残し、リンク切れとして読み込む（割り当ては失わない）。

## モデルの系統のノード

道路系（ピンの型 Mesh）とモデル系（ピンの型 **Model**）はノードを分け、互いには繋がらない。
Mesh Output は道路の「Mesh」入力とモデルの「Model」入力の 2 本を持つ。

| ノード | 保存名 | ピン | 役割 |
| --- | --- | --- | --- |
| Model | `model` | 出力 Model | モデル 1 つ。`{model, position, rotation[3], scale}` |
| Transform | `transform` | 入力 Model → 出力 Model | 上流のモデルをまとめて動かす。`{position, rotation[3], scale}` |
| Model Merge | `modelMerge` | 入力 Model（可変）→ 出力 Model | 複数の枝を 1 本に。入力は Merge と同じく繋ぐたびに増える |

- model はシーンのモデル一覧の ID（0 = なし）。Model の position はモデルの**底面の中心**（形状の境界ボックスの X・Z の中央、Y の最小）の位置。
- 回転は X / Y / Z 軸まわりの度で、Z → X → Y の順に回す（DirectX の RollPitchYaw）。旧データの数値 1 つは Y として読む。
- ワールド行列は 底面中心へ移す → アセットの倍率 → Model の倍率 → 回転 → 位置 → モデルに近い Transform から順に（倍率 → 回転 → 位置）。
  Transform は自分の原点まわりに効く。同じ Model を Model Merge の別の入力や別の Transform から繋げば、その数だけ出る。
- `CollectOutputModels` が Mesh Output の Model 入力から Transform / Model Merge を辿り、Model ごとに通った Transform（近い順）を返す。
  プレビュー中のノードがモデル系ならその枝だけ（道路は出さない）、道路系ならモデルは出さない。
- **置き方の変更（位置・回転・倍率・モデル）はグラフの改版を起こさない**（道路を作り直さない）。描画は毎フレーム設定から行う。
  リンクの変更は従来どおり改版する。設定はノードの一部なのでアンドゥ・コピーの対象。
- 置き方の入口:
  - アセットの帯のモデル（`.tgmodel` / `.fbx`、読み込み前でもよい）をビューポートへドラッグして落とす。落とした所の道路メッシュの面（変位前）、
    当たらなければ地面 y = 0 に、Model ノードを作って置く。複数を落とすと X へ 2 m ずつずらす。
  - モデルプレビューの窓の「ビューポートに置く」（カメラの注視点の真下 y = 0）。
  - グラフの右クリックメニューの「モデル」の項（Model はモデルプレビューで選んでいるモデルが最初から入る）。
  - 作った Model は Mesh Output の Model 入力へ繋ぐ。Mesh Output が無ければ作り、別のものが繋がっていれば Model Merge を挟み、
    既に Model Merge なら空きの入力へ繋ぐ。
- ビューポートの操作（Path ノード未選択のとき。視点は従来どおり Alt）:
  - モデルのクリックでその Model ノードを選ぶ（グラフエディタの選択も合わせる）。本体のドラッグは水平移動。
  - 選んだ Model / Transform のギズモ。**W で移動**（X / Y / Z の矢印と YZ / XZ / XY の平面ハンドル）、**E で回転**（X / Y / Z の輪。Ctrl で 15 度刻み）。
    ギズモの軸はワールド軸で、下流に Transform があればその座標へ戻して値に足す（回転は R' = R・P・Ra・P の逆 を角度へ戻す）。
    ギズモは画面上で一定の大きさ（90px）、ImGui で重ね描きし深度は見ない。
  - Esc でドラッグ前へ戻す / 選択解除、Delete でノードごと削除、F で寄る。設定はグラフパネルのプロパティ欄（モデル / 位置 / 回転 / 倍率 / 寸法）。
- 範囲の枠（モデルの向きに沿った境界ボックス）は `PreviewRenderer::SetOverlayLines` でレンダラが描く。シーンの深度でテストするので奥は隠れる。
  選んだ Model はそのモデル、選んだ Transform はその枝のモデルすべて（`ImGuiCol_PlotLinesHovered`）、ホバーは `ImGuiCol_PlotLines`。
- 描画は `PreviewRenderer::drawSceneExtras` から `ModelPreview::RenderInScene` を呼ぶ。本描画は不透明の道路の直後
  （路面の帯と半透明の帯の前）で線形 HDR を書き、道路と同じカスケードシャドウの影を受ける。
  各シャドウカスケードにも深度を書くので、道路へ影を落とす（半透明のパーツは落とさない）。
  シェーディング / クレイ表示でだけ描く（チャンネルを覗く表示には出ない）。出すモデルを包む球（`SetExtraSceneRadius`）を影の範囲とカメラの距離に含める。
- モデルをシーンから外すと、それを指す Model ノードは「なし」になる（ノードとリンクは残す）。
- 描くメッシュはモデルプレビューと共有しているので、窓で表示 LOD を変えるとビューポートもその LOD になる。
- 選択の判定は CPU で、境界ボックス → LOD 0 の三角形の順に調べる。

## 開発用オプション

- `--import-model <fbx>`: FBX を取り込み、「FBX のマテリアルから作成」まで行ってプレビューを開く。
- `--open-asset <path>`: ルート内のアセットを帯のダブルクリックと同じ経路で開き、そのフォルダを帯に出す。
- `--place-model <path>`: モデル（`.tgmodel` / `.fbx`）の Model ノードを作って原点へ置く（帯からビューポートへ落としたのと同じ経路）。
- `--gizmo-rotate`: ギズモを回転（E）で始める。`--select-node <id>` と合わせて回転ギズモを撮る。

## 未対応（後続）

- モデル系のノードで Path / Road に沿って置く・並べる（ガードレールの支柱や標識）。倍率のギズモ。
- UV2（ライトマップ用の 2 つ目の UV）。F16 はライトマップも UV1 なので AO として使えている。
- スキンメッシュ・アニメーション、埋め込みテクスチャ、FBX 以外の形式（glTF / OBJ）。
