# progress — Road Editor の進捗と注意点

作成日時: 2026-08-31 05:46
更新日時: 2026-09-07 15:54

完了した作業は新しい順に並べる。受入条件と実装順序は [plan.md](plan.md) を参照する。

## 現在の状況

道路・道路付属物エディタへの方針転換と改名を行い、R1 の独立メッシュ基盤を実装した。
R2 は実寸 Path の編集、一定幅の道路メッシュ生成、左右境界の出力、道路 UV での材質表示まで実装した。
R3 は白線（中央線と外側線の帯ポリゴン、矢印、摩耗マスク）を実装した。R2 の縦断曲線と、道路線形のバンク角（自動 / 手動、回転ギズモ）を実装した。
道路材質のレイヤー構造（材質スロット × 4 ＋ Road Mask、混ぜ方、下地のハイトで絞る）、テセレーション・変位、車線数と破線・停止線、路肩（Shoulder）、Merge、Decal、ひび割れ（Crack）を実装した。付属物（R5）は未着手。保存形式は版16。旧地形の仕組み（地形ノード、侵食・マスク・ペイントの合成パス、平面プレビュー）は 2026-09-07 に撤去した。

## 次にやること

- 最優先: [道路・沿道プリセット・区間配置・自動接続への移行計画](roadside-presets.md) の P1。P0は水平直線の4層＋4層＋歩道、GPU計測、サンプル点での変位後誤差検証まで完了。次にBoundaryContract・断面・安定ID・区間・埋込プリセットの型と保存・Undoを導入する。
- 道路本体も区間プリセットの対象に追加。新舗装→荒れた舗装→砂利道を連続変化させ、沿道と異なる区切りで配置する。保存・移行・P0〜P5の受入条件へ反映済み。水平直線での路面移行を試作済み。
- 以下の R4 拡張は自動接続の基盤が固まった後に統合する。

- Decal の道路マスクへのスタンプ。縁石など R5 の断面（路肩の仕組みの延長）。設計は docs/design/road-material-layers.md「路肩との接続」。
- ひび割れの第二段: 頂点アルファで枝を先端へ薄くする、枝用の別材質、生成結果を面上の Path へ書き出す「Path へ変換」。詳細は plan.md の R4「ひび割れデカール」。
- R2 の残り。遠く離れた区間同士の自己交差検査。縦断・バンクの手動操作（Ctrl＋クリック、ドラッグ、リング）の実機確認。
- 設計で確定する項目（型と所有権、区間キャッシュ、道路データの保存形式）は plan.md の「次の設計で確定すること」を参照する。

## 完了した作業

### 2026-09-07 15:54 — P0のGPU計測と継ぎ目のサンプル検査

- `--measure-preview` でGPUタイムスタンプ・VRAM・CPU生成時間をログへ出す。検査はフレーム同期に従って読み戻す。
- 生成時の隣接辺を両側別々に変位し、24 mの約22万点と50 mの約27万点で0.1 mm超0点。入力側へ1 mmずらす確認用の辺も検出した。凹凸を持つ歩道も検査済み。
- Releaseの50 m固定視点で、通常道路4層は平均0.037 / 0.040 ms、接続試作9層は0.663 / 0.617 ms。描画面積・形状数も異なる比較。詳細と限界は [GPU計測資料](../reference/connection-diagnostics.md)。
- Debug / Releaseのビルド・全テスト、検査CSのコンパイルと実行、画像の目視確認が成功。起動ログに警告・エラー無し。
- P0の固定シーンで最小実証できたためP1へ進む。全テセレーション頂点・全視点や曲線での保証ではなく、これらはP2へ引き継ぐ。

### 2026-09-07 15:29 — 接続試作で材質構成を保持

- 接続元のRoadと接続先のSurface / Road / Shoulderから、最大4層の材質構成を別々に評価する。4層＋4層＋仮歩道1層を検証。
- プリセット内のマスク・UV反復と向き・高さ条件・混ぜ方・変位量を保持し、組同士は共通接続マスクで合成する。法線はRNM。
- 材質評価だけの内部エントリは形状を持たず、描画は共通接続面のみ。既存のGPU評価器・遅延解放を共有する。保存形式は変更しない。
- Debugビルド・全テスト・VS/PS/DSのDXCコンパイルが成功。実素材による全体・近景と通常道路の描画を目視確認し、起動ログに警告・エラー無し。資料は [接続試作](../reference/connection-prototype.md)。
- GPU時間・VRAM・境界誤差の数値計測、曲線対応、区間編集と部品一覧は継続課題。

### 2026-09-07 15:13 — 接続の最小試作と仮コンクリート歩道

- 開発用オプション --connection-prototype で、舗装→砂利への進行方向の変化、砂利路肩との横接続、15 cm上がる仮歩道を生成する。入力グラフは変更しない。
- 歩道は1.5 m間隔・幅12 mm・深さ4 mmの目地を実メッシュで生成。接続部の共通位置とUV、世界Y方向の共通変位、RNMでの法線合成を試す。
- Debugビルドと全テスト、MeshPbrのDXCコンパイルが成功。全体・近景・変位なしの画像を data/connection-prototype/ に出力。
- 部品アセット一覧をP3〜P4へ計画追加。断面／反復／端部をプリセットから参照する。詳細は [試作資料](../reference/connection-prototype.md)。
- 限界: 直線・水平、各素材は単一Surface、最大3素材。曲線、区間編集、部品ライブラリ、標示の追従、GPU誤差・フレーム時間・総VRAM計測は未実施。P0全体は継続。

### 2026-09-07 14:31 — レビューで見つかった履歴とグラフ復元の修正

- 材質の明度・彩度・色相・法線の緑反転をアンドゥの記録と復元へ追加。材質を削除して取り消した場合も元の設定を保持する。
- Merge の未接続ピンの ID を無関係な接続変更や保存データ・アンドゥの復元時に保持する。
- グラフ復元時に循環・自己接続・同じ入力への複数接続を除去し、最初の有効な接続を保持する。
- 検証: グラフの回帰テストは修正前の失敗を確認し、修正後は Debug ビルドと全テストが成功。Merge を含む実プロジェクトの保存・再読込・再保存でファイルが完全一致した（data/review-check/）。
- 材質のアンドゥの画面操作と描画結果の目視確認は未実施。

### 2026-09-07 14:40 — 旧地形の平面プレビューの撤去（第二段の 3）

- 36 ファイル、−3075 行。`PreviewRenderer` から平面メッシュ・`PlaneSize` / `DisplacementScale` / `UseMaterialTextures` / 平面用の評価器 / ペイント用 UV の描画先 / 手入力シーンを削除。`Render()` は `MaterialStack` を受けない。シーンが無ければ影パスを飛ばして天球とグリッドだけ描く。
- `SyncGraphStack` / `m_graphStack` / 2D 書き出し（`MaterialExport`、`ExportPack.hlsl`、`--export`）/ 経路探索（`PathRoute`、`CsDownsample` のハイト読み戻し、`HashStackHeightState`）を削除。`PathSettings::worldSpace` を廃止（常に実寸）。
- `IsPreviewableNodeKind` はメッシュノードだけ。Surface のサムネイルは MaterialLibrary のもの。「合成解像度」は道路の材質タイルの解像度として「道路」節に残す。
- 確認: `road-profile-check/gate0` の変更前後の差 0.02% 未満、sample_road / merge / stop の起動、プロジェクト無しの起動（クラッシュ無し）、テスト全件成功、`--save-project` の往復。
- 第二段はこれで完了。残る旧仕様は Path の点の幅 / フェザー / 強さ（保存と初期値のみ。マスク用途は無くなったので次で消せる）。

### 2026-09-07 14:00 — 旧地形の合成パスとマスク・ペイントの撤去（第二段の 2）

- 48 ファイル、−10601 行。`MaterialEvaluator` は 4064 → 821 行（1 層の合成、レイヤーのサムネイル、ハイトの読み戻し、非同期コンピュートを残す）。`MaskGraph` / `PaintMask` と侵食・マスク・ペイントのシェーダを削除。
- `MaterialLayer` は Surface の項目だけ（名前、有効、書き込みチャンネル、PBR 定数、ハイトのソース / 基準 / 強さ / ノイズ、材質、UV スケール）。`LayerKind` は廃止。
- `ValueType::Mask` を廃止し、Surface のピンは Result だけ。`CompileLayersTo` はその Surface の層 1 枚を返す。`SaveProject` から未使用の `rhi::Device&` を外した。
- 確認: 変更前後で `road-profile-check/gate0` のビューポートの差は 0.02% 未満（`ui_gate0_b2_compare.png`）。テスト全件成功、起動時のシェーダのコンパイルも通る。
- 残した: `PreviewRenderer::m_materialUv`（ペイント用だった UV の描画先）と MeshPbr の SV_Target1。次の B-3（プレビュー平面の撤去）で一緒に消す。

### 2026-09-07 13:00 — 旧地形ノードの撤去（第二段の 1）

- `NodeKind` を 10 種に絞り、旧ノードのピン表・設定構造体（`MaskNodeSettings` / `OutputNodeSettings` / `TerrainScale`）・マスク op の生成（`EmitMaskOps` 系）・プロパティ UI・保存読込・テストを削除（10 ファイル、−2775 行）。
- `CompileLayers` は Surface の鎖だけを評価する。`CompiledGraph::maskOps` と `MaterialStack::MaskOps()` は常に空だが、合成器の API はまだ触っていない。
- Surface の Mask 入力ピンは残っている（未接続なら隠す）。`ValueType::Mask` を作るノードは無い。
- 確認: sample_road / sample_road_crack / sample_road_merge を起動して描画が変わらないこと、テスト全件成功。
- 次（第二段の 2）: 合成器の侵食・河川・水滴・散布・マスク op のパスとシェーダ（CompositeSediment / Crumbling / Snow / River / Droplet / Scatter / Fluvial / Mask / MaskOps / MaskPath）、`compositor::LayerKind` の Surface 以外、ペイントマスク。プレビュー設定の「合成結果」節。

### 2026-09-07 12:30 — 旧地形の仕様を UI から外す（第一段）

- 追加メニューを道路のノード ＋ Path ＋ Surface に絞った。旧地形ノードの種類・読込・評価は残っている（第二段でコードごと撤去する）。
- Surface の「UV スケール」「ハイト」節を出さない。`BuildStack` が Road / Shoulder 向けの層で `heightSource = Texture`、`heightBase = 0.5`、`heightGain = 1`、`uvScale = 1` に揃える（新規 Surface の既定がノイズのハイトだった落とし穴を塞ぐ）。
- レイヤーノードの未接続 Base ピンを隠す。
- 第二段（未着手、要確認）: 旧地形ノードの種類・設定・評価パス・シェーダ・テストの削除。プレビュー設定の「合成結果」節と 2D 合成プレビューの撤去。保存形式は「知らない種類は読み飛ばす」で互換を保てる。

### 2026-09-07 12:00 — 混ぜ方「マスクどおり」と旧マスク入力の整理

- `layerBlendMode`（×4）を Road / Shoulder / SceneMesh / MeshConstants に通し、`LayerHeightBlend` で「マスクどおり」のスロットは被覆率をそのまま重みにする（境界は下地とのハイト差 × ブレンド幅）。残りを下地とハイト競合のスロットで分ける。
- 読込はキーが無ければ 1（ハイトで競合）で以前の見た目を保つ。新規ノードは 0。
- グラフのレイヤーノードは未接続の Mask 入力ピンを出さず、レイヤーパネルのマスク節は設定済みのときだけ出す。
- 確認画像 `data/road-profile-check/ui_blendmode_compare.png`（上: ハイトで競合、下: マスクどおり。同じ Road Mask）。

### 2026-09-07 11:30 — 材質スロットを下地のハイトで絞る

- `RoadNodeSettings` / `ShoulderNodeSettings` に `layerHeightGate / Threshold / Softness`（×4）。`SceneMesh` と `MeshConstants`（末尾に uint4 + float4 ×2）へ通し、`MeshPbr.hlsl` の `ApplyHeightGate` がピクセルと `BlendedHeightLevel` の両方で被覆率に掛ける。
- `DrawMaterialSlotRows` にスロット 2〜4 の「下地のハイト」コンボとしきい値・柔らかさ。保存はキー追加のみ。テスト 1 件。
- 確認画像 `data/road-profile-check/ui_gate_compare.png`（上: 使わない、中: 下地の高い所、下: 下地の低い所。`gate0 / gateHigh / gateLow.tgproj`）。端の砂利が下地の凹凸に沿って減る。

### 2026-09-07 11:00 — Road Mask のワールドノイズ

- `RoadMaskShape::WorldNoise`。`EvaluateRoadMask` にワールド座標（`worldX` / `worldZ` / `hasWorld`）を通し、`BakeRoadMask` が `RoadGeometry` から行の左右端を補間してテクセルのワールド座標を出す。`AttachRoadLayers` は Road / Shoulder とも `&chain.road` を渡す。
- 保存は形の名前 `worldNoise` の追加のみ。テスト 3 件。実機の目視はしていない。
- 次: スロット 2〜4 に「下地のハイトで絞る」（ピクセルシェーダで下地のハイトをしきい値にして被覆率に掛ける）。

### 2026-09-07 10:40 — 材質の「明度」

- `MaterialAsset::brightness`（UI は「明度」。並びは 明度・彩度・色相）。`AdjustBaseColor` の最後に掛けて `saturate`。合成（colorAdjust.z）・サムネイル・球の定数に同じ値を渡す。保存はキー追加のみ。
- 確認画像 `data/ui_bright_compare.png`（上: そのまま、下: 道路の下地を 2.5 倍）。

### 2026-09-07 10:00 — Crack ノード（ひび割れの自動配置、R4 第一段）

- `CrackNodeSettings` と `BuildCracks`（`graph/Road.cpp`）。帯生成を `AppendSurfaceStrip`（道路座標の折れ線 ＋ 点ごとの幅）に括り出し、Decal も同じ関数を使う。
- 鎖の評価では Decal と同じ扱い（道路の押し出しに追従、深度バイアスの帯パス）。保存形式は版16。
- 確認: `data/sample_road_crack.tgproj`（sample_road.tgproj の crack 材質を Material に繋ぎ、密度 12、混合、UV の向き = U）。
  `data/ui_crack_tint.png` は同じ材質のベースカラーを赤に染めて 4 m から撮ったもので、枝分かれした細い割れ目がマスク抜きで出ている（`data/sample_road_crack_tint.tgproj`）。
- **注意（材質側）**: crack 材質そのままだと割れ目がほぼ見えない。マスクの白い帯は幅の 11% しかなく、残るのはベースカラーの割れ目の内側（暗い灰）だけで、
  この路面の暗さと差が無い。材質の問題であって生成やマスク抜きの問題ではない（一様な白のマスク、10% / 20% / 40% の帯、赤染めで切り分けた）。
  割れ目の内側をもっと暗くする、ハイトで凹ませて陰を作る、または帯の幅を 3〜4 cm にして割れ目が帯幅いっぱいに来る素材にする、のどれかが要る。
- 残り: 頂点アルファ（枝の先薄れ）、枝用の別材質、「Path へ変換」。

### 2026-09-07 09:30 — 路肩の段差（面取り列）

- `ShoulderNodeSettings::stepHeightMeters / stepWidthMeters`。`BuildShoulder` の列を「境界、面取り列（段差があるとき）、以後約 1 m 刻み」の横位置の並びで作り、面取り列から先を段差ぶん下げる。
- 保存はキー追加のみ。テスト 3 件。実機の目視はしていない。
- 路肩ノードは設計の項目（頂点共有・断面・端の層）まで一通り揃った。残りは縁石など R5 の断面。

### 2026-09-07 09:10 — 停止線

- `PathPoint::stopLine`（`PathStopLine`）。`BuildRoad` が点を最も近い行の間へ射影して `RoadGeometry::stopLines` に実距離で持つ。
- `BuildStopLines`（`graph/Road.cpp`）がその向きの車線の幅いっぱいに帯を置く。進行方向は手前側 [d − 幅, d]、対向は [d, d ＋ 幅]。
- 点のプロパティに「停止線」コンボ、Lane Marking に「停止線」と「停止線の幅」。保存はキー追加のみ。
- 確認画像 `data/ui_stop.png`（`data/sample_road_stop.tgproj`。Path の両端に「両方」）。手前の端に進行方向 2 車線ぶんの帯が出る。

### 2026-09-07 08:50 — 轍を車線に合わせる

- `RoadMaskNodeSettings::tracksFromLanes`。`EvaluateRoadMask` / `BakeRoadMask` / `AttachRoadLayers` に `const RoadLanes*` を通し、Road は車線の並びを渡し、Shoulder は nullptr。
- 読込はキーが無ければ偽（旧ファイルの見た目を変えない）。新規ノードは真。

### 2026-09-07 08:30 — 車線数と車線境界線（破線）

- `RoadNodeSettings::lanesForward / lanesBackward` と `ComputeRoadLanes`（`graph/Road.h`）。車線の中央・向き・中央線の位置・破線の位置を Right 端から並べる。
- `BuildRoadMarkings` は中央線を `centerLateral` に、車線境界線を `dividers` に破線（`BuildDashedStrip`）で引く。矢印は車線ごと（頭の幅は車線幅から）。
- 確認画像 `data/ui_lanes.png`（`data/sample_road_lanes.tgproj`。幅 9 m、進行方向 2 ＋ 対向 1）。
- 残り: 停止線。Road Mask の轍を車線数から自動で置く。

### 2026-09-07 08:00 — Shoulder の材質レイヤーと変位（路肩の第二段）

- `ShoulderNodeSettings` に displacementMeters / layerWorldUv / layerUvRepeatMeters / layerBlendRange を追加し、ピンを Road と同じ Material 1〜4 ＋ Mask 2〜4 にした。`BuildShoulder` が `RoadGeometry::settings` へ写し、鎖の評価で `AttachRoadLayers` をそのまま使う。
- 材質スロットの行を `Application::DrawMaterialSlotRows` に括り出し、Road と Shoulder で共用。
- 路肩の横位置は境界が Right、外側が Left。Road Mask の「側」はその向き。既定の座標はワールド XZ。
- 残り: 段差の面取り列。境界のハイト一致は「同じ材質・ワールド XZ・同じ変位量」を繋ぐ運用で担保する（自動で揃える仕組みは無い）。

### 2026-09-07 07:40 — Road Mask の端の減衰に「側」

- `RoadMaskSide`（両側 / 左 / 右）を `RoadMaskNodeSettings::edgeSide` に追加。横位置は正が Left なので、左は `halfWidth - lateral`、右は `halfWidth + lateral` で端からの距離を取る。保存はキー `edgeSide` の追加のみ（版は 15 のまま）。
- 路肩の第二段（端の層）は、この左右別の端マスクと左右の Shoulder の材質を境界で一致させる前提で組む。

### 2026-09-07 07:20 — Merge ノード

- `NodeKind::Merge`。入力は可変長で、`NodeGraph::NormalizeVariablePins` が「繋がった入力＋空き 1 本」に整える（リンク作成・削除、ノード削除、`Replace` の後）。読込は定義の 1 本に加えてファイルにある分だけ入力を足す。保存形式は版15。
- 鎖の評価に `MeshChain::sources`（メッシュを作ったノード ID）を足し、`MergeChainMeshes` で同じノード由来のメッシュを 1 回だけ積む。`CompileMeshGraph` も全 Mesh Output を通してこの重複除去を使う。
- 確認画像 `data/ui_merge.png`（`data/sample_road_merge.tgproj`。道路＋白線＋デカールの枝と左右の Shoulder を Merge に集め、Mesh Output は 1 つ）。
- 同じ Road が 2 枝に入ったときの重複除去は済んだので、前の項の注意は解消。

### 2026-09-07 06:50 — Shoulder ノード（路肩の第一段）

- `ShoulderNodeSettings`（幅、横断勾配 %、UV 反復長、UV の向き）と `BuildShoulder` / `EvaluateShoulder`（`graph/Road.cpp`）。境界の列をそのまま列 0 に写し、隣の列から外向きを決めて押し出す。列末尾が Outer。
- Path 入力の上流は Road の Left / Right か Shoulder の Outer（`EvaluateBoundarySource`）。Road の Path 入力も Shoulder の Outer を受ける。
- 鎖の評価では Shoulder は Road とは別の枝（chain.road が路肩の格子になる）。道路と一緒に出すには Merge に集める。保存形式は版14。
- 確認画像 `data/ui_shoulder.png`（`data/sample_road_shoulder.tgproj`、左右 1.5 m、4%）。境界は水密。
- 残り（第二段）: 端の層（道路側の材質を境界からの距離で減衰）、段差の面取り列、ワールド XZ 座標の材質、道路の押し出し量との段差の扱い（道路が変位 0.05 m で路肩が 0 だと境界に段ができる。端の層で同じハイトを読ませて解く）。

### 2026-09-07 06:10 — 道路メッシュの部分描画と途中経過の表示

- `EvaluateMeshChain` の戻り値を「道路面が出来たか」にし、白線・Decal の失敗は理由だけ残して上流までを積む。Decal の Path を空にしても道路が消えない。
- `CompileMeshGraph(graph, previewNodeId)` で Road / Lane Marking / Decal までの鎖だけを出せるようにし、出力ピンのクリックをメッシュノードにも広げた（`IsMeshNodeKind`）。2D 合成のプレビュー対象からはメッシュノードを除く。
- Merge ノードは 07:20 に実装（上の項）。

### 2026-09-07 05:24 — 面上のパスの修正

- 面上かどうかをリンクで決めるようにし、道路の評価に一時的に失敗しても座標の意味を変えない（実寸へ戻して再変換すると点が壊れる）。
- Surface を繋いだときの変換で、道路の外にあった点は横位置を道路幅の内側へ寄せる。道路の外の点は面座標で表示できないため。
- 道路の行ごとの実距離を `RoadGeometry::rowDistances` に持ち、白線・矢印・デカール・面座標の変換が UV の向きに依存しないようにした。
- ユーザーの `data/sample_road.tgproj` の面上パス（節 50）は道路の外の座標になっていたので端へ寄せた（元は `sample_road.tgproj.bak2`）。

### 2026-09-07 05:11 — 面上のパスと Decal ノード（R4）

- Path の Surface 入力（Mesh 型）と `surfaceSpace`。繋いだ時点で世界座標の点を面の座標へ変換。`RoadSurfaceCoordinates` / `RoadSurfacePointAt` / `RayHitsRoad` / `FindSurfaceRoad` を Road.h に追加。
- Decal ノード（`BuildDecal`）: 面上の Path に沿った帯を約 0.25 m で刻み、道路 UV を持って路面と一緒に押し出す。深度バイアスの帯パスで描く。
- 編集: 路面へのレイピッキングで点を置き、路面に沿ってドラッグ。面上のパスでは世界軸ギズモと縦断 / バンク編集を出さない。プロパティは横位置 / 高さ / 距離。
- 版13。Debug / Release ビルド・テスト（面座標・レイ・デカール・グラフ 12 項目を追加）が成功。
- `data/road-profile-check/decal.tgproj` / `decal-ui.png` / `decal-path-ui.png` で、車線を斜めに横切る白い帯（マスク抜き）と、路面に沿った Path の表示を確認。
- 未検証: マウス操作での点の追加・ドラッグ、Surface を外したときの座標の扱い（数値をそのまま実寸として残す）。

### 2026-09-07 04:34 — 道路の材質スロットと Road Mask

- Road Mask ノードと道路空間マスク（`graph/RoadMask.h/.cpp`）。轍 / 端の減衰 / 長さ方向ノイズ / 一様、ムラ、強さ、反転。RGBA8 に焼いて GPU へ転送。
- Road のピンを Material 1〜4 / Mask 2〜4 に拡張。スロットごとの座標（道路 UV / ワールド XZ）・反復長、ブレンド幅。版12。
- 描画: スロットごとに評価器を持ち、`LayerCoverage` / `LayerHeightBlend` でハイト競合。変位と白線の押し出しもブレンド後のハイト。
- Debug / Release ビルド・テスト（マスクとスロット 14 項目を追加）が成功。保存往復一致。
- `data/road-profile-check/layers.tgproj` / `layers.png` で、端の砂利（ワールド XZ、ムラ付き）と轍のアスファルトが混ざることを確認。
- 注意: ExecuteImmediate は転送専用なので、道路マスクの読み取り状態への遷移は描画側のコマンドリストで行う。

### 2026-09-07 02:59 — 白線の UV の向きと深度バイアス

- `GraphicsPipelineDesc` に深度バイアスを追加し、帯（`useBlendMode`）を路面の後に −2000 / −2.0 のバイアスで描く。影とワイヤーフレームは路面と帯を一緒に描く。
- Road / Lane Marking に `uvAlongU`。帯の道路 UV は Road の設定に合わせて入れ替える。テスト 3 項目追加。
- 不透明度マップを付けた材質が「不透明」のままなら「マスク抜き」へ自動切替（マテリアルパネル）。
- ユーザーの `data/sample_road.tgproj` を確認。白線材質が不透明のままだったのでマスク抜きへ、白線の UV を長さ方向 = U・反復長 1.2 m に変更した（元は `sample_road.tgproj.bak`）。
  変更前後は `data/road-profile-check/user-sample.png` / `user-sample-fixed.png`。穴は路面の押し出しが帯を突き抜けていたもので、深度バイアスで解消した。

### 2026-09-07 02:37 — 材質の不透明度と合成モード

- MaterialAsset に不透明度マップ・定数・BlendMode・しきい値を追加。保存（プロジェクト / .tgmat）、アンドゥ、マテリアルパネルに対応。
- 合成器は Surface の A に不透明度を書く（空いていた LayerConstants の paintParams.y / maskCurve.y を使用）。
- 帯メッシュ（Lane Marking）は一番上のレイヤーの材質のモードで描く。マスク抜きは discard、半透明は不透明の後にアルファ合成（深度書き込みなし、影なし）。
- Debug / Release ビルド・テスト成功。保存往復一致。`data/textures/paint_wear_mask.png`（生成した摩耗マスク）を使い、
  `data/road-profile-check/paint-masked.png` / `paint-translucent.png` で剥げた白線を確認。
- 起動時の MaterialThumbnail の GPU barrier layout 警告が材質 2 つの構成で再現した（plan.md の既知課題）。今回は未対応。

### 2026-09-07 02:26 — 白線の路面追従（道路 UV）

- 頂点形式に道路 UV（TEXCOORD1）を追加し、白線と矢印の頂点へ路面上の位置を持たせた。保存形式の scene は 12 値のまま。
- 白線メッシュは `displacementSource` で道路面の材質 Height を道路 UV で読み、同じ量だけ押し出す。ドメインシェーダでも同じ。
- テストに道路 UV の一致を追加。`data/road-profile-check/displace-lines.png` で変位 0.25 m の路面に白線が沿うことを確認。

### 2026-09-07 02:17 — テセレーションの細かさ

- 分割の上限を 64（ハードウェア上限）まで、分割する辺の長さを 4〜32 px で設定できるようにした。プロジェクトに保存する。
- `data/road-profile-check/tess64-ui.png`（上限 64、6 px、寄り）で砂利の粒が形として出ること、`tess64-far-ui.png` で遠景の分割が減ることを確認。
- Debug ビルドの計測では近景・遠景とも約 4 FPS で、分割数ではなく Debug 構成と 2880×1620 の描画が支配的。Release での計測は未実施。

### 2026-09-07 01:42 — 道路のテセレーションとワイヤーフレーム表示

- メッシュシーンで HS / DS を有効にし、Road ノードの「変位量」を `SceneMesh::displacementMeters` として頂点 / ドメインシェーダの押し出しへ渡す。Height は道路 UV で wrap。
- 「表示 > ワイヤーフレーム（分割後）」を追加。本描画と同じ VS / HS / DS と `PsWireframe` で、トーンマップ後に線を重ねる。設定は settings.json。
- Debug / Release ビルド・テストが成功。`data/road-profile-check/tess.tgproj` の UI 画像（`tess-ui.png`）で分割の上限 16 のパッチが出ることを確認。
- `data/textures` の砂利材質で変位 0.25 m を確認（`data/road-profile-check/displace-on.png` / `displace-wire.png`）。Surface ノードのハイトのソースを「テクスチャ」にしないと材質のハイトは合成に入らず、変位も効かない。
- 白線の帯は道路 UV で路面と同じ量を押し出すようにした（後述）。

### 2026-09-07 01:29 — 走行側と進行方向の矢印

- `RoadNetworkSettings.leftHandTraffic` を NodeGraph に追加し、`graph.roadNetwork` へ保存、DocumentSnapshot に含めてアンドゥ対象にした。
- プレビュー設定「道路」で切り替え、Road / Lane Marking のプロパティに表示。Lane Marking に矢印（間隔・長さ）を追加。
- Road の Left / Right が右手系で逆だったのを修正（列 0 = Right）。バンクの回転と曲がり向きの符号もこれに合わせた。
- Debug / Release ビルド・テスト（矢印と走行側 10 項目を追加）が成功。保存往復で JSON が一致。
- `data/road-profile-check/traffic-left.tgproj` / `traffic-right.tgproj` の UI 画像で、左右の車線の矢印が走行側で反転することを確認。
- 注意: ユーザーの settings.json で作業グリッドがオフになっていたため、検証画像にはグリッドが写っていない。

### 2026-09-07 00:44 — 縦断曲線とバンク角

- road-editor（D:/GitHub/road-editor）の縦断曲線とバンク角の計算を `src/graph/RoadProfile.h/.cpp` へ移植。
  ポイントは Path が持ち、Road が生成時に反映する。境界 Path と Lane Marking はそのまま追従する。
- 縦断: u・VCL・オフセットのガイド点を放物線でつなぐ。バンク: 曲率半径（前後 10 m）・設計速度・摩擦係数の自動角と
  手動角の補間、任意のガウス平滑化。符号は正で Left 側上がり。
- UI: 「道路線形 > 編集」で制御点 / 縦断 / バンクを切り替え。縦断反映後の中心線とマーカーを重ね、
  Ctrl＋クリックで追加、線に沿ったドラッグで移動、Delete で削除。手動バンクは断面平面の回転リングで角度を回す。
- 保存形式は版11。`--profile-mode` / `--select-path-point` で検証用の状態を再現できる。
- Debug / Release ビルドとテスト（縦断・バンク 20 項目を追加）が成功。保存→再読込→再保存で JSON が一致。
- `data/road-profile-check/profile.tgproj` で縦断の盛り上がり、自動 43.7°・手動 −25° のバンク、リングをUI 画像で確認
  （`profile-mode1-ui.png` / `profile-mode2-ui.png`）。Release は `road_editor_marking.exe` へ出力。
- 未検証: マウス操作（追加・ドラッグ・リング回転）、線形の端で自動角が 0 から急に立ち上がる見え方（平滑化で緩和できる）。

### 2026-09-07 00:20 — 白線ノード・表示メニュー・ライトギズモ

- Lane Marking ノードを追加。Road の格子の行ごとに左右端から横位置を補間した帯ポリゴンで、中央線と左右の外側線を生成する。
- 出力は道路面と白線をまとめた RoadSurface。Mesh Output は上流をたどって Road を起点に部品を積む。白線には 1 m グリッドを重ねない。
- 線幅・端からの距離・浮かせ量（法線方向、既定 5 mm）・UV 反復長を設定。U は帯の幅 0〜1、V は実距離 ÷ UV 反復長。Material 入力で塗料の材質を接続できる。
- 保存形式は版10。「表示 > ハイトの範囲」と設定 showHeightGuide を撤去し、レンダラのガイド描画は作業グリッド専用にした。
- ライトギズモをカメラの注視点中心、半径 = 距離 × tan(縦画角/2) × 0.45 に変更した。
- Debug / Release ビルドとテスト（白線 19 項目を追加）が成功。`data/road-marking-check/marking.tgproj` で中央線と左右の外側線が S 字カーブに追従することを UI 画像で確認。
- 保存→再読込→再保存で JSON が一致（版10）。Release は起動中の exe を避け `road_editor_marking.exe` へ出力した。
- 未検証: 白線への材質接続の目視、L＋ドラッグ時のライトギズモの見え方、表示メニューの手動操作。

### 2026-09-06 23:41 — Path の移動ギズモ修正

- カメラに向いた短い軸を一定長へ引き伸ばす処理を廃止。全軸に共通倍率を使い、不自然な向きの強調を防ぐ。
- 軸の方向と 1px あたりの移動量を投影位置での微分から計算し、近接時のプローブ点の消失と移動倍率のずれを解消する。
- Debug / Release ビルド・テストが成功。浅い角度・斜め・真上付近・カメラ背面を数値検証。
- 3視点の UI 画像を確認し、Release 版へのドラッグ入力で Y のみ 0 から約 0.566 m へ移動することを確認。
- 検証資料は `data/gizmo-check/`。起動中の Release 版を避け、修正版は `build/bin/Release/road_editor_gizmo.exe` へ出力した。

### 2026-09-06 16:20 — 道路マテリアル

- Road の Material 入力に Surface などの Result を接続し、既存の材質合成を道路の実距離 UV で表示する。未接続の道路は単色。
- 各道路の色・法線・ラフネス・メタルネス・AO を評価し、材質編集・画像再読込を反映する。道路形状のハイト変位は対象外。
- 保存形式は版9。旧 Road の入力・出力 ID を維持し、Material ピンのみ追加する。
- Debug / Release ビルド・テスト、DXC の VS/PS 単体コンパイルが成功。アスファルト材質の UI 画像、保存往復一致、版8からのリンク・ID 維持を確認。通常の Release 実行ファイルも更新済み。
- 確認用コピーは `data/sample_road_material.tgproj`、画像・ログは `data/road-material-check/`。素材編集・再リンクの手動操作は未検証。

### 2026-09-06 16:01 — 道路の分割と UV 確認

- 長さ・幅方向を約 1 m の格子へ分割。幅・区間長を切り上げた分割数で等分し、端点と道路幅を維持する。カーブの内外でセル寸法は変わる。
- 直線の角はマイター生成後に行を補間し、細分化による内側の反転を防ぐ。
- 「表示」から道路の 1 m グリッドと UV チェッカーを個別に切り替える。設定は settings.json に保存。
- グリッドは実距離 UV に沿う 1 m 目盛りであり、三角形の辺のワイヤーフレームとは別。UV 反復長を変えても目盛り間隔は変わらない。
- UV チェッカーは 1 タイルに 2×2 の市松模様、U 方向は赤、V 方向は緑。照明・露出の影響を除いて表示する。旧地形の描画へは適用しない。
- Debug ビルド・テスト、DXC の VS/PS 単体コンパイル、ユーザーの道路データで 3 表示を確認。Release は `road_editor_grid.exe` として用意し、起動・テストも成功。
- 検証資料は `data/road-grid-check/`。

### 2026-09-06 14:29 — 道路メッシュ生成

- `Path → Road → Mesh Output` を実装。RoadSurface は Mesh 型、Left / Right は実寸 Path 型。
- 分岐のない開いた Path から一定幅の帯を作り、左右境界、法線・接線、実距離 UV を生成する。
- 曲線は既存の曲線を 24 標本/区間で評価し、約 1 m 間隔に刻み直す。直線の角はマイター接続する。
- 幅・パス・接続の変更とアンドゥで再評価。GPU 転送はフレーム外、旧リソースは遅延解放する。
- 手入力のシーンと生成結果を分離し、版8では元グラフと道路設定を保存する。Mesh Output を削除すると元シーンへ戻る。
- 分岐・閉ループ・孤立点・垂直区間・反転する急カーブを拒否する。遠く離れた区間同士の自己交差検査は未対応。
- Debug / Release でビルド・テスト済み。直線、勾配、曲線端、幅変更、実距離 UV、境界入力、型・循環検証を確認。
- 保存→再読込→再保存が一致。S 字道路の画面と、点の Y 座標を 2 から約 5.58 へドラッグした道路の追従を目視確認。
- 道路が表示されない原因を `data/sample_road.tgproj` で特定。Path → Road だけが接続され、Mesh Output が無かった。
  従来の Output は Material 型専用。接続済みコピーは `data/sample_road_connected.tgproj`。
- 検証資料は `data/road-check/`。

### 2026-09-06 14:07 — 実寸パスのプロパティ整理

- 幅・フェザー・強さは地形マスク用のため、実寸パスのポイント・初期値・鎖の設定欄から除いた。実寸ポイントは XYZ のみ表示する。
- 旧地形パスの UI と保存データは保持する。道路幅は Road 側で扱う。
- Debug ビルドと `--screenshot-ui` の目視確認が完了。Release は道路生成の実装時に通常ビルドで確認した。

### 2026-09-06 13:52 — テクスチャのリンク切れ対応

- terrain-graph の 7c272e3 を移植し、プロジェクトと .tgmat の画像参照を保持する。
- テクスチャ右クリックから個別指定・元の場所・フォルダ一括検索で復旧できる。プレビューにも復旧ボタンを置く。
- 欠けたマップは未指定相当で評価し、復旧後は同じ ID でサムネイルと合成を更新する。
- Debug / Release ビルド・既存テスト、CsImage の DXC 単体コンパイルが成功。
- 欠落画像のパス・名前・複数マップ参照を保持した保存→再読込→再保存が一致。画像を元の場所へ戻した後の再読込と警告解除を UI 画像で確認。
- 復旧ダイアログの手動操作と .tgmat の実行検証は未実施。検証資料は `data/missing-link-check/`。

### 2026-09-06 13:29 — 矩形選択と XYZ 入力

- Path 選択中の空ドラッグで、画面上の制御点を矩形選択する。Shift で追加、Esc で取消。
- Path 未選択のメッシュシーンでは、投影境界矩形との交差でメッシュ単位に選択する。選択枠と件数を表示する。
- XYZ はスライダーを廃止し、1 行の入力欄へ変更した。Enter 確定・Esc 取消、編集した軸だけを適用する。
- XYZ の入力・確定・取消・他軸の保持を ImGui 自動テストで確認。Debug / Release ビルドとテストが成功し、通常の road_editor.exe へ反映した。
- ドラッグ入力の注入で、置換 3 点・Shift 追加 4 点・Esc 取消 1 点・メッシュ 3 個の選択を確認した。選択による dirty フラグの変更は 0。
- 検証資料は `data/path-box-check/`。スクリーンショット検証では `--test-drag x0 y0 x1 y1`、`--test-drag-shift`、`--test-drag-cancel` を指定できる。通常起動では入力の差し替えは行わない。

### 2026-09-06 12:59 — XYZ 座標と 3 軸ギズモ

- PathPoint / PathCurveSample の座標を x/y/z へ整理し、版7で `position: [X, Y, Z]` を保存する。版6の実寸 Path は自動で読み替える。
- X（赤）/ Y（緑）/ Z（青）の移動軸を追加。選択点・鎖の 3 次元の重心に置き、Y 軸で上下移動できる。
- ドラッグ開始時の軸方向と換算率を保持し、移動中の投影変化による飛びを防ぐ。視線と重なる軸だけを非表示にする。
- 2 次ベジェ・3 次 B スプラインは Y も同じ基底で補間する。垂直エッジへの点挿入は 3 次元距離で行う。クロソイドは XZ 平面線形と高さ補間を継続する。
- 垂直エッジへの挿入と、XYZ の軸交換に対する曲線の一致を回帰テストで確認した。
- Debug ビルド・テスト、版6読込→版7保存→再読込→再保存が成功。UI 画像で 3 軸と XYZ 欄を確認した。
- Release は起動中の exe を上書きできず、`road_editor_xyz` としてリンク・起動確認した。検証資料は `data/path-xyz-check/`。実際のマウスドラッグは未検証。

### 2026-09-06 12:25 — 実寸 Path カーブ

- 新規 Path は地形から独立した X/Y/Z 座標（m）を保持する。Ctrl＋クリックで Y=0 平面へ追加し、選択点から延長するとその高さの平面へ置く。
- グリッド外へ追加・移動・貼付でき、点の X/Z と Y（高さ）をプロパティで編集できる。メッシュシーン表示中もグラフと Path 編集を有効にした。
- 旧 Path は「実寸カーブへ変換」で、表示中の制御点位置を固定できる。地形由来の細かな起伏への追従は解除する。
- プロジェクト版6に worldSpace を保存。旧形式の Path も従来の座標として読み込める。
- Debug / Release ビルド、範囲外の追加・移動・貼付・曲線標本の回帰テストと既存テストが成功した。
- グリッド外・異なる高さのカーブを UI 画像で確認し、保存→再読込→再保存で JSON が一致した。
- 検証資料は `data/path-world-check/`。マウスでの追加・ドラッグ、旧 Path 変換ボタンのクリックは未検証。

### 2026-09-06 12:06 — 作業グリッド

- Y=0、原点中心に 50 m × 50 m（X/Z 各 −25〜＋25 m）のグリッドを追加した。1 m 間隔、5 m ごとに強調線、中心線をさらに強調する。
- ビューポートの「表示」からオン・オフできる。既定はオンで、アプリ設定に保存する。
- 新規時のカメラと、グリッド表示中の A キーによる全体表示はグリッドの範囲を含む。
- Debug / Release ビルド、既存テスト、OverlayLines の VS/PS 単体コンパイルが成功した。
- 設定ファイルのオン・オフそれぞれで起動し、UI 画像を目視確認した。メッシュとの遮蔽も確認した。メニューのクリック操作は未検証。
- 検証画像とログは `data/grid-check/`。

### 2026-09-06 11:28 — メッシュ基盤の追加（R1）

- MeshData / MeshScene を GPU 非依存のデータ型として追加した。
- 複数メッシュを単色 PBR 材質・影付きで描画し、地形合成・変位を使わない経路を追加した。
- .tgproj 版5の scene 入力で保存再読込する。旧ファイルの読込も維持した。
- scene 表示中は旧グラフと地形用設定を隠し、カメラ・ライティングは操作できる。
- 入力の妥当性・不正入力時の保持・保存往復の CPU テストを追加した。
- Debug ビルドと既存／追加テストが成功。3 メッシュの上下の重なりと影を UI スクリーンショットで確認した。
- 新形式の保存→再読込→再保存で JSON が完全一致した。描画・保存ログに警告／エラーなし。
- 検証入力と画像は `data/mesh-scene-check/scene.tgproj` / `scene-ui.png`。

### 2026-09-06 11:14 — 文書の整理

- 旧地形ノード一覧、河川・地形パス設計、移植調査、派生元の計画保存版を削除した。
- グラフ・合成の共通基盤と旧ファイルの読込仕様は移行に必要なため残した。
- 変更履歴は Road Editor 開始以降に絞り、作業ルールは AGENTS.md に集約した。
- 残った Markdown のローカルリンク切れは 0 件。文書だけの変更のため、ビルドは再実行していない。

### 2026-09-06 11:03 — Road Editor への改名（R1）

- road-editor / Road Editor / road_editor.exe に名称を統一した。
- CMake のアプリ・テストターゲット、vcpkg 名、ウィンドウ名、ログ、スクリーンショット名を変更した。
- 設定と最近使ったファイルは %LOCALAPPDATA%/road-editor/、レイアウトは road_editor_imgui.ini とした。
- .tgproj / .tgmat と形式識別子は維持し、派生元の保存データを引き続き扱う。
- goals / plan / README と作業ルールの概要を道路向けへ更新した。
- `cmake --preset x64` と `cmake --build --preset x64-debug` 成功。コンパイラ警告なし。`ctest` 成功（1/1）。
- Debug アプリで新規保存→再読込→再保存し、JSON が完全一致した。起動ログで road-editor 0.1.0 を確認。
- `--screenshot-ui --screenshot-frame 20` の 1920×1080 PNG を目視し、既存グラフと PBR 平面の表示を確認した。
- 保存先は `data/road-editor-migration-check/`。設定の確認は LOCALAPPDATA をこの検証用フォルダへ向けて実施した。
- 最小グラフの確認のみ。旧素材付きプロジェクト全般と対話編集は未検証。

## 引き継いだ基盤

ノード・リンク編集、コピー／貼り付け、アンドゥ、グラフ保存、Path の曲線と地形上の編集、
パスマスク、GPU の PBR 合成、直接光と IBL、露出、トーンマップ、非同期 compute 評価。
マスクは DAG + キャッシュ、材質は線形合成。プレビューは地形用平面が中心。
複数メッシュの入力・表示経路と、Road からのメッシュ・材質評価は追加済み。

## 注意点

- 起動中の Release 版を上書きできない場合、`road_editor_xyz` / `road_editor_grid` / `road_editor_gizmo` の別名で
  `build/bin/Release/` へ出力してきた。通常の `road_editor.exe` は道路マテリアルの時点まで反映済みで、
  移動ギズモ修正は `road_editor_gizmo.exe`、白線・表示メニュー・ライトギズモ・縦断とバンクは `road_editor_marking.exe` にのみ入っている。
- 手動操作で未検証のもの: 素材編集と再リンク、リンク切れ復旧ダイアログ、.tgmat の実行検証、
  マウスでの Path 追加・ドラッグ、旧 Path 変換ボタン、表示メニューのクリック、L＋ドラッグのライトギズモ。
- 既知の未解決事項は [plan.md](plan.md#見つけている課題未着手) を参照する。
