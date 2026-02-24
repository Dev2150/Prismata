// ── Save / Load ───────────────────────────────────────────────────────────────
// Binary format layout:
//   [4]  magic "EVOS"
//   [4]  version uint32 = 2
//   [4]  simTime float
//   [4]  nextID uint32
//   [4]  nextSpeciesID uint32
//   [4]  creature count uint32
//   per creature: id, parentA, parentB (uint32×3)
//                 generation, speciesID (uint32×2)
//                 pos.xyz, vel.xyz, yaw (float×7)
//                 genome.raw (float×GENOME_SIZE)
//                 needs.level (float×DRIVE_COUNT)
//                 needs.craveRate (float×DRIVE_COUNT)
//                 energy, maxEnergy, age, lifespan, mass (float×5)
//                 behavior (uint32)
//                 gestTimer (float)
//                 mateTarget (uint32)
//   [4]  plant count uint32
//   per plant:    pos.xyz (float×3)
//                 nutrition, growTimer (float×2)
//                 alive, type (uint8×2)
//   [4]  species count uint32
//   per species:  id (uint32)
//                 count, allTime (int32×2)
//                 color (float×3)
//                 centroid.raw (float×GENOME_SIZE)
//                 nameLen (uint32) + name bytes

#include <cstring>
#include <fstream>
#include "World.hpp"

bool World::saveToFile(const char* path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;

    // Helper lambdas for clean writes
    auto writeF  = [&](float v)    { f.write(reinterpret_cast<const char*>(&v), sizeof(v)); };
    auto writeU32= [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), sizeof(v)); };
    auto writeI32= [&](int32_t v)  { f.write(reinterpret_cast<const char*>(&v), sizeof(v)); };
    auto writeU8 = [&](uint8_t v)  { f.write(reinterpret_cast<const char*>(&v), sizeof(v)); };
    auto writeFA = [&](const float* arr, int n) {
        f.write(reinterpret_cast<const char*>(arr), sizeof(float) * n);
    };
    auto writeVec3 = [&](const Vec3& v) {
        writeF(v.x); writeF(v.y); writeF(v.z);
    };

    // Header
    f.write("EVOS", 4);
    writeU32(3);   // version

    // World time and ID counters
    writeF(simTime);
    writeU32(nextID);
    writeU32(nextSpeciesID);

    // ── Creatures ─────────────────────────────────────────────────────────────
    // Only save alive creatures; dead ones will be removed anyway
    uint32_t cntAlive = 0;
    for (const auto& c : creatures) if (c.alive) cntAlive++;
    writeU32(cntAlive);

    for (const auto& c : creatures) {
        if (!c.alive) continue;

        // Identity
        writeU32(c.id);
        writeU32(c.parentA);
        writeU32(c.parentB);
        writeU32(c.generation);
        writeU32(c.speciesID);

        // Spatial state
        writeVec3(c.pos);
        writeVec3(c.vel);
        writeF(c.yaw);

        // Genome
        writeFA(c.genome.raw.data(), GENOME_SIZE);

        // Needs
        writeFA(c.needs.urgency.data(), DRIVE_COUNT);
        writeFA(c.needs.craveRate.data(), DRIVE_COUNT);
        writeFA(c.needs.desireMult.data(), DRIVE_COUNT);

        // Biology
        writeF(c.energy);
        writeF(c.maxEnergy);
        writeF(c.age);
        writeF(c.lifespan);
        writeF(c.mass);

        // Behaviour
        writeU32(static_cast<uint32_t>(c.behavior));
        writeF(c.gestTimer);
        writeU32(c.mateTarget);
    }

    // ── Plants ────────────────────────────────────────────────────────────────
    uint32_t pcnt = static_cast<uint32_t>(plants.size());
    writeU32(pcnt);
    for (const auto& p : plants) {
        writeVec3(p.pos);
        writeF(p.nutrition);
        writeF(p.growTimer);
        writeU8(p.alive ? 1 : 0);
        writeU8(p.type);
    }

    // ── Species ───────────────────────────────────────────────────────────────
    uint32_t scnt = static_cast<uint32_t>(species.size());
    writeU32(scnt);
    for (const auto& sp : species) {
        writeU32(sp.id);
        writeI32(sp.count);
        writeI32(sp.allTime);
        writeFA(sp.color, 3);
        writeFA(sp.centroid.raw.data(), GENOME_SIZE);
        uint32_t nlen = static_cast<uint32_t>(sp.name.size());
        writeU32(nlen);
        f.write(sp.name.c_str(), nlen);
    }

    return f.good();
}

bool World::loadFromFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    // Helper lambdas for clean reads
    auto readF  = [&]() -> float    { float v=0;    f.read(reinterpret_cast<char*>(&v), sizeof(v)); return v; };
    auto readU32= [&]() -> uint32_t { uint32_t v=0; f.read(reinterpret_cast<char*>(&v), sizeof(v)); return v; };
    auto readI32= [&]() -> int32_t  { int32_t v=0;  f.read(reinterpret_cast<char*>(&v), sizeof(v)); return v; };
    auto readU8 = [&]() -> uint8_t  { uint8_t v=0;  f.read(reinterpret_cast<char*>(&v), sizeof(v)); return v; };
    auto readFA = [&](float* arr, int n) {
        f.read(reinterpret_cast<char*>(arr), sizeof(float) * n);
    };
    auto readVec3 = [&]() -> Vec3 {
        Vec3 v; v.x=readF(); v.y=readF(); v.z=readF(); return v;
    };

    // ── Header ────────────────────────────────────────────────────────────────
    char magic[4] = {};
    f.read(magic, 4);
    if (std::strncmp(magic, "EVOS", 4) != 0) return false;

    uint32_t version = readU32();
    if (version != 3) return false;   // incompatible version

    // ── World state ───────────────────────────────────────────────────────────
    simTime       = readF();
    nextID        = readU32();
    nextSpeciesID = readU32();

    // ── Creatures ─────────────────────────────────────────────────────────────
    creatures.clear();
    idToIndex.clear();

    uint32_t cCount = readU32();
    creatures.resize(cCount);

    for (uint32_t i = 0; i < cCount; i++) {
        Creature& c = creatures[i];
        c.alive = true;

        c.id         = readU32();
        c.parentA    = readU32();
        c.parentB    = readU32();
        c.generation = readU32();
        c.speciesID  = readU32();

        c.pos = readVec3();
        c.vel = readVec3();
        c.yaw = readF();

        readFA(c.genome.raw.data(), GENOME_SIZE);

        readFA(c.needs.urgency.data(), DRIVE_COUNT);
        readFA(c.needs.craveRate.data(), DRIVE_COUNT);
        readFA(c.needs.desireMult.data(), DRIVE_COUNT);

        c.energy    = readF();
        c.maxEnergy = readF();
        c.age       = readF();
        c.lifespan  = readF();
        c.mass      = readF();

        c.behavior   = static_cast<BehaviorState>(readU32());
        c.gestTimer  = readF();
        c.mateTarget = readU32();

        // Perception cache: reset to defaults (will be repopulated on next tick)
        c.nearestPredator = INVALID_ID; c.nearestPredDist = 1e9f;
        c.nearestPrey     = INVALID_ID; c.nearestPreyDist = 1e9f;
        c.nearestMate     = INVALID_ID; c.nearestMateDist = 1e9f;
        c.nearestFoodDist = 1e9f;
        c.nearestWaterDist= 1e9f;

        idToIndex[c.id] = i;
    }

    // ── Plants ────────────────────────────────────────────────────────────────
    plants.clear();
    uint32_t pCount = readU32();
    plants.resize(pCount);

    for (uint32_t i = 0; i < pCount; i++) {
        Plant& p   = plants[i];
        p.pos      = readVec3();
        p.nutrition= readF();
        p.growTimer= readF();
        p.alive    = readU8() != 0;
        p.type     = readU8();
    }

    // ── Species ───────────────────────────────────────────────────────────────
    species.clear();
    uint32_t sCount = readU32();
    species.resize(sCount);

    for (uint32_t i = 0; i < sCount; i++) {
        SpeciesInfo& sp = species[i];
        sp.id      = readU32();
        sp.count   = readI32();
        sp.allTime = readI32();
        readFA(sp.color, 3);
        readFA(sp.centroid.raw.data(), GENOME_SIZE);
        uint32_t nlen = readU32();
        sp.name.resize(nlen);
        if (nlen > 0) f.read(&sp.name[0], nlen);
    }

    // Mark all terrain chunks dirty so the renderer rebuilds them on next frame
    // (terrain itself did not change, but chunk mesh cache may be stale)
    for (auto& ch : chunks) ch.dirty = true;

    return f.good();
}

// ── CSV export ────────────────────────────────────────────────────────────────
void World::exportCSV(const char* path) const {
    std::ofstream f(path);
    if (!f) return;
    f << "id,species,x,y,z,age,energy,speed,herbEff,carnEff\n";
    for (const auto& c : creatures) {
        f << c.id << ',' << c.speciesID << ','
          << c.pos.x << ',' << c.pos.y << ',' << c.pos.z << ','
          << c.age << ',' << c.energy << ','
          << c.genome.maxSpeed() << ','
          << c.genome.herbEfficiency() << ','
          << c.genome.carnEfficiency() << '\n';
    }
}
