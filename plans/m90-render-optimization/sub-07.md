# sub-07: sim の並列化 (CPU 粒子 / Perception / PartFollow / IK、jobs A/B ジョブ、ADR-028)

- 依存: sub-01
- 状態: 未着手
- 往復: 0

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

## フィードバック履歴
