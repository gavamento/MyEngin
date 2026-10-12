//====================================================================================
//                          KernelCompat.cs
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          Compute.ForEach カーネルの C# 側 HLSL 互換ライブラリ
//====================================================================================
// 生成器 (エンジンが assets/scripts/Generated/Kernels.gen.cs に出力する C#) が使う HLSL 互換型と関数。
// ★演算順は Shared/KernelCompat.h (C++ 版) と同一にする。式の正本は plans/m91-compute-foreach/sub-01.md。
//   System.Numerics は使わない (Dot が SIMD の水平加算になり加算順が変わるため)。
//   Math.Min/Max ではなく三項演算子を使い、FMA 縮約が起きる書き方 (Math.FusedMultiplyAdd) を使わない。
// ★この型の契約は「C++ 版と同じ値を返す」こと。変更したら C++ 版との照合 (sub-06) を回す。
using System;
using System.Runtime.InteropServices;

namespace MyeScripting.KernelCompat
{
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct float2
    {
        public float x, y;

        public float2(float s) { x = s; y = s; }
        public float2(float x_, float y_) { x = x_; y = y_; }

        public static float2 operator +(float2 a, float2 b) => new float2((float)(a.x + b.x), (float)(a.y + b.y));
        public static float2 operator +(float2 a, float s) => new float2((float)(a.x + s), (float)(a.y + s));
        public static float2 operator +(float s, float2 b) => new float2((float)(s + b.x), (float)(s + b.y));
        public static float2 operator -(float2 a, float2 b) => new float2((float)(a.x - b.x), (float)(a.y - b.y));
        public static float2 operator -(float2 a, float s) => new float2((float)(a.x - s), (float)(a.y - s));
        public static float2 operator -(float s, float2 b) => new float2((float)(s - b.x), (float)(s - b.y));
        public static float2 operator *(float2 a, float2 b) => new float2((float)(a.x * b.x), (float)(a.y * b.y));
        public static float2 operator *(float2 a, float s) => new float2((float)(a.x * s), (float)(a.y * s));
        public static float2 operator *(float s, float2 b) => new float2((float)(s * b.x), (float)(s * b.y));
        public static float2 operator /(float2 a, float2 b) => new float2((float)(a.x / b.x), (float)(a.y / b.y));
        public static float2 operator /(float2 a, float s) => new float2((float)(a.x / s), (float)(a.y / s));
        public static float2 operator /(float s, float2 b) => new float2((float)(s / b.x), (float)(s / b.y));
        public static float2 operator -(float2 a) => new float2((float)(-a.x), (float)(-a.y));
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct float3
    {
        public float x, y, z;

        public float3(float s) { x = s; y = s; z = s; }
        public float3(float x_, float y_, float z_) { x = x_; y = y_; z = z_; }
        public float3(float2 xy, float z_) { x = xy.x; y = xy.y; z = z_; }
        public float3(float x_, float2 yz) { x = x_; y = yz.x; z = yz.y; }

        public static float3 operator +(float3 a, float3 b) => new float3((float)(a.x + b.x), (float)(a.y + b.y), (float)(a.z + b.z));
        public static float3 operator +(float3 a, float s) => new float3((float)(a.x + s), (float)(a.y + s), (float)(a.z + s));
        public static float3 operator +(float s, float3 b) => new float3((float)(s + b.x), (float)(s + b.y), (float)(s + b.z));
        public static float3 operator -(float3 a, float3 b) => new float3((float)(a.x - b.x), (float)(a.y - b.y), (float)(a.z - b.z));
        public static float3 operator -(float3 a, float s) => new float3((float)(a.x - s), (float)(a.y - s), (float)(a.z - s));
        public static float3 operator -(float s, float3 b) => new float3((float)(s - b.x), (float)(s - b.y), (float)(s - b.z));
        public static float3 operator *(float3 a, float3 b) => new float3((float)(a.x * b.x), (float)(a.y * b.y), (float)(a.z * b.z));
        public static float3 operator *(float3 a, float s) => new float3((float)(a.x * s), (float)(a.y * s), (float)(a.z * s));
        public static float3 operator *(float s, float3 b) => new float3((float)(s * b.x), (float)(s * b.y), (float)(s * b.z));
        public static float3 operator /(float3 a, float3 b) => new float3((float)(a.x / b.x), (float)(a.y / b.y), (float)(a.z / b.z));
        public static float3 operator /(float3 a, float s) => new float3((float)(a.x / s), (float)(a.y / s), (float)(a.z / s));
        public static float3 operator /(float s, float3 b) => new float3((float)(s / b.x), (float)(s / b.y), (float)(s / b.z));
        public static float3 operator -(float3 a) => new float3((float)(-a.x), (float)(-a.y), (float)(-a.z));
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct float4
    {
        public float x, y, z, w;

        public float4(float s) { x = s; y = s; z = s; w = s; }
        public float4(float x_, float y_, float z_, float w_) { x = x_; y = y_; z = z_; w = w_; }
        public float4(float3 xyz, float w_) { x = xyz.x; y = xyz.y; z = xyz.z; w = w_; }
        public float4(float x_, float3 yzw) { x = x_; y = yzw.x; z = yzw.y; w = yzw.z; }
        public float4(float2 xy, float2 zw) { x = xy.x; y = xy.y; z = zw.x; w = zw.y; }

        public static float4 operator +(float4 a, float4 b) => new float4((float)(a.x + b.x), (float)(a.y + b.y), (float)(a.z + b.z), (float)(a.w + b.w));
        public static float4 operator +(float4 a, float s) => new float4((float)(a.x + s), (float)(a.y + s), (float)(a.z + s), (float)(a.w + s));
        public static float4 operator +(float s, float4 b) => new float4((float)(s + b.x), (float)(s + b.y), (float)(s + b.z), (float)(s + b.w));
        public static float4 operator -(float4 a, float4 b) => new float4((float)(a.x - b.x), (float)(a.y - b.y), (float)(a.z - b.z), (float)(a.w - b.w));
        public static float4 operator -(float4 a, float s) => new float4((float)(a.x - s), (float)(a.y - s), (float)(a.z - s), (float)(a.w - s));
        public static float4 operator -(float s, float4 b) => new float4((float)(s - b.x), (float)(s - b.y), (float)(s - b.z), (float)(s - b.w));
        public static float4 operator *(float4 a, float4 b) => new float4((float)(a.x * b.x), (float)(a.y * b.y), (float)(a.z * b.z), (float)(a.w * b.w));
        public static float4 operator *(float4 a, float s) => new float4((float)(a.x * s), (float)(a.y * s), (float)(a.z * s), (float)(a.w * s));
        public static float4 operator *(float s, float4 b) => new float4((float)(s * b.x), (float)(s * b.y), (float)(s * b.z), (float)(s * b.w));
        public static float4 operator /(float4 a, float4 b) => new float4((float)(a.x / b.x), (float)(a.y / b.y), (float)(a.z / b.z), (float)(a.w / b.w));
        public static float4 operator /(float4 a, float s) => new float4((float)(a.x / s), (float)(a.y / s), (float)(a.z / s), (float)(a.w / s));
        public static float4 operator /(float s, float4 b) => new float4((float)(s / b.x), (float)(s / b.y), (float)(s / b.z), (float)(s / b.w));
        public static float4 operator -(float4 a) => new float4((float)(-a.x), (float)(-a.y), (float)(-a.z), (float)(-a.w));
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct int2
    {
        public int x, y;

        public int2(int s) { x = s; y = s; }
        public int2(int x_, int y_) { x = x_; y = y_; }

        public static int2 operator +(int2 a, int2 b) => new int2((int)(a.x + b.x), (int)(a.y + b.y));
        public static int2 operator +(int2 a, int s) => new int2((int)(a.x + s), (int)(a.y + s));
        public static int2 operator +(int s, int2 b) => new int2((int)(s + b.x), (int)(s + b.y));
        public static int2 operator -(int2 a, int2 b) => new int2((int)(a.x - b.x), (int)(a.y - b.y));
        public static int2 operator -(int2 a, int s) => new int2((int)(a.x - s), (int)(a.y - s));
        public static int2 operator -(int s, int2 b) => new int2((int)(s - b.x), (int)(s - b.y));
        public static int2 operator *(int2 a, int2 b) => new int2((int)(a.x * b.x), (int)(a.y * b.y));
        public static int2 operator *(int2 a, int s) => new int2((int)(a.x * s), (int)(a.y * s));
        public static int2 operator *(int s, int2 b) => new int2((int)(s * b.x), (int)(s * b.y));
        public static int2 operator /(int2 a, int2 b) => new int2((int)(a.x / b.x), (int)(a.y / b.y));
        public static int2 operator /(int2 a, int s) => new int2((int)(a.x / s), (int)(a.y / s));
        public static int2 operator /(int s, int2 b) => new int2((int)(s / b.x), (int)(s / b.y));
        public static int2 operator %(int2 a, int2 b) => new int2((int)(a.x % b.x), (int)(a.y % b.y));
        public static int2 operator %(int2 a, int s) => new int2((int)(a.x % s), (int)(a.y % s));
        public static int2 operator %(int s, int2 b) => new int2((int)(s % b.x), (int)(s % b.y));
        public static int2 operator &(int2 a, int2 b) => new int2((int)(a.x & b.x), (int)(a.y & b.y));
        public static int2 operator &(int2 a, int s) => new int2((int)(a.x & s), (int)(a.y & s));
        public static int2 operator &(int s, int2 b) => new int2((int)(s & b.x), (int)(s & b.y));
        public static int2 operator |(int2 a, int2 b) => new int2((int)(a.x | b.x), (int)(a.y | b.y));
        public static int2 operator |(int2 a, int s) => new int2((int)(a.x | s), (int)(a.y | s));
        public static int2 operator |(int s, int2 b) => new int2((int)(s | b.x), (int)(s | b.y));
        public static int2 operator ^(int2 a, int2 b) => new int2((int)(a.x ^ b.x), (int)(a.y ^ b.y));
        public static int2 operator ^(int2 a, int s) => new int2((int)(a.x ^ s), (int)(a.y ^ s));
        public static int2 operator ^(int s, int2 b) => new int2((int)(s ^ b.x), (int)(s ^ b.y));
        public static int2 operator -(int2 a) => new int2((int)(-a.x), (int)(-a.y));
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct int3
    {
        public int x, y, z;

        public int3(int s) { x = s; y = s; z = s; }
        public int3(int x_, int y_, int z_) { x = x_; y = y_; z = z_; }
        public int3(int2 xy, int z_) { x = xy.x; y = xy.y; z = z_; }
        public int3(int x_, int2 yz) { x = x_; y = yz.x; z = yz.y; }

        public static int3 operator +(int3 a, int3 b) => new int3((int)(a.x + b.x), (int)(a.y + b.y), (int)(a.z + b.z));
        public static int3 operator +(int3 a, int s) => new int3((int)(a.x + s), (int)(a.y + s), (int)(a.z + s));
        public static int3 operator +(int s, int3 b) => new int3((int)(s + b.x), (int)(s + b.y), (int)(s + b.z));
        public static int3 operator -(int3 a, int3 b) => new int3((int)(a.x - b.x), (int)(a.y - b.y), (int)(a.z - b.z));
        public static int3 operator -(int3 a, int s) => new int3((int)(a.x - s), (int)(a.y - s), (int)(a.z - s));
        public static int3 operator -(int s, int3 b) => new int3((int)(s - b.x), (int)(s - b.y), (int)(s - b.z));
        public static int3 operator *(int3 a, int3 b) => new int3((int)(a.x * b.x), (int)(a.y * b.y), (int)(a.z * b.z));
        public static int3 operator *(int3 a, int s) => new int3((int)(a.x * s), (int)(a.y * s), (int)(a.z * s));
        public static int3 operator *(int s, int3 b) => new int3((int)(s * b.x), (int)(s * b.y), (int)(s * b.z));
        public static int3 operator /(int3 a, int3 b) => new int3((int)(a.x / b.x), (int)(a.y / b.y), (int)(a.z / b.z));
        public static int3 operator /(int3 a, int s) => new int3((int)(a.x / s), (int)(a.y / s), (int)(a.z / s));
        public static int3 operator /(int s, int3 b) => new int3((int)(s / b.x), (int)(s / b.y), (int)(s / b.z));
        public static int3 operator %(int3 a, int3 b) => new int3((int)(a.x % b.x), (int)(a.y % b.y), (int)(a.z % b.z));
        public static int3 operator %(int3 a, int s) => new int3((int)(a.x % s), (int)(a.y % s), (int)(a.z % s));
        public static int3 operator %(int s, int3 b) => new int3((int)(s % b.x), (int)(s % b.y), (int)(s % b.z));
        public static int3 operator &(int3 a, int3 b) => new int3((int)(a.x & b.x), (int)(a.y & b.y), (int)(a.z & b.z));
        public static int3 operator &(int3 a, int s) => new int3((int)(a.x & s), (int)(a.y & s), (int)(a.z & s));
        public static int3 operator &(int s, int3 b) => new int3((int)(s & b.x), (int)(s & b.y), (int)(s & b.z));
        public static int3 operator |(int3 a, int3 b) => new int3((int)(a.x | b.x), (int)(a.y | b.y), (int)(a.z | b.z));
        public static int3 operator |(int3 a, int s) => new int3((int)(a.x | s), (int)(a.y | s), (int)(a.z | s));
        public static int3 operator |(int s, int3 b) => new int3((int)(s | b.x), (int)(s | b.y), (int)(s | b.z));
        public static int3 operator ^(int3 a, int3 b) => new int3((int)(a.x ^ b.x), (int)(a.y ^ b.y), (int)(a.z ^ b.z));
        public static int3 operator ^(int3 a, int s) => new int3((int)(a.x ^ s), (int)(a.y ^ s), (int)(a.z ^ s));
        public static int3 operator ^(int s, int3 b) => new int3((int)(s ^ b.x), (int)(s ^ b.y), (int)(s ^ b.z));
        public static int3 operator -(int3 a) => new int3((int)(-a.x), (int)(-a.y), (int)(-a.z));
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct int4
    {
        public int x, y, z, w;

        public int4(int s) { x = s; y = s; z = s; w = s; }
        public int4(int x_, int y_, int z_, int w_) { x = x_; y = y_; z = z_; w = w_; }
        public int4(int3 xyz, int w_) { x = xyz.x; y = xyz.y; z = xyz.z; w = w_; }
        public int4(int x_, int3 yzw) { x = x_; y = yzw.x; z = yzw.y; w = yzw.z; }
        public int4(int2 xy, int2 zw) { x = xy.x; y = xy.y; z = zw.x; w = zw.y; }

        public static int4 operator +(int4 a, int4 b) => new int4((int)(a.x + b.x), (int)(a.y + b.y), (int)(a.z + b.z), (int)(a.w + b.w));
        public static int4 operator +(int4 a, int s) => new int4((int)(a.x + s), (int)(a.y + s), (int)(a.z + s), (int)(a.w + s));
        public static int4 operator +(int s, int4 b) => new int4((int)(s + b.x), (int)(s + b.y), (int)(s + b.z), (int)(s + b.w));
        public static int4 operator -(int4 a, int4 b) => new int4((int)(a.x - b.x), (int)(a.y - b.y), (int)(a.z - b.z), (int)(a.w - b.w));
        public static int4 operator -(int4 a, int s) => new int4((int)(a.x - s), (int)(a.y - s), (int)(a.z - s), (int)(a.w - s));
        public static int4 operator -(int s, int4 b) => new int4((int)(s - b.x), (int)(s - b.y), (int)(s - b.z), (int)(s - b.w));
        public static int4 operator *(int4 a, int4 b) => new int4((int)(a.x * b.x), (int)(a.y * b.y), (int)(a.z * b.z), (int)(a.w * b.w));
        public static int4 operator *(int4 a, int s) => new int4((int)(a.x * s), (int)(a.y * s), (int)(a.z * s), (int)(a.w * s));
        public static int4 operator *(int s, int4 b) => new int4((int)(s * b.x), (int)(s * b.y), (int)(s * b.z), (int)(s * b.w));
        public static int4 operator /(int4 a, int4 b) => new int4((int)(a.x / b.x), (int)(a.y / b.y), (int)(a.z / b.z), (int)(a.w / b.w));
        public static int4 operator /(int4 a, int s) => new int4((int)(a.x / s), (int)(a.y / s), (int)(a.z / s), (int)(a.w / s));
        public static int4 operator /(int s, int4 b) => new int4((int)(s / b.x), (int)(s / b.y), (int)(s / b.z), (int)(s / b.w));
        public static int4 operator %(int4 a, int4 b) => new int4((int)(a.x % b.x), (int)(a.y % b.y), (int)(a.z % b.z), (int)(a.w % b.w));
        public static int4 operator %(int4 a, int s) => new int4((int)(a.x % s), (int)(a.y % s), (int)(a.z % s), (int)(a.w % s));
        public static int4 operator %(int s, int4 b) => new int4((int)(s % b.x), (int)(s % b.y), (int)(s % b.z), (int)(s % b.w));
        public static int4 operator &(int4 a, int4 b) => new int4((int)(a.x & b.x), (int)(a.y & b.y), (int)(a.z & b.z), (int)(a.w & b.w));
        public static int4 operator &(int4 a, int s) => new int4((int)(a.x & s), (int)(a.y & s), (int)(a.z & s), (int)(a.w & s));
        public static int4 operator &(int s, int4 b) => new int4((int)(s & b.x), (int)(s & b.y), (int)(s & b.z), (int)(s & b.w));
        public static int4 operator |(int4 a, int4 b) => new int4((int)(a.x | b.x), (int)(a.y | b.y), (int)(a.z | b.z), (int)(a.w | b.w));
        public static int4 operator |(int4 a, int s) => new int4((int)(a.x | s), (int)(a.y | s), (int)(a.z | s), (int)(a.w | s));
        public static int4 operator |(int s, int4 b) => new int4((int)(s | b.x), (int)(s | b.y), (int)(s | b.z), (int)(s | b.w));
        public static int4 operator ^(int4 a, int4 b) => new int4((int)(a.x ^ b.x), (int)(a.y ^ b.y), (int)(a.z ^ b.z), (int)(a.w ^ b.w));
        public static int4 operator ^(int4 a, int s) => new int4((int)(a.x ^ s), (int)(a.y ^ s), (int)(a.z ^ s), (int)(a.w ^ s));
        public static int4 operator ^(int s, int4 b) => new int4((int)(s ^ b.x), (int)(s ^ b.y), (int)(s ^ b.z), (int)(s ^ b.w));
        public static int4 operator -(int4 a) => new int4((int)(-a.x), (int)(-a.y), (int)(-a.z), (int)(-a.w));
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct uint2
    {
        public uint x, y;

        public uint2(uint s) { x = s; y = s; }
        public uint2(uint x_, uint y_) { x = x_; y = y_; }

        public static uint2 operator +(uint2 a, uint2 b) => new uint2((uint)(a.x + b.x), (uint)(a.y + b.y));
        public static uint2 operator +(uint2 a, uint s) => new uint2((uint)(a.x + s), (uint)(a.y + s));
        public static uint2 operator +(uint s, uint2 b) => new uint2((uint)(s + b.x), (uint)(s + b.y));
        public static uint2 operator -(uint2 a, uint2 b) => new uint2((uint)(a.x - b.x), (uint)(a.y - b.y));
        public static uint2 operator -(uint2 a, uint s) => new uint2((uint)(a.x - s), (uint)(a.y - s));
        public static uint2 operator -(uint s, uint2 b) => new uint2((uint)(s - b.x), (uint)(s - b.y));
        public static uint2 operator *(uint2 a, uint2 b) => new uint2((uint)(a.x * b.x), (uint)(a.y * b.y));
        public static uint2 operator *(uint2 a, uint s) => new uint2((uint)(a.x * s), (uint)(a.y * s));
        public static uint2 operator *(uint s, uint2 b) => new uint2((uint)(s * b.x), (uint)(s * b.y));
        public static uint2 operator /(uint2 a, uint2 b) => new uint2((uint)(a.x / b.x), (uint)(a.y / b.y));
        public static uint2 operator /(uint2 a, uint s) => new uint2((uint)(a.x / s), (uint)(a.y / s));
        public static uint2 operator /(uint s, uint2 b) => new uint2((uint)(s / b.x), (uint)(s / b.y));
        public static uint2 operator %(uint2 a, uint2 b) => new uint2((uint)(a.x % b.x), (uint)(a.y % b.y));
        public static uint2 operator %(uint2 a, uint s) => new uint2((uint)(a.x % s), (uint)(a.y % s));
        public static uint2 operator %(uint s, uint2 b) => new uint2((uint)(s % b.x), (uint)(s % b.y));
        public static uint2 operator &(uint2 a, uint2 b) => new uint2((uint)(a.x & b.x), (uint)(a.y & b.y));
        public static uint2 operator &(uint2 a, uint s) => new uint2((uint)(a.x & s), (uint)(a.y & s));
        public static uint2 operator &(uint s, uint2 b) => new uint2((uint)(s & b.x), (uint)(s & b.y));
        public static uint2 operator |(uint2 a, uint2 b) => new uint2((uint)(a.x | b.x), (uint)(a.y | b.y));
        public static uint2 operator |(uint2 a, uint s) => new uint2((uint)(a.x | s), (uint)(a.y | s));
        public static uint2 operator |(uint s, uint2 b) => new uint2((uint)(s | b.x), (uint)(s | b.y));
        public static uint2 operator ^(uint2 a, uint2 b) => new uint2((uint)(a.x ^ b.x), (uint)(a.y ^ b.y));
        public static uint2 operator ^(uint2 a, uint s) => new uint2((uint)(a.x ^ s), (uint)(a.y ^ s));
        public static uint2 operator ^(uint s, uint2 b) => new uint2((uint)(s ^ b.x), (uint)(s ^ b.y));
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct uint3
    {
        public uint x, y, z;

        public uint3(uint s) { x = s; y = s; z = s; }
        public uint3(uint x_, uint y_, uint z_) { x = x_; y = y_; z = z_; }
        public uint3(uint2 xy, uint z_) { x = xy.x; y = xy.y; z = z_; }
        public uint3(uint x_, uint2 yz) { x = x_; y = yz.x; z = yz.y; }

        public static uint3 operator +(uint3 a, uint3 b) => new uint3((uint)(a.x + b.x), (uint)(a.y + b.y), (uint)(a.z + b.z));
        public static uint3 operator +(uint3 a, uint s) => new uint3((uint)(a.x + s), (uint)(a.y + s), (uint)(a.z + s));
        public static uint3 operator +(uint s, uint3 b) => new uint3((uint)(s + b.x), (uint)(s + b.y), (uint)(s + b.z));
        public static uint3 operator -(uint3 a, uint3 b) => new uint3((uint)(a.x - b.x), (uint)(a.y - b.y), (uint)(a.z - b.z));
        public static uint3 operator -(uint3 a, uint s) => new uint3((uint)(a.x - s), (uint)(a.y - s), (uint)(a.z - s));
        public static uint3 operator -(uint s, uint3 b) => new uint3((uint)(s - b.x), (uint)(s - b.y), (uint)(s - b.z));
        public static uint3 operator *(uint3 a, uint3 b) => new uint3((uint)(a.x * b.x), (uint)(a.y * b.y), (uint)(a.z * b.z));
        public static uint3 operator *(uint3 a, uint s) => new uint3((uint)(a.x * s), (uint)(a.y * s), (uint)(a.z * s));
        public static uint3 operator *(uint s, uint3 b) => new uint3((uint)(s * b.x), (uint)(s * b.y), (uint)(s * b.z));
        public static uint3 operator /(uint3 a, uint3 b) => new uint3((uint)(a.x / b.x), (uint)(a.y / b.y), (uint)(a.z / b.z));
        public static uint3 operator /(uint3 a, uint s) => new uint3((uint)(a.x / s), (uint)(a.y / s), (uint)(a.z / s));
        public static uint3 operator /(uint s, uint3 b) => new uint3((uint)(s / b.x), (uint)(s / b.y), (uint)(s / b.z));
        public static uint3 operator %(uint3 a, uint3 b) => new uint3((uint)(a.x % b.x), (uint)(a.y % b.y), (uint)(a.z % b.z));
        public static uint3 operator %(uint3 a, uint s) => new uint3((uint)(a.x % s), (uint)(a.y % s), (uint)(a.z % s));
        public static uint3 operator %(uint s, uint3 b) => new uint3((uint)(s % b.x), (uint)(s % b.y), (uint)(s % b.z));
        public static uint3 operator &(uint3 a, uint3 b) => new uint3((uint)(a.x & b.x), (uint)(a.y & b.y), (uint)(a.z & b.z));
        public static uint3 operator &(uint3 a, uint s) => new uint3((uint)(a.x & s), (uint)(a.y & s), (uint)(a.z & s));
        public static uint3 operator &(uint s, uint3 b) => new uint3((uint)(s & b.x), (uint)(s & b.y), (uint)(s & b.z));
        public static uint3 operator |(uint3 a, uint3 b) => new uint3((uint)(a.x | b.x), (uint)(a.y | b.y), (uint)(a.z | b.z));
        public static uint3 operator |(uint3 a, uint s) => new uint3((uint)(a.x | s), (uint)(a.y | s), (uint)(a.z | s));
        public static uint3 operator |(uint s, uint3 b) => new uint3((uint)(s | b.x), (uint)(s | b.y), (uint)(s | b.z));
        public static uint3 operator ^(uint3 a, uint3 b) => new uint3((uint)(a.x ^ b.x), (uint)(a.y ^ b.y), (uint)(a.z ^ b.z));
        public static uint3 operator ^(uint3 a, uint s) => new uint3((uint)(a.x ^ s), (uint)(a.y ^ s), (uint)(a.z ^ s));
        public static uint3 operator ^(uint s, uint3 b) => new uint3((uint)(s ^ b.x), (uint)(s ^ b.y), (uint)(s ^ b.z));
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct uint4
    {
        public uint x, y, z, w;

        public uint4(uint s) { x = s; y = s; z = s; w = s; }
        public uint4(uint x_, uint y_, uint z_, uint w_) { x = x_; y = y_; z = z_; w = w_; }
        public uint4(uint3 xyz, uint w_) { x = xyz.x; y = xyz.y; z = xyz.z; w = w_; }
        public uint4(uint x_, uint3 yzw) { x = x_; y = yzw.x; z = yzw.y; w = yzw.z; }
        public uint4(uint2 xy, uint2 zw) { x = xy.x; y = xy.y; z = zw.x; w = zw.y; }

        public static uint4 operator +(uint4 a, uint4 b) => new uint4((uint)(a.x + b.x), (uint)(a.y + b.y), (uint)(a.z + b.z), (uint)(a.w + b.w));
        public static uint4 operator +(uint4 a, uint s) => new uint4((uint)(a.x + s), (uint)(a.y + s), (uint)(a.z + s), (uint)(a.w + s));
        public static uint4 operator +(uint s, uint4 b) => new uint4((uint)(s + b.x), (uint)(s + b.y), (uint)(s + b.z), (uint)(s + b.w));
        public static uint4 operator -(uint4 a, uint4 b) => new uint4((uint)(a.x - b.x), (uint)(a.y - b.y), (uint)(a.z - b.z), (uint)(a.w - b.w));
        public static uint4 operator -(uint4 a, uint s) => new uint4((uint)(a.x - s), (uint)(a.y - s), (uint)(a.z - s), (uint)(a.w - s));
        public static uint4 operator -(uint s, uint4 b) => new uint4((uint)(s - b.x), (uint)(s - b.y), (uint)(s - b.z), (uint)(s - b.w));
        public static uint4 operator *(uint4 a, uint4 b) => new uint4((uint)(a.x * b.x), (uint)(a.y * b.y), (uint)(a.z * b.z), (uint)(a.w * b.w));
        public static uint4 operator *(uint4 a, uint s) => new uint4((uint)(a.x * s), (uint)(a.y * s), (uint)(a.z * s), (uint)(a.w * s));
        public static uint4 operator *(uint s, uint4 b) => new uint4((uint)(s * b.x), (uint)(s * b.y), (uint)(s * b.z), (uint)(s * b.w));
        public static uint4 operator /(uint4 a, uint4 b) => new uint4((uint)(a.x / b.x), (uint)(a.y / b.y), (uint)(a.z / b.z), (uint)(a.w / b.w));
        public static uint4 operator /(uint4 a, uint s) => new uint4((uint)(a.x / s), (uint)(a.y / s), (uint)(a.z / s), (uint)(a.w / s));
        public static uint4 operator /(uint s, uint4 b) => new uint4((uint)(s / b.x), (uint)(s / b.y), (uint)(s / b.z), (uint)(s / b.w));
        public static uint4 operator %(uint4 a, uint4 b) => new uint4((uint)(a.x % b.x), (uint)(a.y % b.y), (uint)(a.z % b.z), (uint)(a.w % b.w));
        public static uint4 operator %(uint4 a, uint s) => new uint4((uint)(a.x % s), (uint)(a.y % s), (uint)(a.z % s), (uint)(a.w % s));
        public static uint4 operator %(uint s, uint4 b) => new uint4((uint)(s % b.x), (uint)(s % b.y), (uint)(s % b.z), (uint)(s % b.w));
        public static uint4 operator &(uint4 a, uint4 b) => new uint4((uint)(a.x & b.x), (uint)(a.y & b.y), (uint)(a.z & b.z), (uint)(a.w & b.w));
        public static uint4 operator &(uint4 a, uint s) => new uint4((uint)(a.x & s), (uint)(a.y & s), (uint)(a.z & s), (uint)(a.w & s));
        public static uint4 operator &(uint s, uint4 b) => new uint4((uint)(s & b.x), (uint)(s & b.y), (uint)(s & b.z), (uint)(s & b.w));
        public static uint4 operator |(uint4 a, uint4 b) => new uint4((uint)(a.x | b.x), (uint)(a.y | b.y), (uint)(a.z | b.z), (uint)(a.w | b.w));
        public static uint4 operator |(uint4 a, uint s) => new uint4((uint)(a.x | s), (uint)(a.y | s), (uint)(a.z | s), (uint)(a.w | s));
        public static uint4 operator |(uint s, uint4 b) => new uint4((uint)(s | b.x), (uint)(s | b.y), (uint)(s | b.z), (uint)(s | b.w));
        public static uint4 operator ^(uint4 a, uint4 b) => new uint4((uint)(a.x ^ b.x), (uint)(a.y ^ b.y), (uint)(a.z ^ b.z), (uint)(a.w ^ b.w));
        public static uint4 operator ^(uint4 a, uint s) => new uint4((uint)(a.x ^ s), (uint)(a.y ^ s), (uint)(a.z ^ s), (uint)(a.w ^ s));
        public static uint4 operator ^(uint s, uint4 b) => new uint4((uint)(s ^ b.x), (uint)(s ^ b.y), (uint)(s ^ b.z), (uint)(s ^ b.w));
    }

    // エンジンが生成コードの Range へ渡すバッファ 1 本 (EngineAPI.h の MyeKernelBuffer と同一レイアウト)
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct KernelBuffer
    {
        public void* Data;
        public uint Count;
        public uint Stride;
    }

    // 要素番号。RW の添字にできるのはこの型だけ (C++ 版の KernelId と同じ契約)
    public readonly struct KernelId
    {
        public readonly uint Value;
        public KernelId(uint value) { Value = value; }
    }

    // RWStructuredBuffer<T>。自分の要素 (KernelId) だけを書く約束 (ADR-028)
    public unsafe readonly struct RWView<T> where T : unmanaged
    {
        private readonly T* _data;
        private readonly uint _count;
        public RWView(T* data, uint count) { _data = data; _count = count; }
        public uint Count => _count;
        public ref T this[KernelId id] => ref _data[id.Value];
    }

    // StructuredBuffer<T>。範囲外の読み取りは 0 初期化の T (D3D11 と同じ)
    public unsafe readonly struct ROView<T> where T : unmanaged
    {
        private readonly T* _data;
        private readonly uint _count;
        public ROView(T* data, uint count) { _data = data; _count = count; }
        public uint Count => _count;
        public T this[uint i] => i < _count ? _data[i] : default;
    }

    // 組込み関数 (spec 4.1.2 のホワイトリスト)。生成コードは using static で取り込む
    public static class KernelMath
    {
        private const float kPi = 3.14159265358979f;
        private const float kTwoPi = kPi * 2.0f;
        private const float kInvTwoPi = 1.0f / kTwoPi;

        // ---- スカラー ----
        public static float min(float a, float b) => b < a ? b : a;
        public static float max(float a, float b) => a < b ? b : a;
        public static float clamp(float x, float lo, float hi) => min(max(x, lo), hi);
        public static int min(int a, int b) => b < a ? b : a;
        public static int max(int a, int b) => a < b ? b : a;
        public static int clamp(int x, int lo, int hi) => min(max(x, lo), hi);
        public static uint min(uint a, uint b) => b < a ? b : a;
        public static uint max(uint a, uint b) => a < b ? b : a;
        public static uint clamp(uint x, uint lo, uint hi) => min(max(x, lo), hi);
        // 符号ビットを落とす (-0.0f も +0.0f になる)
        public static float abs(float x) => BitConverter.UInt32BitsToSingle(BitConverter.SingleToUInt32Bits(x) & 0x7fffffffu);
        public static int abs(int x) => x < 0 ? -x : x;
        public static float floor(float x) => MathF.Floor(x);
        public static float ceil(float x) => MathF.Ceiling(x);
        public static float frac(float x) => x - MathF.Floor(x);
        public static float sqrt(float x) => MathF.Sqrt(x);
        // GPU では近似、CPU では厳密な除算 (spec 4.1.2)
        public static float rsqrt(float x) => 1.0f / MathF.Sqrt(x);
        public static float rcp(float x) => 1.0f / x;
        public static float saturate(float x) => min(max(x, 0.0f), 1.0f);
        public static float lerp(float a, float b, float t) => a + (b - a) * t;
        public static float step(float edge, float x) => x >= edge ? 1.0f : 0.0f;
        public static float smoothstep(float lo, float hi, float x)
        {
            float t = saturate((x - lo) / (hi - lo));
            return t * t * (3.0f - 2.0f * t);
        }
        public static int sign(float x) => x > 0.0f ? 1 : (x < 0.0f ? -1 : 0);
        public static int sign(int x) => x > 0 ? 1 : (x < 0 ? -1 : 0);

        // Shared/ScriptAPI.h の MyeSinRad (|x| <= 3pi/2 が前提) の移植。式を 1 命令も変えない
        private static float SinRad(float x)
        {
            if (x > kPi * 0.5f)
            {
                x = kPi - x;
            }
            else if (x < -kPi * 0.5f)
            {
                x = -kPi - x;
            }
            float x2 = x * x;
            return x
                * (1.0f
                   + x2
                       * (-1.0f / 6.0f
                          + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f)))));
        }
        private static float CosRad(float x) => SinRad(x + kPi * 0.5f);

        // 範囲外のときだけ x - 2pi * floor(x / 2pi + 0.5) で [-pi, pi] へ寄せる
        public static float sin(float x)
        {
            if (x > kPi * 1.5f || x < -kPi * 1.5f)
            {
                x = x - kTwoPi * MathF.Floor(x * kInvTwoPi + 0.5f);
            }
            return SinRad(x);
        }
        public static float cos(float x)
        {
            if (x > kPi || x < -kPi)
            {
                x = x - kTwoPi * MathF.Floor(x * kInvTwoPi + 0.5f);
            }
            return CosRad(x);
        }

        // float -> int / uint。Shared/KernelCompat.h の FloatToInt / FloatToUint と同じ式 (NaN は 0、範囲外は飽和)
        public static int FloatToInt(float x)
        {
            if (float.IsNaN(x))
            {
                return 0;
            }
            if (x < -2147483648.0f)
            {
                x = -2147483648.0f;
            }
            if (x > 2147483520.0f)
            {
                x = 2147483520.0f;
            }
            return (int)x;
        }
        public static uint FloatToUint(float x)
        {
            if (!(x > 0.0f))
            {
                return 0u;
            }
            if (x > 4294967040.0f)
            {
                x = 4294967040.0f;
            }
            return (uint)x;
        }

        // ---- 乱数代わりの整数ハッシュ (PCG の出力関数) ----
        public static uint HashU32(uint v)
        {
            uint state = v * 747796405u + 2891336453u;
            uint word = ((state >> (int)((state >> 28) + 4u)) ^ state) * 277803737u;
            return (word >> 22) ^ word;
        }
        public static float HashToUnitFloat(uint h) => (float)(h >> 8) * (1.0f / 16777216.0f);

        // ---- ベクトル (成分ごと) ----
        public static float2 abs(float2 a) => new float2(abs(a.x), abs(a.y));
        public static float2 floor(float2 a) => new float2(floor(a.x), floor(a.y));
        public static float2 ceil(float2 a) => new float2(ceil(a.x), ceil(a.y));
        public static float2 frac(float2 a) => new float2(frac(a.x), frac(a.y));
        public static float2 sqrt(float2 a) => new float2(sqrt(a.x), sqrt(a.y));
        public static float2 rsqrt(float2 a) => new float2(rsqrt(a.x), rsqrt(a.y));
        public static float2 rcp(float2 a) => new float2(rcp(a.x), rcp(a.y));
        public static float2 saturate(float2 a) => new float2(saturate(a.x), saturate(a.y));
        public static float2 sin(float2 a) => new float2(sin(a.x), sin(a.y));
        public static float2 cos(float2 a) => new float2(cos(a.x), cos(a.y));
        public static int2 sign(float2 a) => new int2(sign(a.x), sign(a.y));
        public static int2 abs(int2 a) => new int2(abs(a.x), abs(a.y));
        public static int2 sign(int2 a) => new int2(sign(a.x), sign(a.y));
        public static float2 min(float2 a, float2 b) => new float2(min(a.x, b.x), min(a.y, b.y));
        public static float2 min(float2 a, float s) => min(a, new float2(s));
        public static float2 max(float2 a, float2 b) => new float2(max(a.x, b.x), max(a.y, b.y));
        public static float2 max(float2 a, float s) => max(a, new float2(s));
        public static float2 clamp(float2 x, float2 lo, float2 hi) => min(max(x, lo), hi);
        public static float2 clamp(float2 x, float lo, float hi) => min(max(x, new float2(lo)), new float2(hi));
        public static int2 min(int2 a, int2 b) => new int2(min(a.x, b.x), min(a.y, b.y));
        public static int2 min(int2 a, int s) => min(a, new int2(s));
        public static int2 max(int2 a, int2 b) => new int2(max(a.x, b.x), max(a.y, b.y));
        public static int2 max(int2 a, int s) => max(a, new int2(s));
        public static int2 clamp(int2 x, int2 lo, int2 hi) => min(max(x, lo), hi);
        public static int2 clamp(int2 x, int lo, int hi) => min(max(x, new int2(lo)), new int2(hi));
        public static uint2 min(uint2 a, uint2 b) => new uint2(min(a.x, b.x), min(a.y, b.y));
        public static uint2 min(uint2 a, uint s) => min(a, new uint2(s));
        public static uint2 max(uint2 a, uint2 b) => new uint2(max(a.x, b.x), max(a.y, b.y));
        public static uint2 max(uint2 a, uint s) => max(a, new uint2(s));
        public static uint2 clamp(uint2 x, uint2 lo, uint2 hi) => min(max(x, lo), hi);
        public static uint2 clamp(uint2 x, uint lo, uint hi) => min(max(x, new uint2(lo)), new uint2(hi));
        public static float2 lerp(float2 a, float2 b, float2 t) => a + (b - a) * t;
        public static float2 lerp(float2 a, float2 b, float t) => a + (b - a) * t;
        public static float2 step(float2 edge, float2 x) => new float2(step(edge.x, x.x), step(edge.y, x.y));
        public static float2 step(float edge, float2 x) => step(new float2(edge), x);
        public static float2 smoothstep(float2 lo, float2 hi, float2 x) => new float2(smoothstep(lo.x, hi.x, x.x), smoothstep(lo.y, hi.y, x.y));
        public static float2 smoothstep(float lo, float hi, float2 x) => smoothstep(new float2(lo), new float2(hi), x);
        public static float dot(float2 a, float2 b) => a.x * b.x + a.y * b.y;
        public static float length(float2 v) => sqrt(dot(v, v));
        public static float distance(float2 a, float2 b) => length(a - b);
        public static float2 normalize(float2 v) => v * rsqrt(dot(v, v));
        public static float3 abs(float3 a) => new float3(abs(a.x), abs(a.y), abs(a.z));
        public static float3 floor(float3 a) => new float3(floor(a.x), floor(a.y), floor(a.z));
        public static float3 ceil(float3 a) => new float3(ceil(a.x), ceil(a.y), ceil(a.z));
        public static float3 frac(float3 a) => new float3(frac(a.x), frac(a.y), frac(a.z));
        public static float3 sqrt(float3 a) => new float3(sqrt(a.x), sqrt(a.y), sqrt(a.z));
        public static float3 rsqrt(float3 a) => new float3(rsqrt(a.x), rsqrt(a.y), rsqrt(a.z));
        public static float3 rcp(float3 a) => new float3(rcp(a.x), rcp(a.y), rcp(a.z));
        public static float3 saturate(float3 a) => new float3(saturate(a.x), saturate(a.y), saturate(a.z));
        public static float3 sin(float3 a) => new float3(sin(a.x), sin(a.y), sin(a.z));
        public static float3 cos(float3 a) => new float3(cos(a.x), cos(a.y), cos(a.z));
        public static int3 sign(float3 a) => new int3(sign(a.x), sign(a.y), sign(a.z));
        public static int3 abs(int3 a) => new int3(abs(a.x), abs(a.y), abs(a.z));
        public static int3 sign(int3 a) => new int3(sign(a.x), sign(a.y), sign(a.z));
        public static float3 min(float3 a, float3 b) => new float3(min(a.x, b.x), min(a.y, b.y), min(a.z, b.z));
        public static float3 min(float3 a, float s) => min(a, new float3(s));
        public static float3 max(float3 a, float3 b) => new float3(max(a.x, b.x), max(a.y, b.y), max(a.z, b.z));
        public static float3 max(float3 a, float s) => max(a, new float3(s));
        public static float3 clamp(float3 x, float3 lo, float3 hi) => min(max(x, lo), hi);
        public static float3 clamp(float3 x, float lo, float hi) => min(max(x, new float3(lo)), new float3(hi));
        public static int3 min(int3 a, int3 b) => new int3(min(a.x, b.x), min(a.y, b.y), min(a.z, b.z));
        public static int3 min(int3 a, int s) => min(a, new int3(s));
        public static int3 max(int3 a, int3 b) => new int3(max(a.x, b.x), max(a.y, b.y), max(a.z, b.z));
        public static int3 max(int3 a, int s) => max(a, new int3(s));
        public static int3 clamp(int3 x, int3 lo, int3 hi) => min(max(x, lo), hi);
        public static int3 clamp(int3 x, int lo, int hi) => min(max(x, new int3(lo)), new int3(hi));
        public static uint3 min(uint3 a, uint3 b) => new uint3(min(a.x, b.x), min(a.y, b.y), min(a.z, b.z));
        public static uint3 min(uint3 a, uint s) => min(a, new uint3(s));
        public static uint3 max(uint3 a, uint3 b) => new uint3(max(a.x, b.x), max(a.y, b.y), max(a.z, b.z));
        public static uint3 max(uint3 a, uint s) => max(a, new uint3(s));
        public static uint3 clamp(uint3 x, uint3 lo, uint3 hi) => min(max(x, lo), hi);
        public static uint3 clamp(uint3 x, uint lo, uint hi) => min(max(x, new uint3(lo)), new uint3(hi));
        public static float3 lerp(float3 a, float3 b, float3 t) => a + (b - a) * t;
        public static float3 lerp(float3 a, float3 b, float t) => a + (b - a) * t;
        public static float3 step(float3 edge, float3 x) => new float3(step(edge.x, x.x), step(edge.y, x.y), step(edge.z, x.z));
        public static float3 step(float edge, float3 x) => step(new float3(edge), x);
        public static float3 smoothstep(float3 lo, float3 hi, float3 x) => new float3(smoothstep(lo.x, hi.x, x.x), smoothstep(lo.y, hi.y, x.y), smoothstep(lo.z, hi.z, x.z));
        public static float3 smoothstep(float lo, float hi, float3 x) => smoothstep(new float3(lo), new float3(hi), x);
        public static float dot(float3 a, float3 b) => (a.x * b.x + a.y * b.y) + a.z * b.z;
        public static float length(float3 v) => sqrt(dot(v, v));
        public static float distance(float3 a, float3 b) => length(a - b);
        public static float3 normalize(float3 v) => v * rsqrt(dot(v, v));
        public static float4 abs(float4 a) => new float4(abs(a.x), abs(a.y), abs(a.z), abs(a.w));
        public static float4 floor(float4 a) => new float4(floor(a.x), floor(a.y), floor(a.z), floor(a.w));
        public static float4 ceil(float4 a) => new float4(ceil(a.x), ceil(a.y), ceil(a.z), ceil(a.w));
        public static float4 frac(float4 a) => new float4(frac(a.x), frac(a.y), frac(a.z), frac(a.w));
        public static float4 sqrt(float4 a) => new float4(sqrt(a.x), sqrt(a.y), sqrt(a.z), sqrt(a.w));
        public static float4 rsqrt(float4 a) => new float4(rsqrt(a.x), rsqrt(a.y), rsqrt(a.z), rsqrt(a.w));
        public static float4 rcp(float4 a) => new float4(rcp(a.x), rcp(a.y), rcp(a.z), rcp(a.w));
        public static float4 saturate(float4 a) => new float4(saturate(a.x), saturate(a.y), saturate(a.z), saturate(a.w));
        public static float4 sin(float4 a) => new float4(sin(a.x), sin(a.y), sin(a.z), sin(a.w));
        public static float4 cos(float4 a) => new float4(cos(a.x), cos(a.y), cos(a.z), cos(a.w));
        public static int4 sign(float4 a) => new int4(sign(a.x), sign(a.y), sign(a.z), sign(a.w));
        public static int4 abs(int4 a) => new int4(abs(a.x), abs(a.y), abs(a.z), abs(a.w));
        public static int4 sign(int4 a) => new int4(sign(a.x), sign(a.y), sign(a.z), sign(a.w));
        public static float4 min(float4 a, float4 b) => new float4(min(a.x, b.x), min(a.y, b.y), min(a.z, b.z), min(a.w, b.w));
        public static float4 min(float4 a, float s) => min(a, new float4(s));
        public static float4 max(float4 a, float4 b) => new float4(max(a.x, b.x), max(a.y, b.y), max(a.z, b.z), max(a.w, b.w));
        public static float4 max(float4 a, float s) => max(a, new float4(s));
        public static float4 clamp(float4 x, float4 lo, float4 hi) => min(max(x, lo), hi);
        public static float4 clamp(float4 x, float lo, float hi) => min(max(x, new float4(lo)), new float4(hi));
        public static int4 min(int4 a, int4 b) => new int4(min(a.x, b.x), min(a.y, b.y), min(a.z, b.z), min(a.w, b.w));
        public static int4 min(int4 a, int s) => min(a, new int4(s));
        public static int4 max(int4 a, int4 b) => new int4(max(a.x, b.x), max(a.y, b.y), max(a.z, b.z), max(a.w, b.w));
        public static int4 max(int4 a, int s) => max(a, new int4(s));
        public static int4 clamp(int4 x, int4 lo, int4 hi) => min(max(x, lo), hi);
        public static int4 clamp(int4 x, int lo, int hi) => min(max(x, new int4(lo)), new int4(hi));
        public static uint4 min(uint4 a, uint4 b) => new uint4(min(a.x, b.x), min(a.y, b.y), min(a.z, b.z), min(a.w, b.w));
        public static uint4 min(uint4 a, uint s) => min(a, new uint4(s));
        public static uint4 max(uint4 a, uint4 b) => new uint4(max(a.x, b.x), max(a.y, b.y), max(a.z, b.z), max(a.w, b.w));
        public static uint4 max(uint4 a, uint s) => max(a, new uint4(s));
        public static uint4 clamp(uint4 x, uint4 lo, uint4 hi) => min(max(x, lo), hi);
        public static uint4 clamp(uint4 x, uint lo, uint hi) => min(max(x, new uint4(lo)), new uint4(hi));
        public static float4 lerp(float4 a, float4 b, float4 t) => a + (b - a) * t;
        public static float4 lerp(float4 a, float4 b, float t) => a + (b - a) * t;
        public static float4 step(float4 edge, float4 x) => new float4(step(edge.x, x.x), step(edge.y, x.y), step(edge.z, x.z), step(edge.w, x.w));
        public static float4 step(float edge, float4 x) => step(new float4(edge), x);
        public static float4 smoothstep(float4 lo, float4 hi, float4 x) => new float4(smoothstep(lo.x, hi.x, x.x), smoothstep(lo.y, hi.y, x.y), smoothstep(lo.z, hi.z, x.z), smoothstep(lo.w, hi.w, x.w));
        public static float4 smoothstep(float lo, float hi, float4 x) => smoothstep(new float4(lo), new float4(hi), x);
        public static float dot(float4 a, float4 b) => ((a.x * b.x + a.y * b.y) + a.z * b.z) + a.w * b.w;
        public static float length(float4 v) => sqrt(dot(v, v));
        public static float distance(float4 a, float4 b) => length(a - b);
        public static float4 normalize(float4 v) => v * rsqrt(dot(v, v));
        public static float3 cross(float3 a, float3 b) =>
            new float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
    }
}
