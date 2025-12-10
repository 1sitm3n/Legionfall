#pragma once
#include <vector>
#include <cstdint>

namespace Legionfall {

class JobSystem;

struct InstanceData {
    float offsetX, offsetY;
    float colorR, colorG, colorB;
    float scale;
    float padding[2];
};

struct Hero {
    float x = 0.0f, y = 0.0f;
    float velX = 0.0f, velY = 0.0f;
    float speed = 5.0f;
};

struct Enemy {
    float x, y, velX, velY;
    bool alive;
};

struct InputState {
    bool moveUp = false, moveDown = false, moveLeft = false, moveRight = false;
    bool attack = false, toggleParallel = false, toggleHeavyWork = false;
};

struct ProfilingStats {
    double fps = 0.0;
    uint32_t enemyCount = 0;
    bool parallelEnabled = true;
    bool heavyWorkEnabled = false;
};

class Game {
public:
    void init(uint32_t enemyCount);
    void update(float dt, const InputState& input, JobSystem* jobs);
    const std::vector<InstanceData>& getInstanceData() const { return m_instances; }
    const ProfilingStats& getStats() const { return m_stats; }

private:
    void updateHero(float dt, const InputState& input);
    void rebuildInstances();
    
    Hero m_hero;
    std::vector<Enemy> m_enemies;
    std::vector<InstanceData> m_instances;
    ProfilingStats m_stats;
    float m_time = 0.0f;
    static constexpr float ARENA_MIN = -10.0f;
    static constexpr float ARENA_MAX = 10.0f;
};

}
