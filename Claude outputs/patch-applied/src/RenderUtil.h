#pragma once
// Small helpers shared by Renderer.cpp, World.cpp and Fx.cpp.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Course.h"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

namespace rutil {

constexpr float PI_F = 3.14159265358979f;

struct MeshBuilder {
    std::vector<float> v, n, t;
    std::vector<unsigned char> c;
    std::vector<unsigned short> idx;
    unsigned short add(Vector3 p, Vector3 nn, Vector2 uv, Color col = WHITE) {
        v.insert(v.end(), {p.x, p.y, p.z});
        n.insert(n.end(), {nn.x, nn.y, nn.z});
        t.insert(t.end(), {uv.x, uv.y});
        c.insert(c.end(), {col.r, col.g, col.b, col.a});
        return (unsigned short)(v.size() / 3 - 1);
    }
    void tri(unsigned short a, unsigned short b, unsigned short d) { idx.insert(idx.end(), {a, b, d}); }
    Mesh build(bool colors = false) const {
        Mesh m{};
        m.vertexCount = int(v.size() / 3);
        m.triangleCount = int(idx.size() / 3);
        m.vertices = (float*)MemAlloc(sizeof(float) * v.size());
        std::memcpy(m.vertices, v.data(), sizeof(float) * v.size());
        m.normals = (float*)MemAlloc(sizeof(float) * n.size());
        std::memcpy(m.normals, n.data(), sizeof(float) * n.size());
        m.texcoords = (float*)MemAlloc(sizeof(float) * t.size());
        std::memcpy(m.texcoords, t.data(), sizeof(float) * t.size());
        if (colors) {
            m.colors = (unsigned char*)MemAlloc(c.size());
            std::memcpy(m.colors, c.data(), c.size());
        }
        m.indices = (unsigned short*)MemAlloc(sizeof(unsigned short) * idx.size());
        std::memcpy(m.indices, idx.data(), sizeof(unsigned short) * idx.size());
        UploadMesh(&m, false);
        return m;
    }
};

inline void setF(Shader s, const char* n, float v) { int l = GetShaderLocation(s, n); if (l >= 0) SetShaderValue(s, l, &v, SHADER_UNIFORM_FLOAT); }
inline void setI(Shader s, const char* n, int v) { int l = GetShaderLocation(s, n); if (l >= 0) SetShaderValue(s, l, &v, SHADER_UNIFORM_INT); }
inline void setV2(Shader s, const char* n, Vector2 v) { int l = GetShaderLocation(s, n); if (l >= 0) SetShaderValue(s, l, &v, SHADER_UNIFORM_VEC2); }
inline void setV3(Shader s, const char* n, Vector3 v) { int l = GetShaderLocation(s, n); if (l >= 0) SetShaderValue(s, l, &v, SHADER_UNIFORM_VEC3); }
inline void setV4(Shader s, const char* n, Vector4 v) { int l = GetShaderLocation(s, n); if (l >= 0) SetShaderValue(s, l, &v, SHADER_UNIFORM_VEC4); }
inline void setM(Shader s, const char* n, Matrix m) { int l = GetShaderLocation(s, n); if (l >= 0) SetShaderValueMatrix(s, l, m); }

inline float smooth(float a, float b, float x) { float t = std::clamp((x - a) / (b - a), 0.f, 1.f); return t * t * (3 - 2 * t); }
inline Vector3 toV(const V3& v) { return {v.x, v.y, v.z}; }
inline Vector3 fwdOf(float h) { return {std::sin(h), 0, std::cos(h)}; }
inline Vector3 rightOf(float h) { return {-std::cos(h), 0, std::sin(h)}; }

// align a +Y unit primitive between a and b
inline Matrix alignY(Vector3 a, Vector3 b, float rx, float rz) {
    Vector3 d = Vector3Subtract(b, a);
    float len = Vector3Length(d);
    if (len < 1e-5f) return MatrixTranslate(a.x, a.y, a.z);
    Vector3 dir = Vector3Scale(d, 1.f / len);
    Vector3 up{0, 1, 0};
    Matrix rot;
    float dt = Vector3DotProduct(up, dir);
    if (dt > 0.9999f) rot = MatrixIdentity();
    else if (dt < -0.9999f) rot = MatrixRotateX(PI_F);
    else rot = MatrixRotate(Vector3Normalize(Vector3CrossProduct(up, dir)), std::acos(dt));
    return MatrixMultiply(MatrixMultiply(MatrixScale(rx, len, rz), rot), MatrixTranslate(a.x, a.y, a.z));
}

// unit sphere scaled into an ellipsoid with its long axis from a to b
inline Matrix ellipsoid(Vector3 a, Vector3 b, float rx, float rz) {
    Vector3 mid = Vector3Lerp(a, b, 0.5f);
    Vector3 d = Vector3Subtract(b, a);
    float len = Vector3Length(d);
    if (len < 1e-5f) return MatrixMultiply(MatrixScale(rx, rx, rz), MatrixTranslate(mid.x, mid.y, mid.z));
    Vector3 dir = Vector3Scale(d, 1.f / len);
    float dt = std::clamp(Vector3DotProduct({0, 1, 0}, dir), -1.f, 1.f);
    Matrix rot = dt > 0.9999f ? MatrixIdentity() : dt < -0.9999f ? MatrixRotateX(PI_F) : MatrixRotate(Vector3Normalize(Vector3CrossProduct({0, 1, 0}, dir)), std::acos(dt));
    return MatrixMultiply(MatrixMultiply(MatrixScale(rx, len * 0.5f, rz), rot), MatrixTranslate(mid.x, mid.y, mid.z));
}

inline Material makeMat(Shader s) { Material m = LoadMaterialDefault(); m.shader = s; return m; }

}  // namespace rutil
