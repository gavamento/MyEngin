#include "Editor/Windows/SoundGenWindow.h"

#include <filesystem>
#include <string_view>

#include "Engine/Core/Hash.h"
#include "Engine/Core/Localization.h"
#include "Engine/Platform/PathUtil.h"

#include "fontawesome/IconsFontAwesome6.h"
#include "imgui.h"

namespace mye {
namespace {

namespace fs = std::filesystem;

// 試聴用クリップの固定 AssetID。実アセットのパスハッシュとは衝突しない専用キー
constexpr AssetID kPreviewClipId{ HashStr("__mye_soundgen_preview__") };

constexpr float kMaxDurationSec = 10.0f; // ドラッグ操作で数百 MB を確保しないための上限

const char* const kWaveNames[] = { "Sine", "Square", "Saw", "Triangle", "Noise" };
const uint32_t kSampleRates[] = { 22050u, 44100u, 48000u };
const char* const kSampleRateNames[] = { "22050 Hz", "44100 Hz", "48000 Hz" };

// ファイル名として使えない文字を落とす (RenameAsset と同じ規約)
std::string SanitizeStem(const char* raw)
{
    constexpr std::string_view kBad = "\\/:*?\"<>|";
    std::string out;
    for (const char* p = raw; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x20 || kBad.find(*p) != std::string_view::npos) {
            continue;
        }
        out.push_back(*p);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back(); // 末尾の空白/ドットは Win32 が黙って落とすので先に潰す
    }
    return out;
}

} // namespace

void SoundGenWindow::ApplyPreset(int preset)
{
    params_ = SynthParams{}; // 明示しないフィールドは既定へ戻す
    switch (preset) {
    case 0: // Jump — 上昇スイープの矩形波
        params_.wave = SynthWave::Square;
        params_.freqStart = 220.0f;
        params_.freqEnd = 680.0f;
        params_.durationSec = 0.22f;
        params_.amplitude = 0.5f;
        params_.duty = 0.5f;
        params_.attackSec = 0.005f;
        params_.decaySec = 0.04f;
        params_.sustainLevel = 0.6f;
        params_.releaseSec = 0.12f;
        break;
    case 1: // Coin — 細いデューティの高音 + 短い上昇
        params_.wave = SynthWave::Square;
        params_.freqStart = 988.0f;
        params_.freqEnd = 1319.0f;
        params_.durationSec = 0.32f;
        params_.amplitude = 0.42f;
        params_.duty = 0.32f;
        params_.attackSec = 0.004f;
        params_.decaySec = 0.06f;
        params_.sustainLevel = 0.85f;
        params_.releaseSec = 0.2f;
        break;
    case 2: // Explosion — ノイズ + 長い減衰
        params_.wave = SynthWave::Noise;
        params_.freqStart = 120.0f;
        params_.freqEnd = 40.0f;
        params_.durationSec = 0.85f;
        params_.amplitude = 0.8f;
        params_.attackSec = 0.004f;
        params_.decaySec = 0.28f;
        params_.sustainLevel = 0.35f;
        params_.releaseSec = 0.55f;
        break;
    case 3: // Laser — 下降スイープの鋸波
    default:
        params_.wave = SynthWave::Saw;
        params_.freqStart = 1400.0f;
        params_.freqEnd = 180.0f;
        params_.durationSec = 0.35f;
        params_.amplitude = 0.45f;
        params_.attackSec = 0.002f;
        params_.decaySec = 0.05f;
        params_.sustainLevel = 0.55f;
        params_.releaseSec = 0.26f;
        break;
    }
    dirty_ = true;
}

void SoundGenWindow::EnsureRendered()
{
    if (!dirty_) {
        return;
    }
    SynthRender(params_, clip_);
    dirty_ = false;
    ++clipSerial_;
}

void SoundGenWindow::RebuildPeaks(int columns)
{
    peakColumns_ = columns;
    peakSerial_ = clipSerial_;
    peakMin_.assign(static_cast<size_t>(columns), 0.0f);
    peakMax_.assign(static_cast<size_t>(columns), 0.0f);
    const size_t frames = clip_.Frames();
    if (columns <= 0 || frames == 0) {
        return;
    }
    const size_t stride = clip_.channels;
    for (int c = 0; c < columns; ++c) {
        // 列 → サンプル範囲。1 列に 1 サンプルも入らない (拡大時) 場合は最低 1 サンプル取る
        size_t begin = static_cast<size_t>(static_cast<double>(c) * frames / columns);
        size_t end = static_cast<size_t>(static_cast<double>(c + 1) * frames / columns);
        if (begin >= frames) {
            begin = frames - 1;
        }
        if (end <= begin) {
            end = begin + 1;
        }
        if (end > frames) {
            end = frames;
        }
        float lo = 1.0f;
        float hi = -1.0f;
        for (size_t i = begin; i < end; ++i) {
            const float v = static_cast<float>(clip_.samples[i * stride]) / 32768.0f; // 第 1ch のみ
            lo = v < lo ? v : lo;
            hi = v > hi ? v : hi;
        }
        peakMin_[static_cast<size_t>(c)] = lo;
        peakMax_[static_cast<size_t>(c)] = hi;
    }
}

void SoundGenWindow::DrawWaveform(float height)
{
    const float avail = ImGui::GetContentRegionAvail().x;
    const float width = avail > 64.0f ? avail : 64.0f;
    ImGui::InvisibleButton("##wave", ImVec2(width, height));
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(24, 24, 28, 255));
    dl->AddRect(p0, p1, IM_COL32(90, 90, 100, 255));

    const float midY = (p0.y + p1.y) * 0.5f;
    dl->AddLine(ImVec2(p0.x, midY), ImVec2(p1.x, midY), IM_COL32(70, 70, 80, 255));

    const int columns = static_cast<int>(p1.x - p0.x);
    if (columns <= 0) {
        return;
    }
    if (columns != peakColumns_ || peakSerial_ != clipSerial_) {
        RebuildPeaks(columns);
    }
    const float half = (p1.y - p0.y) * 0.5f - 2.0f;
    for (int c = 0; c < columns; ++c) {
        const float x = p0.x + static_cast<float>(c) + 0.5f;
        const float yTop = midY - peakMax_[static_cast<size_t>(c)] * half;
        float yBottom = midY - peakMin_[static_cast<size_t>(c)] * half;
        if (yBottom < yTop + 1.0f) {
            yBottom = yTop + 1.0f; // 無音区間でも 1px の線を残す
        }
        dl->AddLine(ImVec2(x, yTop), ImVec2(x, yBottom), IM_COL32(120, 200, 255, 255));
    }
}

void SoundGenWindow::Preview(EngineContext& ctx)
{
    if (ctx.audio == nullptr || !ctx.audio->IsReady()) {
        status_ = "audio device is not available";
        return;
    }
    EnsureRendered();
    if (clip_.Empty()) {
        status_ = "nothing to play (duration is 0)";
        return;
    }
    // RegisterClip は差し替え前に StopVoicesUsingClip を通すので、前回のプレビューは
    // ここで自動的に止まる (再生中のバイト列を差し替える use-after-free を避ける契約)
    ctx.audio->RegisterClip(kPreviewClipId, clip_, "(sound generator preview)");
    PlayDesc desc;
    desc.clip = kPreviewClipId;
    desc.bus = AudioSystem::kBusUi; // エディタ操作音なので UI バス
    desc.volume = 1.0f;
    desc.priority = 255;            // 明示操作の試聴は他の音に負けない
    previewHandle_ = ctx.audio->Play(desc);
    status_.clear();
}

void SoundGenWindow::Save(EngineContext& ctx, const std::wstring& assetDir)
{
    EnsureRendered();
    if (clip_.Empty()) {
        status_ = "nothing to save (duration is 0)";
        return;
    }
    // 保存先は AssetBrowser の表示中フォルダ。**ctx.assetsRoot 基準**でフォールバックする
    // (FindAssetsRoot() を使うとプロジェクト起動時にエンジンリポジトリへ落ちる)
    const std::wstring dir = assetDir.empty() ? (ctx.assetsRoot + L"\\audio") : assetDir;
    std::error_code ec;
    fs::create_directories(fs::path{ dir }, ec);

    std::string stem = SanitizeStem(saveName_);
    if (stem.empty()) {
        stem = "new_sfx";
    }
    // 同名は " (N)" 連番。既に読み込み済みの .wav を黙って壊さない (インポートと同じ流儀)
    std::wstring path = dir + L"\\" + Utf8ToWide(stem) + L".wav";
    for (int n = 1; n < 1000 && fs::exists(path); ++n) {
        path = dir + L"\\" + Utf8ToWide(stem) + L" (" + std::to_wstring(n) + L").wav";
    }
    if (!WriteWavToFile(clip_, path)) {
        status_ = "save failed (see Console)";
        return;
    }
    if (ctx.audio != nullptr) {
        ctx.audio->LoadClipFile(path); // 生成直後に登録 → そのまま試聴/参照できる
    }
    status_ = "saved: " + WideToUtf8(fs::path{ path }.filename().wstring());
}

void SoundGenWindow::OnImGui(EngineContext& ctx, const std::wstring& assetDir)
{
    if (!open) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(440.0f, 700.0f), ImGuiCond_FirstUseEver);
    // 窓名はアイコン無しの素の文字列にする — LayoutManager / DockBuilder / Window メニューが
    // この文字列で参照するため (既存パネルと同じ規約)
    if (!ImGui::Begin(Tr(StrId::Win_SoundGenerator), &open)) {
        ImGui::End();
        return;
    }

    // ---- プリセット ----
    ImGui::TextUnformatted("Presets");
    static const char* const kPresetNames[] = { "Jump", "Coin", "Explosion", "Laser" };
    for (int i = 0; i < 4; ++i) {
        if (i != 0) {
            ImGui::SameLine();
        }
        if (ImGui::Button(kPresetNames[i])) {
            ApplyPreset(i);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ARROW_ROTATE_LEFT " Reset")) {
        params_ = SynthParams{};
        dirty_ = true;
    }
    ImGui::Separator();

    bool changed = false;
    ImGui::PushItemWidth(-140.0f);

    // ---- 波形 ----
    int wave = static_cast<int>(params_.wave);
    if (ImGui::Combo("Waveform", &wave, kWaveNames, IM_ARRAYSIZE(kWaveNames))) {
        params_.wave = static_cast<SynthWave>(wave);
        changed = true;
    }
    if (params_.wave == SynthWave::Square) {
        changed |= ImGui::SliderFloat("Duty", &params_.duty, 0.05f, 0.95f, "%.2f");
    }
    if (params_.wave == SynthWave::Noise) {
        // シードは Pcg32 に渡す決定論の種 (spec 11.2 規則 8: rand は使わない)。
        // ボタンは黄金比定数を足すだけ — 同じ操作列なら常に同じ音になる
        ImGui::Text("Noise seed: %llu", static_cast<unsigned long long>(params_.noiseSeed));
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_DICE " Shuffle")) {
            params_.noiseSeed += 0x9E3779B97F4A7C15ull;
            changed = true;
        }
    }

    changed |= ImGui::DragFloat("Freq start (Hz)", &params_.freqStart, 2.0f, 20.0f, 8000.0f, "%.0f");
    changed |= ImGui::DragFloat("Freq end (Hz)", &params_.freqEnd, 2.0f, 20.0f, 8000.0f, "%.0f");
    changed |= ImGui::DragFloat("Duration (s)", &params_.durationSec, 0.005f, 0.01f,
                                kMaxDurationSec, "%.3f");
    changed |= ImGui::SliderFloat("Amplitude", &params_.amplitude, 0.0f, 1.0f, "%.2f");

    ImGui::SeparatorText("Envelope (ADSR)");
    changed |= ImGui::DragFloat("Attack (s)", &params_.attackSec, 0.002f, 0.0f, kMaxDurationSec, "%.3f");
    changed |= ImGui::DragFloat("Decay (s)", &params_.decaySec, 0.002f, 0.0f, kMaxDurationSec, "%.3f");
    changed |= ImGui::SliderFloat("Sustain", &params_.sustainLevel, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::DragFloat("Release (s)", &params_.releaseSec, 0.002f, 0.0f, kMaxDurationSec, "%.3f");
    ImGui::TextDisabled("(A+D+R が全長を超えると比例縮小されます)");

    ImGui::SeparatorText("Format");
    int rateIndex = 1;
    for (int i = 0; i < IM_ARRAYSIZE(kSampleRates); ++i) {
        if (kSampleRates[i] == params_.sampleRate) {
            rateIndex = i;
        }
    }
    if (ImGui::Combo("Sample rate", &rateIndex, kSampleRateNames, IM_ARRAYSIZE(kSampleRateNames))) {
        params_.sampleRate = kSampleRates[rateIndex];
        changed = true;
    }
    int channels = params_.channels >= 2 ? 2 : 1;
    if (ImGui::RadioButton("Mono", &channels, 1)) {
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Stereo", &channels, 2)) {
        changed = true;
    }
    params_.channels = static_cast<uint16_t>(channels);
    ImGui::PopItemWidth();

    if (changed) {
        // 上限を跨いだ入力 (Ctrl+クリックの直接入力は clamp されない) をここで畳む
        if (params_.durationSec > kMaxDurationSec) {
            params_.durationSec = kMaxDurationSec;
        }
        if (params_.durationSec < 0.0f) {
            params_.durationSec = 0.0f;
        }
        dirty_ = true;
    }

    // ---- 波形プレビュー ----
    ImGui::SeparatorText("Preview");
    EnsureRendered();
    DrawWaveform(90.0f);
    ImGui::Text("%.3f s / %zu frames / %u ch @ %u Hz", clip_.Seconds(), clip_.Frames(),
                static_cast<unsigned>(clip_.channels), clip_.sampleRate);

    const bool audioReady = ctx.audio != nullptr && ctx.audio->IsReady();
    ImGui::BeginDisabled(!audioReady);
    if (ImGui::Button(ICON_FA_PLAY " Play")) {
        Preview(ctx);
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_STOP " Stop")) {
        ctx.audio->Stop(previewHandle_);
        previewHandle_ = {};
    }
    ImGui::EndDisabled();
    if (!audioReady) {
        ImGui::SameLine();
        ImGui::TextDisabled("(audio disabled)");
    }

    // ---- 書き出し ----
    ImGui::SeparatorText("Save");
    const std::wstring dir = assetDir.empty() ? (ctx.assetsRoot + L"\\audio") : assetDir;
    // assets ルートからの相対で見せる (絶対パスは長すぎて窓に収まらない)
    std::wstring shown = dir;
    if (dir.size() > ctx.assetsRoot.size() && dir.compare(0, ctx.assetsRoot.size(), ctx.assetsRoot) == 0) {
        shown = L"assets" + dir.substr(ctx.assetsRoot.size());
    }
    ImGui::TextDisabled("%s", WideToUtf8(shown).c_str());
    ImGui::PushItemWidth(-140.0f);
    ImGui::InputText("##savename", saveName_, IM_ARRAYSIZE(saveName_));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    ImGui::TextUnformatted(".wav");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FLOPPY_DISK " Save")) {
        Save(ctx, assetDir);
    }
    if (!status_.empty()) {
        ImGui::TextUnformatted(status_.c_str());
    }

    ImGui::End();
}

} // namespace mye
