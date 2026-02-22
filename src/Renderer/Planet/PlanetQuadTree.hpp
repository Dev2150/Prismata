#pragma once
// ── PlanetQuadTree.hpp ────────────────────────────────────────────────────────
// Implements a quadtree LOD system for one face of a cube-sphere.
//
// MENTAL MODEL: The cube-sphere
// ─────────────────────────────
//   Start with a unit cube. Project every point on its surface outward onto
//   the enclosing sphere by normalising the position vector. You get 6 quad
//   faces that tile the sphere seamlessly.
//
//   Each face is parameterised by (u, v) ∈ [-1, 1]². A face function converts
//   (face, u, v) → normalised 3D direction → position on sphere surface.
//
// MENTAL MODEL: Quadtree LOD
// ──────────────────────────
//   The quadtree root covers the whole face. If the node's projected screen
//   size exceeds a threshold, it splits into 4 children that each cover one
//   quadrant. Leaf nodes generate and cache a GPU mesh (a small patch of
//   terrain). The deeper the node, the finer the mesh.
//
//   Split metric: worldNodeEdge / distToCamera  >  splitThreshold
//   Merge metric: worldNodeEdge / distToCamera  <  splitThreshold × 0.45
//   (hysteresis band prevents oscillation at the boundary)

#include <memory>
#include <array>
#include <vector>
#include <d3d11.h>
#include "Core/Math.hpp"

// ── PlanetConfig ──────────────────────────────────────────────────────────────
struct PlanetConfig {
    float    radius          = 1000.f;   // sphere radius (world units)
    Vec3     center          = {};       // world-space centre of the planet

    // LOD parameters
    int      maxDepth        = 18;       // deepest allowed subdivision level
    float    splitThreshold  = 1.2f;    // split when (edgeLen / camDist) > this
                                         // lower = more aggressive LOD

    // Mesh resolution: each leaf patch is patchRes×patchRes vertices.
    // Must satisfy (patchRes - 1) = power of 2 for stitching.
    // 17 → 16×16 quads = 512 triangles per leaf.
    int      patchRes        = 17;

    // Terrain noise
    float    heightScale     = 100.f;   // max displacement from sphere surface
    float    noiseFrequency  = 1.f;     // base noise frequency (world-space scale)
    int      noiseOctaves    = 8;
    float    noisePersist    = 0.5f;
    float    noiseLacun      = 2.f;

    // Atmospheric / visual
    float    seaLevel        = 0.f;     // y-offset where ocean surface sits
    float    snowLine        = 0.92f;   // fraction of heightScale above which = snow
};

// ── Cube face definitions ─────────────────────────────────────────────────────
// Each face has a normal (face centre direction), a right vector, and an up vector.
// faceUVtoDir(face, u, v) = normalise(normal + u*right + v*up).
//
//  Face 0: +X    Face 1: -X
//  Face 2: +Y    Face 3: -Y
//  Face 4: +Z    Face 5: -Z
//
// The six faces tile with correct orientation so that edges always align.
struct FaceAxes {
    Vec3 normal, right, up;
};
static const FaceAxes FACE_AXES[6] = {
    {{ 1, 0, 0}, { 0, 0,-1}, { 0, 1, 0}},  // +X
    {{-1, 0, 0}, { 0, 0, 1}, { 0, 1, 0}},  // -X
    {{ 0, 1, 0}, { 1, 0, 0}, { 0, 0,-1}},  // +Y
    {{ 0,-1, 0}, { 1, 0, 0}, { 0, 0, 1}},  // -Y
    {{ 0, 0, 1}, { 1, 0, 0}, { 0, 1, 0}},  // +Z
    {{ 0, 0,-1}, {-1, 0, 0}, { 0, 1, 0}},  // -Z
};

// Convert (face, u, v) in [-1,1]² → normalised 3D direction on the unit sphere.
inline Vec3 faceUVtoDir(int face, float u, float v) {
    const FaceAxes& ax = FACE_AXES[face];
    Vec3 raw = {
        ax.normal.x + ax.right.x * u + ax.up.x * v,
        ax.normal.y + ax.right.y * u + ax.up.y * v,
        ax.normal.z + ax.right.z * u + ax.up.z * v,
    };
    return raw.normalised();
}

// ── PlanetVertex ──────────────────────────────────────────────────────────────
// GPU vertex layout for planet patches. Position is in world space;
// normal is computed per-vertex from finite differences during mesh build.
struct PlanetVertex {
    float pos[3];     // world-space position on (displaced) sphere surface
    float nrm[3];     // surface normal (pointing away from planet centre)
    float uv[2];      // local [0,1]² UV within this patch (for texturing)
    float height;     // normalised height above sea level [0,1] for colour blending
    float pad;        // pad to 32 bytes (multiple of 16 for D3D alignment)
};

// ── PlanetNode ────────────────────────────────────────────────────────────────
// One node of the quadtree. Leaf nodes own a GPU mesh; inner nodes only hold
// four children. The split/merge decision is made in PlanetFaceTree::update().
struct PlanetNode {
    // ── Spatial identity ─────────────────────────────────────────────────────
    int   face;                 // which cube face (0-5)
    int   depth;                // depth from root (0 = full face)
    float u0, v0, u1, v1;      // face UV bounds covered by this node

    // Precomputed geometry (computed once at node creation, never changes)
    Vec3  centerDir;            // normalised direction to this node's centre
    Vec3  centerWorld;          // world-space position of the centre (no height displacement)
    float edgeLen;              // approximate world-space edge length of this node

    // ── Tree structure ───────────────────────────────────────────────────────
    bool  isSplit = false;
    // Children are ordered: [0]=BL, [1]=BR, [2]=TL, [3]=TR
    //  (u- v-) | (u+ v-) | (u- v+) | (u+ v+)
    std::unique_ptr<PlanetNode> children[4];

    // ── GPU resources (leaf nodes only) ─────────────────────────────────────
    ID3D11Buffer* vb       = nullptr;
    ID3D11Buffer* ib       = nullptr;
    int           idxCount = 0;
    bool          meshBuilt= false;

    // ── Constructor ──────────────────────────────────────────────────────────
    PlanetNode(int face, int depth, float u0, float v0, float u1, float v1,
               const PlanetConfig& cfg)
        : face(face), depth(depth), u0(u0), v0(v0), u1(u1), v1(v1)
    {
        float umid = (u0 + u1) * 0.5f;
        float vmid = (v0 + v1) * 0.5f;
        centerDir   = faceUVtoDir(face, umid, vmid);
        centerWorld = {
            cfg.center.x + centerDir.x * cfg.radius,
            cfg.center.y + centerDir.y * cfg.radius,
            cfg.center.z + centerDir.z * cfg.radius,
        };
        // Edge length ≈ arc length covered by this node on the sphere surface.
        // At depth 0, a face spans 90° → arc = π/2 × radius.
        // Each subdivision halves the angular span: divide by 2^depth.
        edgeLen = cfg.radius * (3.14159265f * 0.5f) / (float)(1 << depth);
    }

    // Recursively release all D3D11 GPU buffers (called on shutdown or world reset)
    void releaseGPU() {
        if (vb) { vb->Release(); vb = nullptr; }
        if (ib) { ib->Release(); ib = nullptr; }
        idxCount = 0; meshBuilt = false;
        for (auto& ch : children) if (ch) ch->releaseGPU();
    }

    ~PlanetNode() { releaseGPU(); }
};

// ── PlanetFaceTree ────────────────────────────────────────────────────────────
// Manages the entire quadtree for one cube face.
// Owns the root node and drives the split/merge update every frame.
struct PlanetFaceTree {
    int           faceIndex;
    PlanetConfig  cfg;
    std::unique_ptr<PlanetNode> root;

    explicit PlanetFaceTree(int face, const PlanetConfig& c)
        : faceIndex(face), cfg(c)
    {
        root = std::make_unique<PlanetNode>(face, 0, -1.f, -1.f, 1.f, 1.f, cfg);
    }

    // ── update ───────────────────────────────────────────────────────────────
    // Traverses the tree and splits or merges nodes based on camera distance.
    // Must be called once per frame before render().
    // dev / ctx needed to create/release GPU buffers during split/merge.
    void update(const Vec3& camPos, ID3D11Device* dev, ID3D11DeviceContext* ctx);

    // Collect all leaf nodes that have a built mesh (for rendering)
    void collectLeaves(std::vector<PlanetNode*>& out) const {
        collectLeavesRec(root.get(), out);
    }

    // Count total nodes (for diagnostics)
    int nodeCount() const { return countRec(root.get()); }
    int leafCount()  const { return countLeavesRec(root.get()); }

private:
    void updateRec(PlanetNode* node, const Vec3& camPos,
                   ID3D11Device* dev, ID3D11DeviceContext* ctx);

    void splitNode (PlanetNode* node, ID3D11Device* dev, ID3D11DeviceContext* ctx);
    void mergeNode (PlanetNode* node);
    void buildMesh (PlanetNode* node, ID3D11Device* dev);

    void collectLeavesRec(const PlanetNode* n, std::vector<PlanetNode*>& out) const;

    static int countRec(const PlanetNode* n) {
        if (!n) return 0;
        int s = 1;
        if (n->isSplit)
            for (const auto& ch : n->children) s += countRec(ch.get());
        return s;
    }
    static int countLeavesRec(const PlanetNode* n) {
        if (!n) return 0;
        if (!n->isSplit) return 1;
        int s = 0;
        for (const auto& ch : n->children) s += countLeavesRec(ch.get());
        return s;
    }
};

// ── PlanetQuadTree ────────────────────────────────────────────────────────────
// Top-level class: 6 face trees that together form the complete sphere.
struct PlanetQuadTree {
    PlanetConfig                    cfg;
    std::array<PlanetFaceTree, 6>   faces;

    explicit PlanetQuadTree(const PlanetConfig& c)
        : cfg(c),
          faces{
            PlanetFaceTree(0, c), PlanetFaceTree(1, c),
            PlanetFaceTree(2, c), PlanetFaceTree(3, c),
            PlanetFaceTree(4, c), PlanetFaceTree(5, c)
          }
    {}

    void update(const Vec3& camPos, ID3D11Device* dev, ID3D11DeviceContext* ctx) {
        for (auto& f : faces) f.update(camPos, dev, ctx);
    }

    void collectLeaves(std::vector<PlanetNode*>& out) const {
        for (const auto& f : faces) f.collectLeaves(out);
    }

    int totalNodes()  const {
        int s = 0; for (const auto& f : faces) s += f.nodeCount(); return s;
    }
    int totalLeaves() const {
        int s = 0; for (const auto& f : faces) s += f.leafCount(); return s;
    }

    void shutdown() {
        for (auto& f : faces) if (f.root) f.root->releaseGPU();
    }
};
