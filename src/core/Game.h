#pragma once
#include <vector>
#include <cstdint>
#include <chrono>
#include <atomic>

namespace Legionfall {

class JobSystem;

// Per-instance GPU data - must match shader layout
struct InstanceData {
    float offsetX, offsetY;
    float colorR, colorG, colorB;
    float scale;
    float padding[2];
};

struct Hero {
    float x = 0.0f, y = 0.0f;
    float velX = 0.0f, velY = 0.0f;
    float speed = 8.0f;
    float radius = 0.4f;
    
    // Combat
    float attackRadius = 3.0f;
    float attackCooldown = 0.0f;
    float attackCooldownMax = 0.5f;
    bool attackTriggered = false;
    
    // Visual effects
    float pulsePhase = 0.0f;
};

struct Enemy {
    float x, y;
    float baseX, baseY;   // Original spawn (for respawn)
    float phase;          // Random phase for variety
    float speed;          // Movement speed
    float chaseSpeed;     // Speed when chasing hero
    bool alive;
};

struct InputState {
    bool moveUp = false, moveDown = false;
    bool moveLeft = false, moveRight = false;
    bool attack = false;
    bool toggleParallel = false;
    bool toggleHeavyWork = false;
    bool toggleCameraFollow = false;
    bool toggleChaseMode = false;
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
    bool cameraFollowEnabled = false;
    bool chaseModeEnabled = true;
    float heroX = 0.0f, heroY = 0.0f;
};

class Game {
public:
    void init(uint32_t enemyCount);
    void update(float dt, const InputState& input, JobSystem* jobs);
    
    const std::vector<InstanceData>& getInstanceData() const { return m_instances; }
    const ProfilingStats& getStats() const { return m_stats; }
    void getHeroPosition(float& x, float& y) const { x = m_hero.x; y = m_hero.y; }
    bool isCameraFollowEnabled() const { return m_cameraFollowEnabled; }

private:
    void updateHero(float dt, const InputState& input);
    void updateEnemiesSingleThreaded(float dt);
    void updateEnemiesParallel(float dt, JobSystem* jobs);
    void rebuildInstances();
    void spawnEnemiesInGrid(uint32_t count);
    float doHeavyWork(float x, float y);

    Hero m_hero;
    std::vector<Enemy> m_enemies;
    std::vector<InstanceData> m_instances;
    ProfilingStats m_stats;
    
    float m_time = 0.0f;
    
    // Toggle states
    bool m_parallelEnabled = true;
    bool m_heavyWorkEnabled = false;
    bool m_cameraFollowEnabled = false;
    bool m_chaseModeEnabled = true;
    bool m_toggleParallelPressed = false;
    bool m_toggleHeavyPressed = false;
    bool m_toggleCameraPressed = false;
    bool m_toggleChasePressed = false;
    
    // Arena bounds
    static constexpr float ARENA_HALF = 10.0f;
};

}