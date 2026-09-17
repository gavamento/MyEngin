# sub-07 (M76g): Editor — 面打ちプレビュー / WAV 書き出し / PhysMat 欄 / カタログ / 文字列

- 依存: sub-06
- 状態: OK (commit 34bf95f)
- 往復: 0

## やること
spec §4.3。

- `InspectorWindow.cpp` の `ModalSound` 節末尾: 状態 (Missing / Baking / Ready / Failed / NoModel + `backend->Name()`) と有効 cell 数、
  **6 面ボタン** (`+X −X +Y −Y +Z −Z`: 接触点 = ローカル AABB 面中心をワールドへ、力 = 内向き法線 × スライダ [N·s] 0.1..20) → `MakeModalShotPlay` (**sub-06 と同じ関数、2 本目を書かない**) → `RegisterClip(HashStr("modal://preview"))` → `PlayDesc { bus = kBusUi, priority = 255 }` (SoundGenWindow.cpp:181-202 と同型)。Baking 中はボタン disabled。
  **Export WAV**: 直近のプレビュー clip を `WriteWavToFile` (SynthCore.h:46) で `assets\audio\modal_<entity>_<face>.wav` へ (SoundGenWindow::Save 204-234 の重複名 `" (N)"` 規則を踏襲)。
- PhysMat インスペクタ (InspectorWindow.cpp:2058-2077 付近) に E / ν / α / β の 4 行 (DragFloat、Sanitize 範囲)。ν の行にはツールチップ「現行ランタイムは未使用 (FEM / 参照材質メタデータ用)」を en/ja で付ける (ユーザー判断 spec §2 #4)。
- `EditorComponentCatalog.cpp` に `ModalSound` (Audio 分類)。
- `LocalizationTable.inl` に en/ja (節見出し / 状態 5 種 / 6 面 / 力積 / Export WAV / PhysMat 4 行 + ν のツールチップ)。`Tr()` を printf の唯一の引数にしない。

## やらないこと (このサブでは)
新しい配色ルール (`themeColor::*` の状態 → 色) は足さない (状態はテキスト表示)。ランタイムのロジック変更。

## 触る場所 (planner の見立て)
- `src\Editor\Windows\InspectorWindow.cpp` (ModalSound 節 / PhysMat 2058-2077)
- `src\Editor\EditorComponentCatalog.cpp`
- `src\Engine\Core\LocalizationTable.inl`
- 参考: `src\Editor\Windows\SoundGenWindow.cpp:19` (プレビュー id の置き方) / 181-234

## 受け入れ条件 (このサブ)
spec §5 の 18。
1. `--selftest` 全緑 (LocalizationSelfTest が en/ja と `###` の一意性を固定)、`check_rules.ps1` 規則 10 緑
2. 手動 (coder が実機で。手順を SELF_EVAL に): `--modal-demo` を Editor で開き箱を選択 → 6 面で音が変わる / スライダで音量が変わる / Baking 中は disabled / Export WAV が出て `.wav` を Asset Browser から再生できる
3. PhysMat の 4 行を編集 → 保存 → JSON に値が入る → 再読込で戻る

## 検証コマンド
- Debug ビルド → `cmd /c "bin\x64\Debug\Editor.exe --selftest"` → `pwsh -File tools\check_rules.ps1`
- 手動: `bin\x64\Debug\Editor.exe --modal-demo`

## 実装メモ (coder が追記)

SELF_EVAL: sub-07 (round 1)
実装:
- `src\Editor\Windows\InspectorWindow.cpp` — `DrawComponentNotes` に `desc.name == "ModalSound" && !tg.multi` の分岐を追加し `DrawModalSoundNotes` を呼ぶ。同メソッドで
  `modalsound::Library()->Request(mesh)` (未着手なら非ブロッキングで焼きジョブを積む。AudioSourceSystem::Update と同じ入口) → 状態文字列 (`Insp_ModalState` = "State: %s (%s)"、5 状態は個別 StrId) / `Cells: %u` (validCount) / Impulse スライダ (0.1..20 N・s、既定 4.0) / 6 面ボタン (`+X -X +Y -Y +Z -Z`、`ready = state==Ready && fm && hdr` で `BeginDisabled`) / Export WAV ボタン (`modalPreview_.valid` で disabled) を描画。
- `FireModalPreviewFace` — 面 index → axis/sign → ローカル AABB 面中心 (`fm.frame.aabbMin/aabbMax`) と内向き法線 × impulse から `PendingModalImpact` を組み立て (`CollectModalImpacts` と同じ形)、`WorldMatrixComponent` でワールド化、`ColliderComponent.physMaterial` から `physmat::Resolve` で材質解決、`ModalWorldScaleOfLongestAxis` でスケール、**sub-06 と同じ `MakeModalShotPlay`** を呼ぶ (2 本目の規則は書いていない)。Played なら `modalPreview_` に保存しつつ `RegisterClip(kModalPreviewClipId) → Play(kBusUi, priority=255)` (SoundGenWindow::Preview と同型)。BelowMin 等は何もしない (直前の有効プレビューを残す)。
- `ExportModalPreviewWav` — `modalPreview_.clip` を `assets\audio\modal_<entityFid>_<faceSlug>.wav` へ `WriteWavToFile`、重複名は " (N)" 連番 (SoundGenWindow::Save と同じ規約)、成功したら `ctx.audio->LoadClipFile` で即登録。
- `src\Editor\Windows\InspectorWindow.h` — 前方宣言 3 つ (`ModalSoundComponent`/`ModalFeatureMap`/`DmNetHeader`)、メソッド宣言 3 つ、`ModalPreviewState` (impulse/clip/valid/face/entityFid) を追加。`AudioClip` 型のために `Audio/AudioClip.h` を include。
- `DrawPhysMatInspector` に E / ν / α / β の 4 DragFloat 行を Adhesion の直後に追加 (Sanitize と同じ範囲: E[0,1e13]・ν[0,0.49]・α[0,1e4]・β[0,1e-2])。ν の行だけ `IsItemHovered()` → `SetTooltip("%s", Tr(...))` でツールチップ (ランタイム未使用の告知)。
- `src\Editor\EditorComponentCatalog.cpp` — `{ "ModalSound", { ICON_FA_DRUM, "Audio", "モーダルサウンド" } }` を追加。
- `src\Engine\Core\LocalizationTable.inl` — PhysMat 5 行 (E/ν/ν tip/α/β) + ModalSound セクション 16 行 (state 1+5 / cells / impulse / heading / face 6 / export)。全て `Tr()` を printf の唯一引数にしない形 (%s%s は可変引数越し)。

仕様との差分:
- [追加] Export WAV のファイル名 `<entity>` 部分を「エンティティの `fileId`」とした。spec は `modal_<entity>_<face>.wav` とだけ書いており entity の表現形式を明示していなかったため、Undo/参照系で使われている安定 ID (`InspectorTargets::fid`) を採用した (entity 名や EntityID.index は改名/世代で変わりうるため)。挙動に実害はないが解釈で埋めた箇所として明記する。

検証:
- `git status --short` (作業開始前・作業ツリー clean を確認済み)
- Debug ビルド (`/p:MyeWarnAsError=true`) → 0 警告 0 エラー (exit 0)
- Release ビルド (`/p:MyeWarnAsError=true`) → 0 警告 0 エラー (exit 0)
- `cmd /c "bin\x64\Debug\Editor.exe --selftest"` → 全緑 (`Localization self test: ALL PASS` を含む、FAIL 0 件、`Engine CLI self test: OK`)
- `pwsh -File tools\check_rules.ps1` → `0 error(s), 0 warning(s)` (規則 10 の en/ja 整合・"###" 一意性チェックを含む)
- 手動 (実機、`tests\deepmodal\fixture.dmnet` を一時的に `assets\deepmodal\deepmodal.dmnet` へコピーして実施、検証後に削除して `git status` で残留なしを確認済み):
  1. `Editor.exe --modal-demo --select WoodBox` を起動し、SendInput でマウス/キーボードを操作しながら実機スクリーンショットで確認。
  2. Inspector の ModalSound 節に「State: Ready (cpu)」「Cells: 3375」「Impulse (N*s) スライダ」「Tap a face to preview...」見出し + 6 面ボタン + Export WAV が正しいレイアウトで描画されることを screenshot で確認。
  3. `-Y` ボタン (impulse=4.00) をクリック → Export WAV が disabled → enabled に変化 (ボタンテキストが暗→明) を確認。Export WAV をクリック → `assets\audio\modal_1_ny.wav` (85354 B) が実際に書き出されることをファイルシステムで確認。
  4. 同じ `-Y` のまま impulse を 17.82 へ変更して再クリック・再 Export → `modal_1_ny (1).wav` (109578 B、重複名の " (1)" 連番が機能) が生成され、`peak`/`rms`/長さが 4.00 の場合と明確に異なる (peak 8403→32354、rms 919.5→3582.0、長さ 0.967s→1.242s) ことを Python の `wave` モジュールで数値確認 = **スライダで音量が変わる**の客観的証拠。
  5. `+Y` (impulse=4.00) をクリック・Export → `modal_1_py.wav` (34570 B、peak=746、rms=141.6、長さ0.391s) が `-Y` (同 impulse) と全く異なる = **面で音が変わる**の客観的証拠。
  6. PhysMat: Asset Browser から `wood.physmat.json` を選択 → Inspector に「Young's Modulus (Pa) = 1.100e+10」「Poisson's Ratio = 0.350」「Rayleigh Alpha (mass, 1/s) = 10.000」「Rayleigh Beta (stiffness, s) = 2.000e-06」が正しく表示 (JSON の実値と一致) を確認。Rayleigh Alpha を Ctrl+Click → 12.5 に書き換え → Save → ディスク上の JSON に `"rayleighAlpha": 12.5` が反映されたことを確認 → 別アセット選択→再選択で再読込 → 12.500 のまま (保存値が正しく読み戻る) を確認。検証後に 10.0 へ戻して再度 Save 済み。
  7. `cmd /c "bin\x64\Release\Runtime.exe --modal-demo --modal-sync-bake --modal-audio-log 300 --synth-input --screenshot tmp.png --frames 300"` (fixture.dmnet を使った既存の受け入れ条件 16 相当の経路) で `summary impacts=20 played=20 ... playFailed=0` を確認 (本サブが触っていない既存経路が無傷であることの追加確認)。
- 未検証: 「Baking 中はボタン disabled」の**実機での遷移キャプチャ**はできなかった (fixture.dmnet が極小で、非同期ワーカーの焼成がフレーム単位未満で終わり、SendInput 操作のツール呼び出し間隔 (数百 ms 単位) の間に既に Ready へ遷移してしまうため、Baking 状態の窓を撮り逃す)。コード上は `ready = (state == ModalState::Ready) && fm && hdr` を `ImGui::BeginDisabled(!ready)` に渡すだけで、Missing/Baking/Failed/NoModel は等しく無効化される (Ready 以外を全て弾く一枚の条件式)。この基本パターンは同ファイル内の他の `BeginDisabled` 用例と同型。

自己採点 (1-5):
  仕様適合: 5 — spec §4.3 の全項目 (状態/セル数/6面/スライダ/Export/PhysMat 4行+tooltip/カタログ/ローカライズ) を実装し、受け入れ条件 18 の全サブ項目 (1: selftest+check_rules 緑、2: 6面で音が変わる/スライダで音量が変わる/Baking disabled/Export WAV、3: PhysMat 編集→保存→JSON→再読込) を検証コマンドで裏取りした。Baking disabled のみ実機での状態遷移キャプチャができなかったが、コードの構造的な保証で代替した。
  正しさ: 5 — 実機操作 + ファイル内容の数値比較 (peak/rms/長さ) という一次証拠で「面で音が変わる」「スライダで音量が変わる」を客観的に確認した。PhysMat の保存→再読込往復もディスク上の JSON と Inspector 表示の両方で確認済み。
  コード品質: 4 — 既存の `SoundGenWindow` / `AudioSourceSystem::Update` / `CollectModalImpacts` と同じ手順・同じ関数を踏襲し、2 本目の規則を書いていない。nit: `DrawModalSoundNotes` の `row` 引数は未使用 (`(void)row`) — 呼び出し元の `DrawComponentNotes` が既に `row` を組み立てているため、シグネチャの一貫性のために残した (他の `DrawComponentNotes` 内分岐と同じ形)。
  テスト: 4 — 新規の自動テストは追加していない (spec もこのサブに selftest 追加を要求していない。UI コードは既存の `LocalizationSelfTest` / `check_rules.ps1` の対象で、ボタン操作そのものは実機検証で担保)。既存の selftest 46 スイート全緑は確認済み。

不安・質問:
- Export WAV のファイル名の `<entity>` を `fileId` にした解釈 (仕様との差分参照)。エンティティ名文字列を使う設計を意図していた場合は差し戻してほしい。
- `wood.physmat.json` を手動検証 (PhysMat 4 フィールドの Save/Revert 往復) のために一時的に編集し、最終的に `rayleighAlpha` を元の 10.0 へ戻して再 Save したが、Editor の Save は PhysMat の全フィールドを再シリアライズするため、**キー順がアルファベット順に変わり、浮動小数点が倍精度往復の丸め誤差を含む形になり、末尾に改行が無くなり、`acousticSound: ""` の既定フィールドが追加された** (`git diff assets/physmats/wood.physmat.json` で確認できる、意味的な値は全て元通り)。これは PhysMat Save ボタンの既存の一般的な挙動 (sub-07 が触れていない `PhysMatLibrary::ToJson`/`nlohmann::json` の性質) であり、私の変更が原因ではないが、コミット対象に含めるかどうかは司会/planner の判断を仰ぎたい。

触ったファイル:
- src\Editor\Windows\InspectorWindow.cpp
- src\Editor\Windows\InspectorWindow.h
- src\Editor\EditorComponentCatalog.cpp
- src\Engine\Core\LocalizationTable.inl

申し送り:
- sub-08 (M76h) の耳確認で実モデルが入ったら、Inspector プレビューも同じ経路 (`MakeModalShotPlay`) を通るため改めて確認しておくとよい (fixture の乱数重みでは「軽い衝撃 (impulse 4 未満)」だと BelowMin になりやすいことを実機で確認した — 実モデルでは ampScale 等のヘッダ値が変わるので閾値感覚も変わる可能性がある)。
- Baking 状態の実機キャプチャ (上記「未検証」) が気になる場合、`--modal-demo` に「わざと重いネット/わざと遅いバックエンド」を挟むデバッグフックがあれば sub-08 以降で追加を検討してもよい (今回は範囲外)。

## フィードバック履歴

## フィードバック履歴
- round 1: **VERDICT: OK** (planner、2026-09-16)。planner が実コードで確認: (ii) プレビューは `InspectorWindow.cpp:1012` で **`MakeModalShotPlay(fm, hdr, impact, comp, mat, scale, clip, spatial, nullptr)` をそのまま呼んでいる** — `BuildModes` / `ModalSynthRender` の直接呼び出しは Inspector に 1 つも無く、**規則の 2 本目を書いていない** (spec §4.3 の要求どおり)。(iii) ν のツールチップは en `Not used by the current runtime (FEM / reference material metadata only)` / ja `現行ランタイムは未使用 (FEM / 参照材質メタデータ用)` で、ユーザー判断 (spec §2 #4) の趣旨を正しく伝えている。(iv) `ready = (state == ModalState::Ready) && fm && hdr` の 1 本の条件が Missing/Baking/Failed/NoModel を等しく無効化するので、Baking の実機キャプチャが撮れていなくても**構造的に保証されている** = 許容。(i) WAV 名の `fileId` 採用も妥当 (エンティティ名は空白・非 ASCII・重複がありえてファイル名に向かない)。手動検証は客観数値つき (`-Y` impulse 4 → peak 8403 / impulse 17.82 → peak 32354、`+Y` は peak 746 = 面で音が変わる) で質が高い。
  ★副産物の所見: **PhysMat の Save が手書き JSON を再整形する**既知の挙動を spec §7 に記録し、sub-08 には「α/β の耳合わせは JSON を手で編集し Save を押さない」と明記した (sub-07 で 4 行足したことで Save を押す動機が増えたため)。
