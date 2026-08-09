#include "Engine/Platform/InputActions.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

#include "nlohmann/json.hpp"

namespace mye {

using json = nlohmann::json;

namespace {

struct VkEntry {
    uint8_t vk;
    const char* name;
};
constexpr VkEntry kVkTable[] = {
#define MYE_VK(vk, name) { vk, name },
#include "Engine/Platform/VkNameTable.inl"
#undef MYE_VK
};

// MyePadButton (Shared/EngineAPI.h) = XINPUT_GAMEPAD_* と同値の ABI 定数。
// 値は将来も不変 — Platform 層から Shared を include しないためここに再掲する
struct PadEntry {
    uint16_t mask;
    const char* name;
};
constexpr PadEntry kPadTable[] = {
    { 0x0001, "DPadUp" },   { 0x0002, "DPadDown" }, { 0x0004, "DPadLeft" },
    { 0x0008, "DPadRight" }, { 0x0010, "Start" },   { 0x0020, "Back" },
    { 0x0040, "LThumb" },   { 0x0080, "RThumb" },   { 0x0100, "LB" },
    { 0x0200, "RB" },       { 0x1000, "A" },        { 0x2000, "B" },
    { 0x4000, "X" },        { 0x8000, "Y" },
};

// PadAxis / mouseButtons ビットと同順
constexpr const char* kPadAxisNames[] = { "None", "LX", "LY", "RX", "RY", "LT", "RT" };
constexpr const char* kMouseNames[] = { "Left", "Right", "Middle", "X1", "X2" };
constexpr int kMouseButtonCount = 5;

// スティック生値 → [-1, 1] (-32768 は -1 に飽和させて対称にする)
float NormStick(int16_t raw)
{
    return std::max(-1.0f, static_cast<float>(raw) / 32767.0f);
}

bool ActionDown(const InputActionDef& a, const InputSnapshot& s)
{
    for (uint8_t vk : a.keys) {
        if (s.KeyDown(vk)) {
            return true;
        }
    }
    if ((a.padMask & s.padButtons) != 0) {
        return true;
    }
    if ((a.mouseMask & s.mouseButtons) != 0) {
        return true;
    }
    return false;
}

float EvalAxis(const InputAxisDef& a, const InputSnapshot& s)
{
    float v = 0.0f;
    if (a.posKey != 0 && s.KeyDown(a.posKey)) {
        v += 1.0f;
    }
    if (a.negKey != 0 && s.KeyDown(a.negKey)) {
        v -= 1.0f;
    }
    switch (a.padAxis) {
    case PadAxis::LX: v += InputActions::ApplyDeadzone(NormStick(s.padLX), a.deadzone); break;
    case PadAxis::LY: v += InputActions::ApplyDeadzone(NormStick(s.padLY), a.deadzone); break;
    case PadAxis::RX: v += InputActions::ApplyDeadzone(NormStick(s.padRX), a.deadzone); break;
    case PadAxis::RY: v += InputActions::ApplyDeadzone(NormStick(s.padRY), a.deadzone); break;
    case PadAxis::LT:
        v += InputActions::ApplyDeadzone(static_cast<float>(s.padLeftTrigger) / 255.0f, a.deadzone);
        break;
    case PadAxis::RT:
        v += InputActions::ApplyDeadzone(static_cast<float>(s.padRightTrigger) / 255.0f, a.deadzone);
        break;
    default:
        break;
    }
    return std::clamp(v, -1.0f, 1.0f);
}

} // namespace

void InputActions::Load(const std::wstring& assetsRoot, bool force)
{
    if (!force && loadedRoot_ == assetsRoot) {
        return;
    }
    loadedRoot_ = assetsRoot;
    actions_.clear();
    axes_.clear();
    actionBits_.clear();
    axisValues_.clear();
    const std::filesystem::path path(assetsRoot + L"\\input\\actions.json");
    std::ifstream f(path);
    if (!f) {
        return; // 不在 = 空マップ = 完全 no-op (決定台帳 4)
    }
    std::stringstream ss;
    ss << f.rdbuf();
    if (LoadFromJsonText(ss.str())) {
        MYE_LOG_INFO("[input] actions.json: %zu action(s), %zu axis(es)", actions_.size(),
                     axes_.size());
    } else {
        MYE_LOG_WARN("[input] actions.json unusable - action map disabled (%s)",
                     WideToUtf8(path.wstring()).c_str());
    }
}

bool InputActions::Save(const std::wstring& assetsRoot) const
{
    std::error_code ec;
    const std::filesystem::path dir(assetsRoot + L"\\input");
    std::filesystem::create_directories(dir, ec); // 既存でも成功扱い
    std::ofstream out(dir / L"actions.json");
    if (!out) {
        MYE_LOG_WARN("[input] failed to write actions.json");
        return false;
    }
    out << ToJsonText();
    return true;
}

bool InputActions::LoadFromJsonText(const std::string& text)
{
    actions_.clear();
    axes_.clear();
    actionBits_.clear();
    axisValues_.clear();
    json j;
    try {
        j = json::parse(text);
    } catch (const json::exception& e) {
        MYE_LOG_WARN("[input] actions JSON parse error: %s", e.what());
        return false;
    }
    if (!j.is_object()) {
        MYE_LOG_WARN("[input] actions JSON root is not an object");
        return false;
    }

    if (j.contains("actions") && j["actions"].is_array()) {
        for (const auto& e : j["actions"]) {
            if (!e.is_object()) {
                continue;
            }
            InputActionDef def;
            def.name = e.value("name", std::string());
            if (def.name.empty()) {
                MYE_LOG_WARN("[input] action without name - skipped");
                continue;
            }
            def.nameHash = HashStr(def.name);
            const bool dup =
                std::any_of(actions_.begin(), actions_.end(),
                            [&](const InputActionDef& a) { return a.nameHash == def.nameHash; });
            if (dup) {
                MYE_LOG_WARN("[input] duplicate action '%s' - first definition wins",
                             def.name.c_str());
                continue;
            }
            if (e.contains("keys") && e["keys"].is_array()) {
                for (const auto& k : e["keys"]) {
                    if (!k.is_string()) {
                        continue;
                    }
                    const std::string kn = k.get<std::string>();
                    const uint8_t vk = VkFromName(kn);
                    if (vk == 0) {
                        MYE_LOG_WARN("[input] action '%s': unknown key '%s' - skipped",
                                     def.name.c_str(), kn.c_str());
                        continue;
                    }
                    def.keys.push_back(vk);
                }
            }
            if (e.contains("pad") && e["pad"].is_array()) {
                for (const auto& p : e["pad"]) {
                    if (!p.is_string()) {
                        continue;
                    }
                    const std::string pn = p.get<std::string>();
                    const uint16_t mask = PadMaskFromName(pn);
                    if (mask == 0) {
                        MYE_LOG_WARN("[input] action '%s': unknown pad button '%s' - skipped",
                                     def.name.c_str(), pn.c_str());
                        continue;
                    }
                    def.padMask |= mask;
                }
            }
            if (e.contains("mouse") && e["mouse"].is_array()) {
                for (const auto& m : e["mouse"]) {
                    if (!m.is_string()) {
                        continue;
                    }
                    const std::string mn = m.get<std::string>();
                    const int bit = MouseBitFromName(mn);
                    if (bit < 0) {
                        MYE_LOG_WARN("[input] action '%s': unknown mouse button '%s' - skipped",
                                     def.name.c_str(), mn.c_str());
                        continue;
                    }
                    def.mouseMask |= static_cast<uint8_t>(1u << bit);
                }
            }
            actions_.push_back(std::move(def));
        }
    }

    if (j.contains("axes") && j["axes"].is_array()) {
        for (const auto& e : j["axes"]) {
            if (!e.is_object()) {
                continue;
            }
            InputAxisDef def;
            def.name = e.value("name", std::string());
            if (def.name.empty()) {
                MYE_LOG_WARN("[input] axis without name - skipped");
                continue;
            }
            def.nameHash = HashStr(def.name);
            const bool dup =
                std::any_of(axes_.begin(), axes_.end(),
                            [&](const InputAxisDef& a) { return a.nameHash == def.nameHash; });
            if (dup) {
                MYE_LOG_WARN("[input] duplicate axis '%s' - first definition wins",
                             def.name.c_str());
                continue;
            }
            const std::string pos = e.value("posKey", std::string());
            const std::string neg = e.value("negKey", std::string());
            if (!pos.empty()) {
                def.posKey = VkFromName(pos);
                if (def.posKey == 0) {
                    MYE_LOG_WARN("[input] axis '%s': unknown posKey '%s' - skipped",
                                 def.name.c_str(), pos.c_str());
                }
            }
            if (!neg.empty()) {
                def.negKey = VkFromName(neg);
                if (def.negKey == 0) {
                    MYE_LOG_WARN("[input] axis '%s': unknown negKey '%s' - skipped",
                                 def.name.c_str(), neg.c_str());
                }
            }
            const std::string axisName = e.value("padAxis", std::string("None"));
            def.padAxis = PadAxisFromName(axisName);
            if (def.padAxis == PadAxis::None && axisName != "None" && !axisName.empty()) {
                MYE_LOG_WARN("[input] axis '%s': unknown padAxis '%s' - ignored",
                             def.name.c_str(), axisName.c_str());
            }
            def.deadzone = std::clamp(e.value("deadzone", 0.24f), 0.0f, 0.95f);
            axes_.push_back(std::move(def));
        }
    }

    actionBits_.assign(actions_.size(), 0);
    axisValues_.assign(axes_.size(), 0.0f);
    return true;
}

std::string InputActions::ToJsonText() const
{
    json j = json::object();
    json actions = json::array();
    for (const InputActionDef& a : actions_) {
        json e = json::object();
        e["name"] = a.name;
        json keys = json::array();
        for (uint8_t vk : a.keys) {
            keys.push_back(VkNameStr(vk));
        }
        e["keys"] = std::move(keys);
        json pad = json::array();
        for (const PadEntry& p : kPadTable) {
            if ((a.padMask & p.mask) != 0) {
                pad.push_back(p.name);
            }
        }
        e["pad"] = std::move(pad);
        json mouse = json::array();
        for (int b = 0; b < kMouseButtonCount; ++b) {
            if ((a.mouseMask & (1u << b)) != 0) {
                mouse.push_back(kMouseNames[b]);
            }
        }
        e["mouse"] = std::move(mouse);
        actions.push_back(std::move(e));
    }
    j["actions"] = std::move(actions);

    json axes = json::array();
    for (const InputAxisDef& a : axes_) {
        json e = json::object();
        e["name"] = a.name;
        if (a.posKey != 0) {
            e["posKey"] = VkNameStr(a.posKey);
        }
        if (a.negKey != 0) {
            e["negKey"] = VkNameStr(a.negKey);
        }
        e["padAxis"] = PadAxisName(a.padAxis);
        e["deadzone"] = a.deadzone;
        axes.push_back(std::move(e));
    }
    j["axes"] = std::move(axes);
    return j.dump(2) + "\n";
}

void InputActions::Evaluate(const InputSnapshot& cur, const InputSnapshot& prev)
{
    // 定義列がロード以外で伸縮した場合 (編集 UI) もここで追随する
    if (actionBits_.size() != actions_.size()) {
        actionBits_.assign(actions_.size(), 0);
    }
    if (axisValues_.size() != axes_.size()) {
        axisValues_.assign(axes_.size(), 0.0f);
    }
    for (size_t i = 0; i < actions_.size(); ++i) {
        const bool now = ActionDown(actions_[i], cur);
        const bool was = ActionDown(actions_[i], prev);
        uint8_t bits = 0;
        if (now) {
            bits |= kActionHeld;
        }
        if (now && !was) {
            bits |= kActionPressed;
        }
        if (!now && was) {
            bits |= kActionReleased;
        }
        actionBits_[i] = bits;
    }
    for (size_t i = 0; i < axes_.size(); ++i) {
        axisValues_[i] = EvalAxis(axes_[i], cur);
    }
}

uint32_t InputActions::ActionState(uint64_t nameHash) const
{
    for (size_t i = 0; i < actions_.size(); ++i) {
        if (actions_[i].nameHash == nameHash) {
            return ActionStateAt(i);
        }
    }
    return 0;
}

float InputActions::AxisValue(uint64_t nameHash) const
{
    for (size_t i = 0; i < axes_.size(); ++i) {
        if (axes_[i].nameHash == nameHash) {
            return AxisValueAt(i);
        }
    }
    return 0.0f;
}

uint32_t InputActions::ActionStateAt(size_t i) const
{
    return (i < actionBits_.size()) ? actionBits_[i] : 0u;
}

float InputActions::AxisValueAt(size_t i) const
{
    return (i < axisValues_.size()) ? axisValues_[i] : 0.0f;
}

const char* InputActions::VkNameInTable(uint8_t vk)
{
    for (const VkEntry& e : kVkTable) {
        if (e.vk == vk) {
            return e.name;
        }
    }
    return nullptr;
}

std::string InputActions::VkNameStr(uint8_t vk)
{
    if (const char* n = VkNameInTable(vk)) {
        return n;
    }
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0x%02X", vk);
    return buf;
}

uint8_t InputActions::VkFromName(std::string_view name)
{
    for (const VkEntry& e : kVkTable) {
        if (name == e.name) {
            return e.vk;
        }
    }
    // "0xNN" 16 進名 (未収載 VK の往復用)
    if (name.size() > 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        unsigned v = 0;
        for (size_t i = 2; i < name.size(); ++i) {
            const char c = name[i];
            unsigned d;
            if (c >= '0' && c <= '9') {
                d = static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                d = static_cast<unsigned>(c - 'a') + 10u;
            } else if (c >= 'A' && c <= 'F') {
                d = static_cast<unsigned>(c - 'A') + 10u;
            } else {
                return 0;
            }
            v = v * 16u + d;
            if (v > 0xFFu) {
                return 0;
            }
        }
        return static_cast<uint8_t>(v); // "0x00" は 0 = 割り当てなしに落ちる (意図どおり)
    }
    return 0;
}

const char* InputActions::PadButtonName(uint16_t singleBitMask)
{
    for (const PadEntry& e : kPadTable) {
        if (e.mask == singleBitMask) {
            return e.name;
        }
    }
    return nullptr;
}

uint16_t InputActions::PadMaskFromName(std::string_view name)
{
    for (const PadEntry& e : kPadTable) {
        if (name == e.name) {
            return e.mask;
        }
    }
    return 0;
}

const char* InputActions::PadAxisName(PadAxis a)
{
    const size_t i = static_cast<size_t>(a);
    return (i < static_cast<size_t>(PadAxis::Count)) ? kPadAxisNames[i] : "None";
}

PadAxis InputActions::PadAxisFromName(std::string_view name)
{
    for (size_t i = 0; i < static_cast<size_t>(PadAxis::Count); ++i) {
        if (name == kPadAxisNames[i]) {
            return static_cast<PadAxis>(i);
        }
    }
    return PadAxis::None;
}

const char* InputActions::MouseButtonName(int bit)
{
    return (bit >= 0 && bit < kMouseButtonCount) ? kMouseNames[bit] : nullptr;
}

int InputActions::MouseBitFromName(std::string_view name)
{
    for (int i = 0; i < kMouseButtonCount; ++i) {
        if (name == kMouseNames[i]) {
            return i;
        }
    }
    return -1;
}

float InputActions::ApplyDeadzone(float v, float deadzone)
{
    if (deadzone <= 0.0f) {
        return std::clamp(v, -1.0f, 1.0f);
    }
    if (deadzone >= 1.0f) {
        return 0.0f;
    }
    const float a = std::fabs(v);
    if (a <= deadzone) {
        return 0.0f;
    }
    const float t = std::min(1.0f, (a - deadzone) / (1.0f - deadzone));
    return (v < 0.0f) ? -t : t;
}

} // namespace mye
