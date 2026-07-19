#pragma once
// DLL 境界 (Engine <-> GameLogic.dll) を越える POD 型 (engine_spec.md 8.4)。
// 規則: このディレクトリのヘッダに STL / DirectXMath / エンジンヘッダを含めてはならない。
// C ABI + trivially copyable な型のみ。

#include <stdint.h>

struct MyeVec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct MyeVec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct MyeVec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

struct MyeQuat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct MyeColor {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

// mye::EntityID (Engine/Core/EntityID.h) とバイナリ互換。エンジン側で static_assert される
struct MyeEntityId {
    uint32_t index = 0xFFFFFFFFu;
    uint32_t generation = 0;
};

inline bool MyeEntityIdIsNull(MyeEntityId id)
{
    return id.index == 0xFFFFFFFFu;
}
