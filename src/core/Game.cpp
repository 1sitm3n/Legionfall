#include "core/Game.h"
#include "core/JobSystem.h"
#include <cmath>
#include <algorithm>

namespace Legionfall {

void Game::init(uint32_t enemyCount) {
    // Initialize hero at center
    m_hero = Hero{};
    m_hero.x = 0.0f;
    m_hero.y = 0.0f;
    m_hero.speed = 8.0f;

    // Spawn enemies in a grid
    m_enemies.clear();
    spawnEnemiesInGrid(enemyCount);

    // Build initial instance buffer
    rebuildInstances();

    m_stats.enemyCount = enemyCount;
    m_stats.aliveCount = enemyCount;
    m_time = 0.0f;
}

void Game::update(float dt, const InputState& input, JobSystem* jobs) {
    (void)jobs; // Will use in Phase 3
    
    m_time += dt;
    
    updateHero(dt, input);
    updateEnemies(dt);
    rebuildInstances();
}

void Game::updateHero(float dt, const InputState& input) {
    // Calculate velocity from input
    float vx = 0.0f, vy = 0.0f;
    
    if (input.moveUp)    vy += 1.0f;
    if (input.moveDown)  vy -= 1.0f;
    if (input.moveRight) vx += 1.0f;
    if (input.moveLeft)  vx -= 1.0f;

    // Normalize diagonal movement
    float len = std::sqrt(vx * vx + vy * vy);
    if (len > 0.0f) {
        vx = (vx / len) * m_hero.speed;
        vy = (vy / len) * m_hero.speed;
    }

    // Apply velocity
    m_hero.x += vx * dt;
    m_hero.y += vy * dt;

    // Clamp to arena
    m_hero.x = std::clamp(m_hero.x, -ARENA_HALF, ARENA_HALF);
    m_hero.y = std::clamp(m_hero.y, -ARENA_HALF, ARENA_HALF);
}

void Game::updateEnemies(float dt) {
    // Phase 2: Simple wave motion to show they're alive
    for (auto& e : m_enemies) {
        if (!e.alive) continue;
        
        // Gentle sine wave motion
        float baseX = e.x;
        float wave = std::sin(m_time * 2.0f + baseX * 0.5f) * 0.02f;
        e.y += wave;
        
        // Keep in bounds
        e.y = std::clamp(e.y, -ARENA_HALF, ARENA_HALF);
    }
}

void Game::rebuildInstances() {
    m_instances.clear();
    
    // Count alive enemies + 1 for hero
    uint32_t aliveCount = 0;
    for (const auto& e : m_enemies) {
        if (e.alive) aliveCount++;
    }
    m_instances.reserve(1 + aliveCount);

    // Hero is instance 0 - Blue, larger
    InstanceData hero{};
    hero.offsetX = m_hero.x;
    hero.offsetY = m_hero.y;
    hero.colorR = 0.2f;
    hero.colorG = 0.5f;
    hero.colorB = 1.0f;
    hero.scale = 0.5f;
    m_instances.push_back(hero);

    // Enemies - Red/orange, smaller
    for (const auto& e : m_enemies) {
        if (!e.alive) continue;
        
        InstanceData inst{};
        inst.offsetX = e.x;
        inst.offsetY = e.y;
        inst.colorR = 1.0f;
        inst.colorG = 0.3f;
        inst.colorB = 0.1f;
        inst.scale = 0.2f;
        m_instances.push_back(inst);
    }

    m_stats.aliveCount = aliveCount;
}

void Game::spawnEnemiesInGrid(uint32_t count) {
    m_enemies.reserve(count);
    
    // Calculate grid dimensions
    uint32_t gridSize = (uint32_t)std::ceil(std::sqrt((double)count));
    float spacing = (ARENA_HALF * 2.0f - 2.0f) / (float)(gridSize);
    float startX = -ARENA_HALF + 1.0f + spacing * 0.5f;
    float startY = -ARENA_HALF + 1.0f + spacing * 0.5f;

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t col = i % gridSize;
        uint32_t row = i / gridSize;
        
        Enemy e{};
        e.x = startX + col * spacing;
        e.y = startY + row * spacing;
        e.velX = 0.0f;
        e.velY = 0.0f;
        e.alive = true;
        
        // Skip if too close to hero spawn (center)
        float distSq = e.x * e.x + e.y * e.y;
        if (distSq < 4.0f) {
            e.alive = false;
        }
        
        m_enemies.push_back(e);
    }
}

}