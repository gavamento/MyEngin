#include "Editor/Windows/AudioMixerWindow.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "Engine/Engine/Audio/AudioSystem.h"

#include "imgui.h"

namespace mye {
namespace {

constexpr float kStripWidth = 104.0f;
// フェーダの高さ。ストリップ全体 (名前/親/フェーダ/dB/M・S/送り/削除) が既定の窓サイズに
// 収まる値にすること — 溢れると下端のボタンが見えなくなる
constexpr float kFaderHeight = 120.0f;
constexpr float kMeterWidth = 14.0f;
// メーターの表示レンジ。線形のままだと小音量が潰れて何も見えないので dB で描く。
// 上端に +6 dB ぶんの余白を取り、0 dBFS を目盛りとして見せる
constexpr float kMeterFloorDb = -48.0f;
constexpr float kMeterTopDb = 6.0f;

float MeterNorm(float linear)
{
    const float db = LinearToDb(linear);
    if (db <= kMeterFloorDb) {
        return 0.0f;
    }
    return std::clamp((db - kMeterFloorDb) / (kMeterTopDb - kMeterFloorDb), 0.0f, 1.0f);
}

ImU32 MeterColor(float norm)
{
    if (norm >= MeterNorm(1.0f)) { // 0 dBFS 到達 = クリップ
        return IM_COL32(230, 80, 70, 255);
    }
    if (norm > 0.78f) { // -6 dB 付近
        return IM_COL32(230, 200, 70, 255);
    }
    return IM_COL32(90, 200, 120, 255);
}

// 小さなトグルボタン (Mute / Solo)。押下中は色を変える
bool ToggleButton(const char* label, bool on, ImU32 onColor, const ImVec2& size)
{
    int styles = 0;
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, onColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, onColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, onColor);
        styles = 3;
    }
    const bool clicked = ImGui::Button(label, size);
    if (styles != 0) {
        ImGui::PopStyleColor(styles);
    }
    return clicked;
}

// bus が候補 candidate の子孫か (親コンボで閉路を作らせないための判定)
bool IsDescendantOf(const AudioSystem& audio, int bus, int ancestor)
{
    int cur = bus;
    for (int step = 0; cur >= 0 && step <= audio.BusCount(); ++step) {
        if (cur == ancestor) {
            return true;
        }
        cur = audio.BusParent(cur);
    }
    return false;
}

std::string UniqueBusName(const MixerAsset& m, const std::string& base)
{
    if (FindMixerBus(m, base.c_str()) < 0) {
        return base;
    }
    for (int n = 2; n < 1000; ++n) {
        const std::string candidate = base + " " + std::to_string(n);
        if (FindMixerBus(m, candidate.c_str()) < 0) {
            return candidate;
        }
    }
    return base;
}

} // namespace

// ---------------------------------------------------------------------------
// トポロジ編集 (ランタイムを写して書き換え、ApplyMixer で差し戻す)
// ---------------------------------------------------------------------------

void AudioMixerWindow::AddBus(EngineContext& ctx, int parentBus)
{
    MixerAsset m = ctx.audio->CurrentMixer();
    if (parentBus < 0 || parentBus >= static_cast<int>(m.buses.size())) {
        parentBus = ctx.audio->RootBus();
    }
    MixerBus b;
    b.name = UniqueBusName(m, "New Bus");
    b.parent = m.buses[static_cast<size_t>(parentBus)].name;
    m.buses.push_back(std::move(b));
    ctx.audio->ApplyMixer(m);
    status_ = "added a bus (applies on the next frame)";
}

void AudioMixerWindow::RemoveBus(EngineContext& ctx, int bus)
{
    MixerAsset m = ctx.audio->CurrentMixer();
    if (bus < 0 || bus >= static_cast<int>(m.buses.size())) {
        return;
    }
    if (m.buses[static_cast<size_t>(bus)].parent.empty()) {
        status_ = "the root bus cannot be removed";
        return;
    }
    // 子は削除するバスの親へ引き上げる (孤児にすると検証で丸ごと弾かれる)
    const std::string gone = m.buses[static_cast<size_t>(bus)].name;
    const std::string newParent = m.buses[static_cast<size_t>(bus)].parent;
    m.buses.erase(m.buses.begin() + bus);
    for (MixerBus& b : m.buses) {
        if (b.parent == gone) {
            b.parent = newParent;
        }
    }
    ctx.audio->ApplyMixer(m);
    status_ = "removed bus '" + gone + "' (its children moved to '" + newParent + "')";
}

void AudioMixerWindow::RenameBus(EngineContext& ctx, int bus, const std::string& newName)
{
    MixerAsset m = ctx.audio->CurrentMixer();
    if (bus < 0 || bus >= static_cast<int>(m.buses.size()) || newName.empty()) {
        return;
    }
    const std::string old = m.buses[static_cast<size_t>(bus)].name;
    if (old == newName) {
        return;
    }
    const int clash = FindMixerBus(m, newName.c_str());
    if (clash >= 0 && clash != bus) {
        status_ = "a bus named '" + newName + "' already exists";
        return;
    }
    m.buses[static_cast<size_t>(bus)].name = newName;
    for (MixerBus& b : m.buses) { // 子の親参照は名前なので付け替える
        if (b.parent == old) {
            b.parent = newName;
        }
    }
    ctx.audio->ApplyMixer(m);
    // .sound.json はバス名で参照しているので、改名すると参照が切れて既定バスに落ちる
    status_ = "renamed '" + old + "' to '" + newName
        + "' — .sound.json assets referencing the old name fall back to the default bus";
}

void AudioMixerWindow::ReparentBus(EngineContext& ctx, int bus, int newParent)
{
    MixerAsset m = ctx.audio->CurrentMixer();
    if (bus < 0 || bus >= static_cast<int>(m.buses.size()) || newParent < 0
        || newParent >= static_cast<int>(m.buses.size()) || bus == newParent) {
        return;
    }
    m.buses[static_cast<size_t>(bus)].parent = m.buses[static_cast<size_t>(newParent)].name;
    ctx.audio->ApplyMixer(m); // 閉路になる組み合わせは ApplyMixer 側の検証で弾かれる
    status_.clear();
}

void AudioMixerWindow::Save(EngineContext& ctx)
{
    if (ctx.mixers == nullptr) {
        return;
    }
    const uint64_t hash = ctx.mixers->ActiveHash();
    MixerAsset* dst = ctx.mixers->GetMutable(hash);
    if (dst == nullptr) {
        status_ = "no .mixer.json is active — create one from the Asset Browser (Create > Mixer)";
        return;
    }
    MixerAsset cur = ctx.audio->CurrentMixer();
    cur.hash = dst->hash; // アセットの同一性 (GUID / パス / 名前) は保つ
    cur.name = dst->name;
    cur.path = dst->path;
    *dst = std::move(cur);
    status_ = ctx.mixers->SaveToFile(hash) ? ("saved: " + dst->name + ".mixer.json")
                                           : "save failed (see Console)";
}

// ---------------------------------------------------------------------------
// 描画
// ---------------------------------------------------------------------------

void AudioMixerWindow::DrawMeter(EngineContext& ctx, int bus, float width, float height)
{
    ImGui::InvisibleButton("##meter", ImVec2(width, height));
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(20, 20, 24, 255));
    dl->AddRect(p0, p1, IM_COL32(80, 80, 90, 255));

    const float h = p1.y - p0.y;
    const float level = MeterNorm(ctx.audio->BusLevel(bus));
    if (level > 0.0f) {
        dl->AddRectFilled(ImVec2(p0.x + 1.0f, p1.y - level * h), ImVec2(p1.x - 1.0f, p1.y - 1.0f),
                          MeterColor(level));
    }
    const float hold = MeterNorm(ctx.audio->BusPeakHold(bus));
    if (hold > 0.0f) {
        const float y = p1.y - hold * h;
        dl->AddLine(ImVec2(p0.x + 1.0f, y), ImVec2(p1.x - 1.0f, y), MeterColor(hold), 2.0f);
    }
    // 0 dB の目盛り (どこで振り切るかが見えないと調整できない)
    const float zeroY = p1.y - MeterNorm(1.0f) * h;
    dl->AddLine(ImVec2(p0.x, zeroY), ImVec2(p1.x, zeroY), IM_COL32(140, 140, 150, 140));
}

void AudioMixerWindow::DrawStrip(EngineContext& ctx, int bus)
{
    AudioSystem& audio = *ctx.audio;
    const bool isRoot = audio.BusParent(bus) < 0;
    ImGui::PushID(bus);
    ImGui::BeginGroup();

    // ---- 名前 (クリックで改名。Enter で確定 = グラフ再構築は 1 回だけ) ----
    if (renaming_ == bus) {
        ImGui::SetNextItemWidth(kStripWidth);
        if (renameFocus_) {
            ImGui::SetKeyboardFocusHere();
            renameFocus_ = false;
        }
        if (ImGui::InputText("##name", renameBuf_, sizeof(renameBuf_),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            RenameBus(ctx, bus, renameBuf_);
            renaming_ = -1;
        } else if (ImGui::IsItemDeactivated()) {
            renaming_ = -1; // Esc / フォーカスが外れたら破棄
        }
    } else {
        if (ImGui::Button(audio.BusName(bus), ImVec2(kStripWidth, 0.0f))) {
            renaming_ = bus;
            renameFocus_ = true;
            std::snprintf(renameBuf_, sizeof(renameBuf_), "%s", audio.BusName(bus));
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("click to rename");
        }
    }

    // ---- 親バス ----
    ImGui::SetNextItemWidth(kStripWidth);
    if (isRoot) {
        ImGui::BeginDisabled();
        if (ImGui::BeginCombo("##parent", "(root)")) {
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
    } else {
        const int parent = audio.BusParent(bus);
        if (ImGui::BeginCombo("##parent", audio.BusName(parent))) {
            for (int i = 0; i < audio.BusCount(); ++i) {
                // 自分自身と自分の子孫は親にできない (閉路になる)
                if (i == bus || IsDescendantOf(audio, i, bus)) {
                    continue;
                }
                if (ImGui::Selectable(audio.BusName(i), i == parent)) {
                    ReparentBus(ctx, bus, i);
                }
            }
            ImGui::EndCombo();
        }
    }

    // ---- フェーダ + メーター ----
    float db = audio.BusVolumeDb(bus);
    if (ImGui::VSliderFloat("##fader", ImVec2(kStripWidth - kMeterWidth - 6.0f, kFaderHeight), &db,
                            kMinDb, kMaxBusDb, "")) {
        audio.SetBusVolumeDb(bus, db);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%.1f dB", static_cast<double>(db));
    }
    ImGui::SameLine(0.0f, 4.0f);
    DrawMeter(ctx, bus, kMeterWidth, kFaderHeight);

    if (db <= kMinDb) {
        ImGui::TextUnformatted("  -inf dB");
    } else {
        ImGui::Text("%+6.1f dB", static_cast<double>(db));
    }

    // ---- ミュート / ソロ ----
    const float halfW = (kStripWidth - 4.0f) * 0.5f;
    if (ToggleButton("M", audio.BusMute(bus), IM_COL32(190, 70, 60, 255), ImVec2(halfW, 0.0f))) {
        audio.SetBusMute(bus, !audio.BusMute(bus));
    }
    ImGui::SameLine(0.0f, 4.0f);
    if (ToggleButton("S", audio.BusSolo(bus), IM_COL32(200, 170, 60, 255), ImVec2(halfW, 0.0f))) {
        audio.SetBusSolo(bus, !audio.BusSolo(bus));
    }

    // ---- リバーブ送り (ルートは reverb の出力先なので送れない) ----
    float send = audio.BusReverbSend(bus);
    ImGui::SetNextItemWidth(kStripWidth);
    if (isRoot || !audio.HasReverbBus()) {
        ImGui::BeginDisabled();
        ImGui::SliderFloat("##rev", &send, 0.0f, 1.0f, "rev --");
        ImGui::EndDisabled();
    } else if (ImGui::SliderFloat("##rev", &send, 0.0f, 1.0f, "rev %.2f")) {
        audio.SetBusReverbSend(bus, send);
    }

    // ---- 削除 ----
    ImGui::BeginDisabled(isRoot);
    if (ImGui::Button("Remove", ImVec2(kStripWidth, 0.0f))) {
        RemoveBus(ctx, bus);
    }
    ImGui::EndDisabled();

    ImGui::EndGroup();
    ImGui::PopID();
}

void AudioMixerWindow::DrawFooter(EngineContext& ctx)
{
    AudioSystem& audio = *ctx.audio;

    // ---- どの .mixer.json を鳴らすか ----
    if (ctx.mixers != nullptr) {
        const std::vector<MixerEntry> all = ctx.mixers->Enumerate();
        const MixerAsset* active = ctx.mixers->Get(ctx.mixers->ActiveHash());
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::BeginCombo("mixer asset", active != nullptr ? active->name.c_str() : "(none)")) {
            for (const MixerEntry& e : all) {
                if (ImGui::Selectable(e.name.c_str(), active != nullptr && active->hash == e.hash)) {
                    if (const MixerAsset* m = ctx.mixers->Get(e.hash)) {
                        ctx.mixers->SetActive(e.hash);
                        audio.ApplyMixer(*m);
                        status_ = "switched to '" + e.name + "'";
                    }
                }
            }
            ImGui::EndCombo();
        }
        if (all.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(create one from the Asset Browser)");
        }
    }

    if (ImGui::Button("+ Add Bus")) {
        AddBus(ctx, audio.RootBus());
    }
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        Save(ctx);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload Default")) {
        audio.ApplyMixer(DefaultMixer());
        status_ = "reset to the built-in Master/BGM/SE/UI layout (not saved yet)";
    }

    // ---- リバーブ (送り先は 1 本のグローバルバス。APO の制約でステレオ固定) ----
    ImGui::SeparatorText("Reverb");
    if (!audio.HasReverbBus()) {
        ImGui::TextDisabled("reverb bus is unavailable on this device");
    } else {
        int preset = audio.ReverbPreset();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo("preset", ReverbPresetName(preset))) {
            for (int i = 0; i < kReverbPresetCount; ++i) {
                if (ImGui::Selectable(ReverbPresetName(i), i == preset)) {
                    audio.SetReverbPreset(i);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        float wet = audio.ReverbWetDryMix();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::SliderFloat("wet/dry", &wet, 0.0f, 100.0f, "%.0f %%")) {
            audio.SetReverbWetDryMix(wet);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(Default = no reverb)");
    }

    if (!status_.empty()) {
        ImGui::TextWrapped("%s", status_.c_str());
    }
}

void AudioMixerWindow::OnImGui(EngineContext& ctx)
{
    if (!open) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(660.0f, 540.0f), ImGuiCond_FirstUseEver);
    // 窓名はアイコン無しの素の文字列にする — LayoutManager / DockBuilder / Window メニューが
    // この文字列で参照するため (既存パネルと同じ規約、M45b の教訓)
    if (!ImGui::Begin("Audio Mixer", &open)) {
        ImGui::End();
        return;
    }
    if (ctx.audio == nullptr) {
        ImGui::TextDisabled("audio system is not available");
        ImGui::End();
        return;
    }
    if (!ctx.audio->IsReady()) {
        ImGui::TextDisabled("audio device is not available (--no-audio) — faders are inert");
    }

    // メーターは**窓が開いている間だけ**読む。実時間の減衰が要るので dt を渡す
    // (6500fps では同じ処理済みブロックを何度も読むことになる)
    ctx.audio->PollBusMeters(ImGui::GetIO().DeltaTime);

    const float footerHeight = ImGui::GetFrameHeightWithSpacing() * 4.0f + 40.0f;
    if (ImGui::BeginChild("##strips", ImVec2(0.0f, -footerHeight), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        for (int i = 0; i < ctx.audio->BusCount(); ++i) {
            if (i != 0) {
                ImGui::SameLine(0.0f, 8.0f);
            }
            DrawStrip(ctx, i);
        }
    }
    ImGui::EndChild();

    DrawFooter(ctx);
    ImGui::End();
}

} // namespace mye
