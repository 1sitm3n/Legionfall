#pragma once
#include <vector>
#include <cstdint>
#include <chrono>

namespace Legionfall {

class JobSystem;

// Per-instance GPU data - must match shader layout
struct InstanceData {
    float offsetX, offsetY;       // World position
    float colorR, colorG, colorB; // RGB color
    float scale;                  // Uniform scale
    float padding[2];             // Pad to 32 bytes
};

struct Hero {
    float x = 0.0f, y = 0.0f;
    float velX = 0.0f, velY = 0.0f;
    float speed = 8.0f;
    float radius = 0.3f;
};

struct Enemy {
    float x, y;           // Current position
    float baseX, baseY;   // Original spawn position (for wave motion)
    float phase;          // Phase offset for wave motion
    float speed;          // Movement speed multiplier
    bool alive;
};

struct InputState {
    bool moveUp = false, moveDown = false;
    bool moveLeft = false, moveRight = false;
    bool attack = false;
    bool toggleParallel = false;
    bool toggleHeavyWork = false;
    bool increaseEnemies = false;
    bool decreaseEnemies = false;
};

struct ProfilingStats {
    double fps = 0.0;
    double updateTimeMs = 0.0;
    double frameTimeMs = 0.0;
    uint32_t enemyCount = 0;
    uint32_t aliveCount = 0;
    bool parallelEnabled = true;
    bool heavyWorkEnabled = false;
};

class Game {
public:
    void init(uint32_t enemyCount);
    void update(float dt, const InputState& input, JobSystem* jobs);
    
    const std::vector<InstanceData>& getInstanceData() const { return m_instances; }
    const ProfilingStats& getStats() const { return m_stats; }
    void getHeroPosition(float& x, float& y) const { x = m_hero.x; y = m_hero.y; }

private:
    void updateHero(float dt, const InputState& input);
    void updateEnemiesSingleThreaded(float dt);
    void updateEnemiesParallel(float dt, JobSystem* jobs);
    void updateEnemySlice(size_t start, size_t end, float dt);
    void rebuildInstances();
    void spawnEnemiesInGrid(uint32_t count);
    
    // Heavy work simulation (for profiling)
    float doHeavyWork(float x, float y);

    Hero m_hero;
    std::vector<Enemy> m_enemies;
    std::vector<InstanceData> m_instances;
    ProfilingStats m_stats;
    
    float m_time = 0.0f;
    
    // Toggle states (persist across frames)
    bool m_parallelEnabled = true;
    bool m_heavyWorkEnabled = false;
    bool m_toggleParallelPressed = false;
    bool m_toggleHeavyPressed = false;
    
    // Arena bounds
    static constexpr float ARENA_HALF = 10.0f;
};

}