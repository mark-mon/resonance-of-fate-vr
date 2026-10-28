#pragma once
#include <cmath>

// Minimal math for converting between OpenXR and the game's camera conventions.
//
// Game (Aska): left-handed, X right, Y up, Z forward, centimetres.
//   4x4 matrices are float[16] row-major with column-vector math (translation in [3],[7],[11]),
//   so a camera world matrix's columns are right / up / forward / position.
// OpenXR: right-handed, X right, Y up, -Z forward, metres.
namespace so4vr::vm {

struct Vec3 { float x, y, z; };
struct Quat { float x, y, z, w; };

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Len(Vec3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
inline Vec3 Norm(Vec3 a) { float l = Len(a); return l > 1e-8f ? a * (1.0f / l) : Vec3{0, 0, 1}; }

// OpenXR -> game local axes (mirror Z) and metres -> centimetres * scale.
inline Vec3 XrToGamePos(Vec3 p, float cmPerMetre) { return {p.x * cmPerMetre, p.y * cmPerMetre, -p.z * cmPerMetre}; }
// Mirroring across the XY plane maps a rotation quaternion (x,y,z,w) to (-x,-y,z,w).
inline Quat XrToGameRot(Quat q) { return {-q.x, -q.y, q.z, q.w}; }

// Rotation matrix (row-major, column-vector convention) from a unit quaternion.
inline void QuatToMat3(Quat q, float r[9]) {
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    r[0] = 1 - 2 * (yy + zz); r[1] = 2 * (xy - wz);     r[2] = 2 * (xz + wy);
    r[3] = 2 * (xy + wz);     r[4] = 1 - 2 * (xx + zz); r[5] = 2 * (yz - wx);
    r[6] = 2 * (xz - wy);     r[7] = 2 * (yz + wx);     r[8] = 1 - 2 * (xx + yy);
}

// out = a * b (4x4, row-major storage). out may not alias a or b.
inline void Mul4(const float a[16], const float b[16], float out[16]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                             a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
}

// Builds a rigid transform from rotation + translation.
inline void MakeTransform(const float r[9], Vec3 t, float m[16]) {
    m[0] = r[0]; m[1] = r[1]; m[2] = r[2];  m[3] = t.x;
    m[4] = r[3]; m[5] = r[4]; m[6] = r[5];  m[7] = t.y;
    m[8] = r[6]; m[9] = r[7]; m[10] = r[8]; m[11] = t.z;
    m[12] = 0;   m[13] = 0;   m[14] = 0;    m[15] = 1;
}

// Removes pitch and roll from a camera world matrix, keeping yaw and position (level horizon).
inline void LevelCamera(const float in[16], float out[16]) {
    Vec3 fwd{in[2], in[6], in[10]};
    Vec3 f = Norm({fwd.x, 0.0f, fwd.z});
    if (std::fabs(fwd.x) < 1e-5f && std::fabs(fwd.z) < 1e-5f) {
        // Looking straight down (up): the camera's up (down) vector points the way it faces.
        float s = fwd.y < 0 ? 1.0f : -1.0f;
        f = Norm({in[1] * s, 0.0f, in[9] * s});
    }
    Vec3 u{0, 1, 0};
    Vec3 r = Cross(u, f); // left-handed: right = up x forward
    out[0] = r.x; out[1] = u.x; out[2] = f.x;  out[3] = in[3];
    out[4] = r.y; out[5] = u.y; out[6] = f.y;  out[7] = in[7];
    out[8] = r.z; out[9] = u.z; out[10] = f.z; out[11] = in[11];
    out[12] = 0;  out[13] = 0;  out[14] = 0;   out[15] = 1;
}

// General 4x4 inverse (row-major). Returns false if singular.
inline bool Inverse4(const float m[16], float out[16]) {
    float inv[16];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    double det = double(m[0]) * inv[0] + double(m[1]) * inv[4] + double(m[2]) * inv[8] + double(m[3]) * inv[12];
    if (det == 0.0) return false;
    float id = float(1.0 / det);
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * id;
    return true;
}

// Quaternion helpers (x, y, z, w).
inline Quat QMul(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat QConj(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }
inline Vec3 QRotate(Quat q, Vec3 v) {
    Quat p{v.x, v.y, v.z, 0};
    Quat r = QMul(QMul(q, p), QConj(q));
    return {r.x, r.y, r.z};
}

} // namespace so4vr::vm
