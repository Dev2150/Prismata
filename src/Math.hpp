#pragma once
#include <cmath>
#include <algorithm>

// ── Vec3 ──────────────────────────────────────────────────────────────────────
// 3-component float vector (position, velocity, direction).
// Y is the vertical (up) axis; X and Z are horizontal.
struct Vec3 {
    float x = 0, y = 0, z = 0;

    Vec3 operator+(const Vec3& o) const { return {x+o.x, y+o.y, z+o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x-o.x, y-o.y, z-o.z}; }
    Vec3 operator*(float s)       const { return {x*s,   y*s,   z*s  }; }
    float dot(const Vec3& o)      const { return x*o.x + y*o.y + z*o.z; }
    float len2()                  const { return x*x + y*y + z*z; }    // squared length (avoids sqrt when only comparing distances)
    float len()                   const { return std::sqrt(len2()); }
    Vec3  normalised()            const { float l=len(); return l>1e-6f?(*this)*(1.f/l):Vec3{}; }
    Vec3& operator+=(const Vec3& o){ x+=o.x; y+=o.y; z+=o.z; return *this; }
};

// ── Vec4 ──────────────────────────────────────────────────────────────────────
// 4-component float vector; used for clip-space coordinates (x,y,z,w) when
// multiplying by a 4×4 projection matrix.
struct Vec4 {
    float x = 0, y = 0, z = 0, w = 0;
    Vec4() = default;
    Vec4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
    Vec4 operator+(const Vec4& o) const { return {x+o.x, y+o.y, z+o.z, w+o.w}; }
    Vec4 operator-(const Vec4& o) const { return {x-o.x, y-o.y, z-o.z, w-o.w}; }
    Vec4 operator*(float s)       const { return {x*s,   y*s,   z*s,   w*s  }; }
    float dot(const Vec4& o)      const { return x*o.x + y*o.y + z*o.z + w*o.w; }
};

// ── Mat4 ──────────────────────────────────────────────────────────────────────
// Row-major 4×4 matrix. m[row][col].
//
// "Row-major" means the four elements of row 0 are stored contiguously in
// memory: m[0][0..3], then row 1: m[1][0..3], etc.
// This matches HLSL's row_major cbuffer layout, so the raw float[16] array
// can be uploaded directly to a D3D11 constant buffer without transposing.
//
// Vectors are treated as ROW vectors, multiplied on the LEFT: v' = v * M.
// When combining transforms: M = View * Proj (left-to-right application order).
struct Mat4 {
    float m[4][4] = {};

    static Mat4 identity() {
        Mat4 r;
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.f;
        return r;
    }

    // Standard O(n³) matrix multiply: (A*B)[i][j] = Σ A[i][k] * B[k][j]
    Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (int row = 0; row < 4; row++)
            for (int col = 0; col < 4; col++)
                for (int k = 0; k < 4; k++)
                    r.m[row][col] += m[row][k] * o.m[k][col];
        return r;
    }

    // Swap rows and columns. Used to convert between row-major and column-major
    // conventions when uploading to APIs that expect the opposite layout.
    Mat4 transposed() const {
        Mat4 r;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                r.m[i][j] = m[j][i];
        return r;
    }

    // Transform a Vec4 by this matrix (row-vector × matrix convention).
    // Each output component is a dot product of v with the corresponding column.
    Vec4 transform(const Vec4& v) const {
        return {
            v.x*m[0][0] + v.y*m[1][0] + v.z*m[2][0] + v.w*m[3][0],
            v.x*m[0][1] + v.y*m[1][1] + v.z*m[2][1] + v.w*m[3][1],
            v.x*m[0][2] + v.y*m[1][2] + v.z*m[2][2] + v.w*m[3][2],
            v.x*m[0][3] + v.y*m[1][3] + v.z*m[2][3] + v.w*m[3][3],
        };
    }

    // Full 4×4 matrix inverse via Cramer's rule (cofactor expansion).
    //
    // Cramer's rule:  A⁻¹ = adj(A) / det(A)
    // where adj(A) is the adjugate (transpose of the cofactor matrix).
    //
    // The cofactors are computed by expanding the determinant along each row/column.
    // Each inv[i] below is the (i-th element of adj(A)) = signed 3×3 minor.
    //
    // This is expensive (O(n³) arithmetic, many terms) but only called for ray-
    // picking (once per click), not in the main render loop.
    // Returns identity if the matrix is singular (det ≈ 0).
    Mat4 inversed() const {
        const float* src = &m[0][0];
        float inv[16], det;

        // Cofactor expansion – each inv[i] = cofactor of src[i] in the 4×4 matrix
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

        // det = dot(first row of src, first column of adj)
        det = src[0]*inv[0] + src[1]*inv[4] + src[2]*inv[8] + src[3]*inv[12];
        if (std::abs(det) < 1e-8f) return identity();  // singular – return identity to avoid NaN
        float invDet = 1.f / det;
        Mat4 result;
        for (int i = 0; i < 16; i++)
            (&result.m[0][0])[i] = inv[i] * invDet;
        return result;
    }

    // ── Camera matrices ───────────────────────────────────────────────────────

    // Right-handed look-at view matrix.
    // Builds a coordinate frame where the camera looks from `eye` toward `at`,
    // with `up` hinting which way is "up". The resulting matrix transforms world
    // positions into camera (view) space.
    //
    // Right-handed convention: the camera looks down the -Z axis in view space.
    // This is consistent with OpenGL and D3D right-handed projection matrices.
    static Mat4 lookAtRH(float ex, float ey, float ez,
                         float ax, float ay, float az,
                         float ux, float uy, float uz) {
        // Forward (camera -Z): points from target back toward the eye
        float fx = ex-ax, fy = ey-ay, fz = ez-az;
        float fl = std::sqrt(fx*fx+fy*fy+fz*fz);
        fx/=fl; fy/=fl; fz/=fl;

        // Right = Up × Forward (cross product gives the camera's +X axis)
        float rx = uy*fz - uz*fy, ry = uz*fx - ux*fz, rz = ux*fy - uy*fx;
        float rl = std::sqrt(rx*rx+ry*ry+rz*rz);
        rx/=rl; ry/=rl; rz/=rl;

        // True up = Forward × Right (reorthogonalise in case input Up was not perpendicular)
        float tx = fy*rz - fz*ry, ty = fz*rx - fx*rz, tz = fx*ry - fy*rx;

        // Pack into a 4×4 row-major matrix (translation in the bottom row)
        Mat4 r;
        r.m[0][0]=rx; r.m[0][1]=tx; r.m[0][2]=fx; r.m[0][3]=0;
        r.m[1][0]=ry; r.m[1][1]=ty; r.m[1][2]=fy; r.m[1][3]=0;
        r.m[2][0]=rz; r.m[2][1]=tz; r.m[2][2]=fz; r.m[2][3]=0;
        r.m[3][0]=-(rx*ex+ry*ey+rz*ez);  // -dot(right, eye)
        r.m[3][1]=-(tx*ex+ty*ey+tz*ez);  // -dot(up, eye)
        r.m[3][2]=-(fx*ex+fy*ey+fz*ez);  // -dot(forward, eye)
        r.m[3][3]=1;
        return r;
    }

    // Right-handed perspective projection matrix.
    // Maps the view frustum to clip space: X/Y in [-1,1], Z in [-1,0] (D3D convention).
    //
    // fovY   – vertical field of view in radians
    // aspect – viewport width / height
    // nearZ  – near clip plane distance (must be > 0)
    // farZ   – far clip plane distance
    //
    // f = 1/tan(fovY/2) is the "focal length" scaling factor.
    static Mat4 perspectiveRH(float fovY, float aspect, float nearZ, float farZ) {
        float f = 1.f / std::tan(fovY * 0.5f);  // cotangent of half-FOV
        Mat4 r;
        r.m[0][0] = f / aspect;    // scale X by inverse aspect ratio
        r.m[1][1] = f;             // scale Y by focal length
        r.m[2][2] = farZ  / (nearZ - farZ);      // remap Z to [0,1] clip range
        r.m[2][3] = -1.f;          // perspective divide trigger (w = -z_view)
        r.m[3][2] = (nearZ * farZ) / (nearZ - farZ);  // depth bias for near plane
        return r;
    }
};

// ── Convenience float structs ─────────────────────────────────────────────────
// Used for normals and colours where we want a plain aggregate without Vec3 operators.
struct Float3 { float x=0,y=0,z=0; };
struct Float4 { float x=0,y=0,z=0,w=0; };

// Normalise a Float3 to unit length. Returns (0,1,0) for zero-length input
// to avoid NaN propagation into the rendering pipeline.
inline Float3 normalise3(float x, float y, float z) {
    float l = std::sqrt(x*x + y*y + z*z);
    if (l < 1e-6f) return {0,1,0};
    return {x/l, y/l, z/l};
}
