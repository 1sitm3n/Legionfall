#pragma once
#include <vector>
#include <cstdint>
#include <chrono>

namespace Legionfall {

class JobSystem;

// Per-instance GPU data
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
    float radius = 0.35f;
    
    // Combat
    float attackRadius = 3.5f;
    float attackCooldown = 0.0f;
    float attackCooldownMax = 0.4f;
    bool attackTriggered = false;
    
    // Shockwave visual effect
    float shockwaveRadius = 0.0f;
    float shockwaveAlpha = 0.0f;
    
    // Stats
    int health = 100;
    int maxHealth = 100;
    int killCount = 0;
    
    // Visual
    float pulsePhase = 0.0f;
    float damageFlash = 0.0f;
};

struct Enemy {
    float x, y;
    float baseX, baseY;
    float phase;
    float speed;
    float chaseSpeed;
    bool alive;
    
    // Death animation
    float deathTimer;
    float deathX, deathY;  // Position at death for respawn reference
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
    uint32_t killCount = 0;
    int heroHealth = 100;
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
    float getShockwaveRadius() const { return m_hero.shockwaveRadius; }
    float getShockwaveAlpha() const { return m_hero.shockwaveAlpha; }

private:
    void updateHero(float dt, const InputState& input);
    void performAttack();
    void updateEnemiesSingleThreaded(float dt);
    void updateEnemiesParallel(float dt, JobSystem* jobs);
    void checkCollisions();
    void respawnEnemy(Enemy& e);
    void rebuildInstances();
    void spawnEnemiesInGrid(uint32_t count);
    float doHeavyWork(float x, float y);

    Hero m_hero;
    std::vector<Enemy> m_enemies;
    std::vector<InstanceData> m_instances;
    ProfilingStats m_stats;
    
    float m_time = 0.0f;
    uint32_t m_initialEnemyCount = 0;
    
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
    
    // Respawn delay
    static constexpr float RESPAWN_DELAY = 2.0f;
};

}