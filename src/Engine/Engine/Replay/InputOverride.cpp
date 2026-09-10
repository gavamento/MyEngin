#include "Engine/Engine/Replay/InputOverride.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mye {
namespace {

void SetKey(InputSnapshot& in, uint8_t vk)
{
    in.keys[vk >> 3] |= static_cast<uint8_t>(1u << (vk & 7u));
}

uint16_t LowestBit(uint16_t mask)
{
    return static_cast<uint16_t>(mask & (0u - mask));
}

int16_t StickValue(float v)
{
    const float c = std::clamp(v, -1.0f, 1.0f);
    return static_cast<int16_t>(std::lround(c * 32767.0f));
}

uint8_t TriggerValue(float v)
{
    const float c = std::clamp(std::fabs(v), 0.0f, 1.0f);
    return static_cast<uint8_t>(std::lround(c * 255.0f));
}

void CopyLabel(InputOverride& o, const std::string& name)
{
    std::strncpy(o.label, name.c_str(), sizeof(o.label) - 1);
}

} // namespace

void InputOverride::ApplyTo(InputSnapshot& in) const
{
    for (size_t i = 0; i < sizeof(in.keys); ++i) {
        in.keys[i] |= orBits.keys[i];
    }
    in.mouseButtons |= orBits.mouseButtons;
    in.padButtons |= orBits.padButtons;
    in.padConnected |= orBits.padConnected;
    if (setMask & kSetPadLX) {
        in.padLX = setValues.padLX;
    }
    if (setMask & kSetPadLY) {
        in.padLY = setValues.padLY;
    }
    if (setMask & kSetPadRX) {
        in.padRX = setValues.padRX;
    }
    if (setMask & kSetPadRY) {
        in.padRY = setValues.padRY;
    }
    if (setMask & kSetPadLT) {
        in.padLeftTrigger = setValues.padLeftTrigger;
    }
    if (setMask & kSetPadRT) {
        in.padRightTrigger = setValues.padRightTrigger;
    }
    if (setMask & kSetMouseDX) {
        in.mouseDeltaX = setValues.mouseDeltaX;
    }
    if (setMask & kSetMouseDY) {
        in.mouseDeltaY = setValues.mouseDeltaY;
    }
}

InputOverride InputOverride::HoldAction(const InputActionDef& def, uint32_t lane, uint64_t from,
                                        uint64_t to)
{
    InputOverride o;
    o.lane = lane;
    o.fromTick = from;
    o.toTick = to;
    CopyLabel(o, def.name);
    if (!def.keys.empty()) {
        SetKey(o.orBits, def.keys.front());
    } else if (def.padMask != 0) {
        o.orBits.padButtons = LowestBit(def.padMask);
        o.orBits.padConnected = 1;
    } else if (def.mouseMask != 0) {
        o.orBits.mouseButtons = static_cast<uint8_t>(def.mouseMask & (0u - def.mouseMask));
    }
    return o;
}

InputOverride InputOverride::HoldAxis(const InputAxisDef& def, float value, uint32_t lane,
                                      uint64_t from, uint64_t to)
{
    InputOverride o;
    o.lane = lane;
    o.fromTick = from;
    o.toTick = to;
    CopyLabel(o, def.name);
    if (value > 0.0f && def.posKey != 0) {
        SetKey(o.orBits, def.posKey);
        return o;
    }
    if (value < 0.0f && def.negKey != 0) {
        SetKey(o.orBits, def.negKey);
        return o;
    }
    switch (def.padAxis) {
    case PadAxis::LX:
        o.setMask = kSetPadLX;
        o.setValues.padLX = StickValue(value);
        break;
    case PadAxis::LY:
        o.setMask = kSetPadLY;
        o.setValues.padLY = StickValue(value);
        break;
    case PadAxis::RX:
        o.setMask = kSetPadRX;
        o.setValues.padRX = StickValue(value);
        break;
    case PadAxis::RY:
        o.setMask = kSetPadRY;
        o.setValues.padRY = StickValue(value);
        break;
    case PadAxis::LT:
        o.setMask = kSetPadLT;
        o.setValues.padLeftTrigger = TriggerValue(value);
        break;
    case PadAxis::RT:
        o.setMask = kSetPadRT;
        o.setValues.padRightTrigger = TriggerValue(value);
        break;
    default:
        break;
    }
    if (o.setMask != 0) {
        o.orBits.padConnected = 1; // パッドの軸を置くなら「繋がっている」も立てる
    }
    return o;
}

void InputOverrideSet::Apply(uint64_t tick, InputSnapshot* lanes, uint32_t playerCount) const
{
    for (const InputOverride& o : items) {
        if (o.lane < playerCount && o.lane < kMaxPlayers && o.Applies(tick)) {
            o.ApplyTo(lanes[o.lane]);
        }
    }
}

} // namespace mye
