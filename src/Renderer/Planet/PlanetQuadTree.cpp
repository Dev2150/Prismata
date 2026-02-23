// ── PlanetQuadTree.cpp ────────────────────────────────────────────────────────
// Implements the split/merge logic and GPU mesh generation for planet quadtree nodes.
//
// SPLIT DECISION
// ──────────────
//   A node should subdivide when its projected screen-space size is large.
//   We approximate this with: metric = nodeEdgeLen / distToNearestPoint.
//   "Nearest point" uses the node's sphere-surface centre for simplicity.
//   Metric > splitThreshold → split; < splitThreshold × 0.45 → merge (hysteresis).
//
// MESH GENERATION
// ───────────────
//   Each leaf node generates a PlanetVertex grid of size patchRes × patchRes.
//   Vertex positions are computed by:
//     1. Sample (u, v) across the node's UV bounds
//     2. Convert to unit-sphere direction via faceUVtoDir()
//     3. Sample 3D terrain noise along that direction
//     4. Displace: pos = dir × (radius + height)
//   Normals are approximated from cross-products of neighbouring vertices
//   (computed during the same pass — one extra row/column sampled for border verts).

#include "PlanetQuadTree.hpp"
#include "PlanetNoise.hpp"
#include <cmath>
#include <algorithm>
#include <vector>
#include <cstring>

// ── Mesh build helper ─────────────────────────────────────────────────────────
// Samples height at a UV position on a given face (no GPU, CPU-only).
static float sampleH(int face, float u, float v, const PlanetConfig& cfg) {
    Vec3 dir = faceUVtoDir(face, u, v);
    return PlanetNoise::sampleHeight(
        dir.x, dir.y, dir.z,
        cfg.heightScale,
        0.3f,   // seaFloor fraction
        0);
}

// Compute a world-space position on the (displaced) sphere surface.
static Vec3 surfacePos(int face, float u, float v, const PlanetConfig& cfg) {
    Vec3 dir = faceUVtoDir(face, u, v);
    float h  = PlanetNoise::sampleHeight(dir.x, dir.y, dir.z, cfg.heightScale);
    h = std::max(h, 0.0f); // Clamp to sea level for a flat water surface
    float r  = cfg.radius + h;
    return {
        cfg.center.x + dir.x * r,
        cfg.center.y + dir.y * r,
        cfg.center.z + dir.z * r,
    };
}

// ── PlanetFaceTree::buildMesh ─────────────────────────────────────────────────
// Generates a patchRes × patchRes vertex grid for a leaf node.
// Normals computed via central finite differences on the sphere surface.
void PlanetFaceTree::buildMesh(PlanetNode* node, ID3D11Device* dev) {
    const int res = cfg.patchRes;   // e.g. 17
    const int quads = res - 1;

    // UV step within this node's UV bounds
    float uRange = node->u1 - node->u0;
    float vRange = node->v1 - node->v0;
    float du = uRange / quads;
    float dv = vRange / quads;

    // UV epsilon for normal finite differences (1/4 of a cell)
    float eps = std::min(du, dv) * 0.25f;

    std::vector<PlanetVertex> verts;
    verts.reserve(res * res);

    for (int row = 0; row < res; row++) {
        float v = node->v0 + row * dv;
        for (int col = 0; col < res; col++) {
            float u = node->u0 + col * du;

            // Surface position at this vertex
            Vec3 pos = surfacePos(node->face, u, v, cfg);

            // Approximate normal: central finite difference on the sphere surface.
            // Sample two neighbours in each direction, compute tangents, cross product.
            Vec3 px = surfacePos(node->face, u + eps, v, cfg);
            Vec3 mx = surfacePos(node->face, u - eps, v, cfg);
            Vec3 pz = surfacePos(node->face, u, v + eps, cfg);
            Vec3 mz = surfacePos(node->face, u, v - eps, cfg);

            Vec3 tangU = {px.x - mx.x, px.y - mx.y, px.z - mx.z};
            Vec3 tangV = {pz.x - mz.x, pz.y - mz.y, pz.z - mz.z};

            // Cross product tangU × tangV = surface normal (pointing outward)
            Vec3 nrm = {
                tangU.y * tangV.z - tangU.z * tangV.y,
                tangU.z * tangV.x - tangU.x * tangV.z,
                tangU.x * tangV.y - tangU.y * tangV.x,
            };
            nrm = nrm.normalised();

            // Normalised height for biome colour blending in the shader
            float rawH = sampleH(node->face, u, v, cfg);
            float normH = (rawH + cfg.heightScale * 0.3f) / (cfg.heightScale * 1.3f);
            normH = std::max(0.f, std::min(1.f, normH));

            PlanetVertex pv;
            pv.pos[0] = pos.x; pv.pos[1] = pos.y; pv.pos[2] = pos.z;
            pv.nrm[0] = nrm.x; pv.nrm[1] = nrm.y; pv.nrm[2] = nrm.z;
            pv.uv[0]  = (float)col / quads;
            pv.uv[1]  = (float)row / quads;
            pv.height = normH;
            pv.pad    = 0.f;
            verts.push_back(pv);
        }
    }

    // ── Index buffer: res×res grid → two triangles per quad ──────────────────
    std::vector<uint32_t> idxs;
    idxs.reserve(quads * quads * 6);
    for (int row = 0; row < quads; row++) {
        for (int col = 0; col < quads; col++) {
            uint32_t TL = row * res + col;
            uint32_t TR = TL + 1;
            uint32_t BL = TL + res;
            uint32_t BR = BL + 1;
            // Counter-clockwise winding (right-handed, looking outward from planet)
            idxs.push_back(TL); idxs.push_back(TR); idxs.push_back(BL);
            idxs.push_back(TR); idxs.push_back(BR); idxs.push_back(BL);
        }
    }

    // ── Upload to GPU (IMMUTABLE: terrain never changes at runtime) ───────────
    D3D11_BUFFER_DESC bd{};
    D3D11_SUBRESOURCE_DATA sd{};

    bd.Usage     = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.ByteWidth = (UINT)(verts.size() * sizeof(PlanetVertex));
    sd.pSysMem   = verts.data();
    dev->CreateBuffer(&bd, &sd, &node->vb);

    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.ByteWidth = (UINT)(idxs.size() * sizeof(uint32_t));
    sd.pSysMem   = idxs.data();
    dev->CreateBuffer(&bd, &sd, &node->ib);

    node->idxCount = (int)idxs.size();
    node->meshBuilt= true;
}

// ── PlanetFaceTree::splitNode ─────────────────────────────────────────────────
// Splits a leaf node into 4 children. The parent's mesh is discarded (children
// take over rendering). Children are created but meshes built lazily next frame.
void PlanetFaceTree::splitNode(PlanetNode* node, ID3D11Device* dev, ID3D11DeviceContext* /*ctx*/) {
    if (node->depth >= cfg.maxDepth) return;

    float umid = (node->u0 + node->u1) * 0.5f;
    float vmid = (node->v0 + node->v1) * 0.5f;

    // 4 children cover the four quadrants of the parent UV space
    // [0]=BL, [1]=BR, [2]=TL, [3]=TR
    node->children[0] = std::make_unique<PlanetNode>(
        node->face, node->depth+1, node->u0, node->v0, umid, vmid, cfg);
    node->children[1] = std::make_unique<PlanetNode>(
        node->face, node->depth+1, umid, node->v0, node->u1, vmid, cfg);
    node->children[2] = std::make_unique<PlanetNode>(
        node->face, node->depth+1, node->u0, vmid, umid, node->v1, cfg);
    node->children[3] = std::make_unique<PlanetNode>(
        node->face, node->depth+1, umid, vmid, node->u1, node->v1, cfg);

    // Release parent mesh: children will render instead
    node->meshBuilt = false;
    node->idxCount  = 0;
    node->isSplit   = true;

    // Build child meshes immediately (blocking).
    // For a non-blocking version, push to a worker thread queue instead.
    for (auto& ch : node->children)
        buildMesh(ch.get(), dev);
}

// ── PlanetFaceTree::mergeNode ─────────────────────────────────────────────────
// Collapses 4 children back into the parent (builds parent mesh, destroys children).
void PlanetFaceTree::mergeNode(PlanetNode* node) {
    // Children are destroyed by unique_ptr when we reset them
    for (auto& ch : node->children) {
        if (ch) ch->releaseGPU();
        ch.reset();
    }
    node->isSplit   = false;
    node->meshBuilt = false;  // will be rebuilt next update pass
}

// ── PlanetFaceTree::updateRec ─────────────────────────────────────────────────
// Depth-first traversal. Split or merge based on the LOD metric.
void PlanetFaceTree::updateRec(PlanetNode* node, const Vec3& camPos,
                               ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (!node) return;

    // Distance from camera to the node's sphere-surface centre.
    // Using the displaced centre would be marginally better but requires a noise
    // sample on every node every frame — the undisplaced version is good enough.
    float dx = camPos.x - node->centerWorld.x;
    float dy = camPos.y - node->centerWorld.y;
    float dz = camPos.z - node->centerWorld.z;
    float dist = std::sqrt(dx*dx + dy*dy + dz*dz);

    // Subtract bounding radius to get distance to the closest edge of the node
    dist = std::max(1.f, dist - node->edgeLen * 0.75f);

    // LOD metric: ratio of node's edge length to camera distance
    float metric = node->edgeLen / dist;

    // ── Back-face culling at tree level ───────────────────────────────────────
    // If the node is on the far side of the planet, skip subdivision entirely.
    // dot(centerDir, camDir) < threshold → node faces away from camera.
    Vec3 camDir = {
        camPos.x - cfg.center.x,
        camPos.y - cfg.center.y,
        camPos.z - cfg.center.z,
    };
    float camDistFromCenter = camDir.len();
    if (camDistFromCenter > 1e-3f) {
        float invLen = 1.f / camDistFromCenter;
        float dot = node->centerDir.x * camDir.x * invLen
                  + node->centerDir.y * camDir.y * invLen
                  + node->centerDir.z * camDir.z * invLen;
        // Skip nodes facing >110° away (well behind the horizon)
        // At horizon, dot ≈ sin(acos(radius/camDist)) which is slightly negative
        // The exact horizon angle: cos(asin(radius/camDist))
        float horizonCos = -std::sqrt(std::max(0.f,
            1.f - (cfg.radius / camDistFromCenter) *
                  (cfg.radius / camDistFromCenter)));
        if (dot < horizonCos - 0.15f) {
            // Far side of planet: merge everything and stop
            if (node->isSplit) mergeNode(node);
            return;
        }
    }

    // ── Split / merge decision ────────────────────────────────────────────────
    bool shouldSplit = (metric > cfg.splitThreshold)
                    && (node->depth < cfg.maxDepth);
    bool shouldMerge = (metric < cfg.splitThreshold * 0.45f);

    if (!node->isSplit) {
        // Leaf node
        if (shouldSplit) {
            splitNode(node, dev, ctx);
        } else if (!node->meshBuilt) {
            buildMesh(node, dev);
        }
    } else {
        // Inner node: recurse first, then check if all children can be merged
        for (auto& ch : node->children)
            updateRec(ch.get(), camPos, dev, ctx);

        if (shouldMerge) {
            // All children are leaves (check before merge to avoid partial collapse)
            bool allChildrenAreLeaves = true;
            for (const auto& ch : node->children)
                if (ch && ch->isSplit) { allChildrenAreLeaves = false; break; }

            if (allChildrenAreLeaves) {
                mergeNode(node);
                // Rebuild the coarser mesh for this node
                buildMesh(node, dev);
            }
        }
    }
}

// ── PlanetFaceTree::update (public) ──────────────────────────────────────────
void PlanetFaceTree::update(const Vec3& camPos, ID3D11Device* dev,
                            ID3D11DeviceContext* ctx) {
    updateRec(root.get(), camPos, dev, ctx);
}

// ── PlanetFaceTree::collectLeavesRec ─────────────────────────────────────────
void PlanetFaceTree::collectLeavesRec(PlanetNode *n,
                                      std::vector<PlanetNode *> &out) const {
    if (!n) return;
    if (!n->isSplit) {
        if (n->meshBuilt)
            out.emplace_back(n);
        return;
    }
    for (const auto& ch : n->children)
        collectLeavesRec(ch.get(), out);
}
