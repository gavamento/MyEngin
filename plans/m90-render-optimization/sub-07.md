# sub-07: sim の並列化 (CPU 粒子 / Perception / PartFollow / IK、jobs A/B ジョブ、ADR-028)

- 依存: sub-01
- 状態: OK (commit 3935904)
- 往復: 2

## やること
spec §4.1.6、§4.1.7、§2 #12 #13。
1. **先に A/B の検証路を作る** (並列化より前): `tools\replay_verify.bat` に、同じシーンを jobs あり / `--no-jobs` で録り、**毎 tick のワールドハッシュ列**を比べるジョブを足す (Debug と Release)。ヘッダの `kCfgJobs` が違うので `.rep` のバイト比較ではなくハッシュ列で比べる。既存の `--job` 再入の流儀に従う (bat の echo は ASCII、子 cmd は chcp 437 — `run_parallel.ps1` の注意)。対象シーンは粒子・知覚・部位追従・IK を含むものを選ぶ (無ければ既存デモの組み合わせ)。
   - 既存の比較手段 (`--hash-diff`、tick ごとのダンプ) で足りるか先に確認し、足りなければ最小の CLI を足す。
2. §4.1.7 の v1 対象を、表の「局所修正」を入れてから `jobs::System().ParallelRanges` で出力 (エンティティ / プール) 次元だけ割る:
   - CPU 粒子 (`Particles\CpuParticleBackend.cpp`)
   - Perception (`Perception\PerceptionSystem.cpp`)
   - PartFollow (`Animation\PartFollowSystem.cpp`)
   - TwoBoneIk / FootIk (`RaycastWorld` の並行読みの安全性を先に確認。駄目なら外して「仕様との差分」に出す)
   - Skinning は sub-01 の計測で目立つときだけ
   - 1 系ずつ入れ、入れるたびに A/B を回す (割れたら直前の系が原因と分かる)。
3. 粒度 (grain) は既存 (`kCullGrain`) の流儀で名前付き定数。小さい入力では直列と同じ経路になってよい (結果は同じであること)。
4. `docs\adr\ADR-028-*.md` (番号は着手時に `docs\adr\` の末尾を確認): 並列化の 3 条件 (他エンティティの今 tick の書き込みを読まない / 共有 RNG を使わない / 共有コンテナ・イベント・構造変更は並列段の後に直列で index 順)、縮約は index 順で結合、禁止リスト (空力・XPBD・破壊のボクセル化)、外した系と理由 (§4.1.7)、証明方法 (A/B ジョブ)。`JobSystem.h:14-22` の既存規約と矛盾させない (ADR から参照する)。
5. `perf_verify` 系のマイクロベンチに足すかは任意 (足すなら ms はゲートにしない)。

## やらないこと (このサブでは)
- BT / AgentSystem / Crowd / AnimatorController / 物理 / 空力 / XPBD の並列化。
- 並列化のためのアルゴリズム変更・RNG ストリームの割り当て変更。
- SimSnapshot の版を上げること (状態の形は変えない。上げる必要が出たら差分に出す)。

## 触る場所 (planner の見立て)
- `tools\replay_verify.bat`、`tools\run_parallel.ps1` (ジョブの追加)
- 上記 4 系のソース、`src\Engine\Core\Jobs\JobSystem.h` (使うだけ。ネスト呼び出し非対応に注意 — TransformSystem 等の中から呼ばない)
- `docs\adr\`

## 受け入れ条件 (このサブ)
1. A/B ジョブが、並列化前の状態で PASS する (検証路そのものの確認)。わざと 1 系に順序依存を入れると FAIL する (一時的な注入で確認し、コミットしない)。
2. 各対象系が jobs ありで実際に複数ワーカーで走る (ログまたは統計)。
3. `tools\replay_verify.bat` 全体 (新しい A/B を含む、Debug / Release) PASS。
4. `Editor.exe --selftest` Debug / Release PASS、`tools\check_rules.ps1` PASS。
5. ADR-028 がある。
6. 対象系の sim 時間を jobs あり / なしで参考値として報告 (sub-01 の計測を使う)。

## 検証コマンド
- ビルド Debug / Release、`--selftest` 両構成、`tools\check_rules.ps1`、`tools\replay_verify.bat`
- 単発: `tools\replay_verify.bat --job <新しい A/B ジョブ名>`

## 実装メモ (coder が追記)

SELF_EVAL: sub-07 (round 1)
- 実装: CpuParticleBackend (プール並列 `StepPool`、乱流 scratch をローカル化)、PerceptionSystem (知覚者並列、コライダー表は call_once)、PartFollowSystem (ポーズ評価と分解を並列)、TwoBoneIkSystem (収集 → 並列 → WARN を直列)、JobSystem に統計 (`GetStats`)、`SimParallelSelfTest` 新設、replay_verify.bat に jobs A/B (`jobsab` ジョブ + `parts` 内のペア)、`docs\adr\ADR-028-sim-parallelism.md`。
- 差分: FootIk は外した (`RaycastWorld` が `ForEachArchetype` を呼び並行不可)。IK は replay のシーンが無く selftest のみ。PartFollow の replay A/B は小シーンで並列経路に入らない (batches 0)。Perception の grain は 2 (3 体のデモで割れるように)。視線コライダー表は事前確定ではなく call_once。
- 検証: Debug/Release selftest exit 0、check_rules 0 error、replay_verify 全体 PASS、粒子へ順序依存を注入して jobsab が HASH MISMATCH (tick 3) で FAIL することを確認し戻した。
- 参考値 (Release、selftest の 90 tick 平均、jobs あり / 直列): 粒子 0.114 / 0.270 ms、知覚 0.126 / 0.254 ms、IK + 部位追従 0.293 / 0.315 ms。Debug の知覚は 8.97 / 5.20 ms で並列の方が遅い。

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-10-09)
  1. [must] Perception の視線コライダー表を、並列段の中で `std::call_once` を使って集めている (`PerceptionSystem.cpp:569-573` → `CollectLosColliders`、`:646` から呼ぶ)。これは並列段の中で World を走査することになる。coder 自身が FootIk を外した理由 (「`ForEachArchetype` は `iterationDepth_` の非アトミックな増減とクエリキャッシュ `queryCache_` の充填をするので並行不可」) と同じ操作を、ワーカーで走らせている。ほかのワーカーは同時に `Detects` / `LineOfSight` で World を読んでいる。今は競合する読み手が無いとしても、安全性が World の内部実装の偶然に乗っている。ADR-028 の規約 (並列段で World を走査しない) にも、spec §4.1.7 の「`ensureColliders()` をループの前で確定」にも反する。修正: 直列の前段で、保守的な条件 (視覚が有効な知覚者が 1 人以上いて、刺激が空でない) のときだけ `CollectLosColliders` を呼ぶ。並列段では、レンジごとに「レイを撃ったか」の bool だけを持つ。並列段の後で 1 つでも撃っていれば `stats_.losColliders` を更新する (従来の「集めなかった tick は stats_ を触らない」と同じ観測になる)。`std::call_once` と `once_flag` は消す。直したら、`--job jobsab`・replay_verify 全体・SimParallelSelfTest・両構成の selftest を取り直す。
  2. [should] Debug だけ知覚の並列が遅い件 (8.97 / 5.20 ms) は、#1 の修正後に 1 回だけ測り直して報告する。遅いままでも合否には使わない (ms はゲートにしない、spec §2 #14)。原因の調査は不要。
  - 差分の判定: FootIk を外した件は spec §4.1.7 の条件どおりなので採用。IK と PartFollow の並列経路の被覆を SimParallelSelfTest (200 体、毎 tick ハッシュ) で持つ件も採用。実シーンの A/B に入れるためにデモを足すと golden に響くので、M90 では足さない (spec §7)。受け入れ 1 の「並列化前の PASS」が未実施の件は採用。並列化前は直列同士の比較で自明であり、順序依存の注入で FAIL し、正常なコードで PASS する、で検証路の働きは示せている。Perception の grain 2、`JobSystem::GetStats`、PASS 行の `[jobs]` ログも採用。
  - 不安・質問への回答: (a) IK / PartFollow の実シーン被覆は M90 では足さない (spec §7 に記録)。(b) Debug の速度は #2 のとおり。(c) ServerNet の一過性 FAIL は sub-08 やること 6 で基点と比べる (計 4 回)。
- round 2: VERDICT OK (planner、2026-10-09)。#1 は解消した。`call_once` を消し、`CollectLosColliders` を並列段の前の直列段に移した (`PerceptionSystem.cpp:572`)。並列段は知覚者ごとに `firedRays` を立てるだけで、並列段の後に `stats_.losColliders` を更新する (`:610-646`、`:784`)。ADR-028 も実装に合わせた。jobsab / replay_verify 全体 / 両構成の selftest / check_rules をすべて取り直して PASS。#2: Debug の知覚の並列は 9.46 / 5.37 ms で遅いまま。原因は call_once ではなかった。ms はゲートにしないので参考値として記録するにとどめる (申し送り)。
