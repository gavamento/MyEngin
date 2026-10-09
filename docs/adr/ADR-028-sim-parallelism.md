# ADR-028: sim の並列化は「出力次元だけを割る」系に限り、jobs あり / なしのハッシュ一致で証明する

- 状態: **確定** (2026-10-09、M90g)
- 出所: M90 描画の軽量化。計画は `plans\m90-render-optimization\spec.md` (§4.1.6、§4.1.7)。
- 実体: `src\Engine\Core\Jobs\JobSystem.{h,cpp}` (規約の正本は `JobSystem.h` 冒頭)、
  `Particles\CpuParticleBackend.cpp`・`Perception\PerceptionSystem.cpp`・`Animation\PartFollowSystem.cpp`・`Animation\TwoBoneIkSystem.cpp`。
  検証は `Loop\SimParallelSelfTest.cpp` と `tools\replay_verify.bat` の jobs A/B (`jobsab` ジョブ、`parts` ジョブ内のペア)。
- 番号: SimSnapshot・ABI・TypeId は変更なし。状態の形を変えていない。

## 背景

このエンジンの最大の制約は、Debug / Release / CI (WARP) のシミュレーションがビット一致すること。
float の加算は非結合なので、要素を足す順序・乱数を引く順序が結果のビットを決める。
並列化で「速いが結果が揺れる」ものを入れると、リプレイ・ロールバック・サーバ照合の土台が崩れる。
一方で、エンティティやプールごとに独立した仕事 (自分の行だけを書く) は、割っても結果が変わらない。
`JobSystem` (M25) と `TransformSystem` (深さごと) が既にこの流儀で動いている。

## 決定 1: 並列化してよい系の 3 条件

`jobs::System().ParallelRanges` で割ってよいのは、次の 3 つを**すべて**満たす系の、出力 (エンティティ / プール) 次元だけ。
`JobSystem.h` 冒頭の規約 (連続レンジ・レンジ間依存なし・縮約は呼出側が index 順で結合・ネスト非対応) をそのまま前提にする。

1. 他エンティティの今 tick の書き込みを読まない。読むのは前 tick の値・不変データ・自分の行だけで、書くのは自分の行だけ。
2. 共有 RNG (`world.Rng()`) を使わない。使ってよい乱数はエミッタごとなど、仕事の単位が自分で持つストリームだけ。
3. 共有コンテナ・イベント・ログ・構造変更・警告済み表は、並列段の**後**に直列で index 順に適用する。
   並列段のワーカーは World の内部状態 (`ForEachArchetype` の反復深さ・クエリキャッシュ) を触らない。
   World を引く処理 (`ForEachArchetype`) と判定は並列段の**前**の直列段で済ませ、`GetComponent` / `GetParent` / `IsAlive` の読み取りだけを並列段に残す。

縮約 (統計の合算) は、レンジごとではなく**仕事の単位ごと**の値を配列に置き、直列で添字順に足す。
結果がレンジの切り方 (= ワーカー数) に依存しない形にするため。

## 決定 2: 並列化した系と、そのために入れた局所修正

| 系 | 割る次元 | 局所修正 |
|---|---|---|
| CPU 粒子 | プール (エミッタ) | `Update` を「直列: World を引いて凍結を判定 → 並列: `StepPool` → 直列: 生存数の合算」に分けた。乱流のパラメータはメンバ (共有 scratch) から `TurbulenceParams` (ローカル) に移した |
| Perception | 知覚者 | scratch (`events` / `sightCandidates` / `bestPriority`) をレンジごとに持つ。視線のコライダー表は並列段の前の直列段で集める (視覚の知覚者がいて刺激がある tick だけ。World の走査を並列段に持ち込まない)。`stats_.losColliders` は並列段の後、どれかの知覚者がレイを撃ったときだけ更新 (従来と同じ観測)。レイ数は知覚者ごとの配列に置いて直列で合算 |
| PartFollow | ポーズ / 部位 | 直列段で警告・ポーズキャッシュの表・`LocalTransform` の引きを済ませ、ポーズの評価 (`SampleSkinnedLocals`) と TRS の分解を並列にした |
| TwoBoneIk | SkinnedMesh | 直列段で収集、並列段で `poseIk` を書き、WARN と警告済み表は直列段の後で収集順に出す |

粒度 (grain) は各ファイルの名前付き定数。入力が grain 以下なら `ParallelRanges` が呼出スレッドで直列実行する (結果は同じ)。

## 決定 3: 並列化しない系 (理由つき)

- **空力・XPBD・破壊のボクセル化**: 永久に禁止。加算順が結果の一部 (`AeroSampling.h`、`FractureVoxel.h`)。
- **BehaviorTree**: 共有の `world.Rng()`、他エンティティの BT を処理済み / 未処理の境界ごと読む (Gauss-Seidel が仕様)、イベントの `seq` が順序依存、C# コールバック。
- **AgentSystem (M65f)**: 共有の `world.Rng()`。
- **Crowd (dtCrowd)**: Detour の共有 scratch と経路キューの反復予算が順序依存。分けられるのは Surface 単位まで。
- **AnimatorController**: `ApplyClipPose` が子孫の任意のコンポーネントを書き、入れ子で部分木が重なりうる。`fired_` の順序が意味を持つ。
- **物理 (剛体ソルバ)**: ソルバの反復は順序依存とみなす。
- **FootIk (足の接地)**: `RaycastWorld` が内部で `World::ForEachArchetype` を呼ぶ。`ForEachArchetype` は反復深さの非アトミックな増減とクエリキャッシュの充填をするので、
  並行呼び出しは安全でない。物理側のレイを撃つ関数を作り替えるのはこの M90 の範囲外なので、足の接地は直列のまま。
- **Skinning (`SkinningSystem`)**: 1 行の仕事が小さく、割る利益を示す計測が無いので今回は割らない (計測で目立てば同じ 3 条件で割れる)。

「外す」系を割るには、アルゴリズムか RNG の割り当てを変える必要がある。それは挙動の変更であって軽量化ではない。

## 決定 4: 証明は jobs あり / なしのハッシュ列の一致

- `tools\replay_verify.bat` の jobs A/B: 直列 (`--no-jobs`) で録った `.rep` を、ワーカーを起こした Debug と Release で照合する。
  ヘッダの jobs ビットは違うので `.rep` のバイト比較はできないが、verify はヘッダの jobs ビットを見ず、毎 tick のワールドハッシュで合否を決める。
  = 直列の毎 tick のワールドハッシュ列と並列の列が全 tick で一致することの証明になる。
  粒子 (`--particle-demo --particle-backend cpu`)・知覚 (`--perception-demo`) は `jobsab` ジョブ、部位追従 (`--parts-demo`) は保存済みシーンを使う `parts` ジョブの中で回す。
  失敗したときは `:diagnose` が `--no-jobs` で録り直した側とのフィールド差分を出す。
- `SimParallelSelfTest`: 同じ初期状態を jobs あり / なしで 90 tick 回し、ワールドハッシュ (と NoHash の `poseIk`) が毎 tick 一致することを確かめる。
  replay のシーンに TwoBoneIk を含むものは無いので、IK の一致はここだけが受け持つ。ワーカーへ実際に配ったか (`JobSystem::GetStats`) も見る。
- ワーカーが動いたことの観測: `--replay-verify` の PASS 行の次に `[jobs] parallel batches N, chunks run by workers M` が出る (`--no-jobs` なら 0 / 0)。

## 却下した案

- **ワーカーごとの RNG ストリームを割り当てて BT / Agent を割る**: 乱数の消費順が変わり、既存のリプレイが全て無効になる。
- **`ForEachArchetype` を並行安全にして FootIk の `RaycastWorld` を割る**: ECS の中核の変更で、他の全システムの性能にも効く。この M90 の範囲を超える。
- **grain を 1 にして小さなシーンでも必ず割る**: スレッド起床のコストの方が高い。小さい入力は直列と同じ経路で結果が同じなので、テストは入力を grain より大きくして割れる経路を叩く。
