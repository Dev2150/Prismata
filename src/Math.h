#pragma once
#include <cmath>
#include <algorithm>

// ── Vec3 (already in Creature.h, redeclared here for Math.h standalone use)
// We rely on the one in Creature.h; this file only adds Vec4 + Mat4.

struct Vec4 {
    float x = 0, y = 0, z = 0, w = 0;
    Vec4() = default;
    Vec4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
    Vec4 operator+(const Vec4& o) const { return {x+o.x, y+o.y, z+o.z, w+o.w}; }
    Vec4 operator-(const Vec4& o) const { return {x-o.x, y-o.y, z-o.z, w-o.w}; }
    Vec4 operator*(float s)       const { return {x*s,   y*s,   z*s,   w*s  }; }
    float dot(const Vec4& o)      const { return x*o.x + y*o.y + z*o.z + w*o.w; }
};

// Row-major 4x4 matrix. m[row][col].
// Stored so that the raw float[16] array is ready to upload to D3D cbuffers
// using row_major packing (matches HLSL row_major or transposed column_major).
struct Mat4 {
    float m[4][4] = {};

    static Mat4 identity() {
        Mat4 r;
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.f;
        return r;
    }

    Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (int row = 0; row < 4; row++)
            for (int col = 0; col < 4; col++)
                for (int k = 0; k < 4; k++)
                    r.m[row][col] += m[row][k] * o.m[k][col];
        return r;
    }

    Mat4 transposed() const {
        Mat4 r;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                r.m[i][j] = m[j][i];
        return r;
    }

    // Transform a Vec4 by this matrix (row-vector * matrix)
    Vec4 transform(const Vec4& v) const {
        return {
            v.x*m[0][0] + v.y*m[1][0] + v.z*m[2][0] + v.w*m[3][0],
            v.x*m[0][1] + v.y*m[1][1] + v.z*m[2][1] + v.w*m[3][1],
            v.x*m[0][2] + v.y*m[1][2] + v.z*m[2][2] + v.w*m[3][2],
            v.x*m[0][3] + v.y*m[1][3] + v.z*m[2][3] + v.w*m[3][3],
        };
    }

    // Inverse (only correct for affine / orthogonal-ish matrices; uses Cramer's rule)
    Mat4 inversed() const {
        // Full 4x4 inverse via cofactors
        const float* src = &m[0][0];
        float inv[16], det;
        inv[0]  =  src[5]*src[10]*src[15] - src[5]*src[11]*src[14] - src[9]*src[6]*src[15] + src[9]*src[7]*src[14] + src[13]*src[6]*src[11] - src[13]*src[7]*src[10];
        inv[4]  = -src[4]*src[10]*src[15] + src[4]*src[11]*src[14] + src[8]*src[6]*src[15] - src[8]*src[7]*src[14] - src[12]*src[6]*src[11] + src[12]*src[7]*src[10];
        inv[8]  =  src[4]*src[9] *src[15] - src[4]*src[11]*src[13] - src[8]*src[5]*src[15] + src[8]*src[7]*src[13] + src[12]*src[5]*src[11] - src[12]*src[7]*src[9];
        inv[12] = -src[4]*src[9] *src[14] + src[4]*src[10]*src[13] + src[8]*src[5]*src[14] - src[8]*src[6]*src[13] - src[12]*src[5]*src[10] + src[12]*src[6]*src[9];
        inv[1]  = -src[1]*src[10]*src[15] + src[1]*src[11]*src[14] + src[9]*src[2]*src[15] - src[9]*src[3]*src[14] - src[13]*src[2]*src[11] + src[13]*src[3]*src[10];
        inv[5]  =  src[0]*src[10]*src[15] - src[0]*src[11]*src[14] - src[8]*src[2]*src[15] + src[8]*src[3]*src[14] + src[12]*src[2]*src[11] - src[12]*src[3]*src[10];
        inv[9]  = -src[0]*src[9] *src[15] + src[0]*src[11]*src[13] + src[8]*src[1]*src[15] - src[8]*src[3]*src[13] - src[12]*src[1]*src[11] + src[12]*src[3]*src[9];
        inv[13] =  src[0]*src[9] *src[14] - src[0]*src[10]*src[13] - src[8]*src[1]*src[14] + src[8]*src[2]*src[13] + src[12]*src[1]*src[10] - src[12]*src[2]*src[9];
        inv[2]  =  src[1]*src[6] *src[15] - src[1]*src[7] *src[14] - src[5]*src[2]*src[15] + src[5]*src[3]*src[14] + src[13]*src[2]*src[7]  - src[13]*src[3]*src[6];
        inv[6]  = -src[0]*src[6] *src[15] + src[0]*src[7] *src[14] + src[4]*src[2]*src[15] - src[4]*src[3]*src[14] - src[12]*src[2]*src[7]  + src[12]*src[3]*src[6];
        inv[10] =  src[0]*src[5] *src[15] - src[0]*src[7] *src[13] - src[4]*src[1]*src[15] + src[4]*src[3]*src[13] + src[12]*src[1]*src[7]  - src[12]*src[3]*src[5];
        inv[14] = -src[0]*src[5] *src[14] + src[0]*src[6] *src[13] + src[4]*src[1]*src[14] - src[4]*src[2]*src[13] - src[12]*src[1]*src[6]  + src[12]*src[2]*src[5];
        inv[3]  = -src[1]*src[6] *src[11] + src[1]*src[7] *src[10] + src[5]*src[2]*src[11] - src[5]*src[3]*src[10] - src[9] *src[2]*src[7]  + src[9] *src[3]*src[6];
        inv[7]  =  src[0]*src[6] *src[11] - src[0]*src[7] *src[10] - src[4]*src[2]*src[11] + src[4]*src[3]*src[10] + src[8] *src[2]*src[7]  - src[8] *src[3]*src[6];
        inv[11] = -src[0]*src[5] *src[11] + src[0]*src[7] *src[9]  + src[4]*src[1]*src[11] - src[4]*src[3]*src[9]  - src[8] *src[1]*src[7]  + src[8] *src[3]*src[5];
        inv[15] =  src[0]*src[5] *src[10] - src[0]*src[6] *src[9]  - src[4]*src[1]*src[10] + src[4]*src[2]*src[9]  + src[8] *src[1]*src[6]  - src[8] *src[2]*src[5];
        det = src[0]*inv[0] + src[1]*inv[4] + src[2]*inv[8] + src[3]*inv[12];
        if (std::abs(det) < 1e-8f) return identity();
        float invDet = 1.f / det;
        Mat4 result;
        for (int i = 0; i < 16; i++)
            (&result.m[0][0])[i] = inv[i] * invDet;
        return result;
    }

    // ── Camera matrices ───────────────────────────────────────────────────────

    // Right-handed look-at (eye, at, up)
    static Mat4 lookAtRH(float ex, float ey, float ez,
                         float ax, float ay, float az,
                         float ux, float uy, float uz) {
        // Forward (z-axis of camera space, pointing away from scene in RH)
        float fx = ex-ax, fy = ey-ay, fz = ez-az;
        float fl = std::sqrt(fx*fx+fy*fy+fz*fz);
        fx/=fl; fy/=fl; fz/=fl;
        // Right = Up x Forward
        float rx = uy*fz - uz*fy, ry = uz*fx - ux*fz, rz = ux*fy - uy*fx;
        float rl = std::sqrt(rx*rx+ry*ry+rz*rz);
        rx/=rl; ry/=rl; rz/=rl;
        // True up = Forward x Right
        float tx = fy*rz - fz*ry, ty = fz*rx - fx*rz, tz = fx*ry - fy*rx;

        Mat4 r;
        r.m[0][0]=rx; r.m[0][1]=tx; r.m[0][2]=fx; r.m[0][3]=0;
        r.m[1][0]=ry; r.m[1][1]=ty; r.m[1][2]=fy; r.m[1][3]=0;
        r.m[2][0]=rz; r.m[2][1]=tz; r.m[2][2]=fz; r.m[2][3]=0;
        r.m[3][0]=-(rx*ex+ry*ey+rz*ez);
        r.m[3][1]=-(tx*ex+ty*ey+tz*ez);
        r.m[3][2]=-(fx*ex+fy*ey+fz*ez);
        r.m[3][3]=1;
        return r;
    }

    // Right-handed perspective (fovY radians, aspect, near, far)
    static Mat4 perspectiveRH(float fovY, float aspect, float nearZ, float farZ) {
        float f = 1.f / std::tan(fovY * 0.5f);
        Mat4 r;
        r.m[0][0] = f / aspect;
        r.m[1][1] = f;
        r.m[2][2] = farZ  / (nearZ - farZ);
        r.m[2][3] = -1.f;
        r.m[3][2] = (nearZ * farZ) / (nearZ - farZ);
        return r;
    }
};

// ── Convenience: 3-component float pair for normals/colors ───────────────────
struct Float3 { float x=0,y=0,z=0; };
struct Float4 { float x=0,y=0,z=0,w=0; };

// Normalise a Float3
inline Float3 normalise3(float x, float y, float z) {
    float l = std::sqrt(x*x + y*y + z*z);
    if (l < 1e-6f) return {0,1,0};
    return {x/l, y/l, z/l};
}
