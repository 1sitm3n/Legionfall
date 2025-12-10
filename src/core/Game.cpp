#include "core/Game.h"
#include "core/JobSystem.h"
#include <cmath>

namespace Legionfall {

void Game::init(uint32_t enemyCount) {
    m_hero = Hero{};
    m_enemies.clear();
    m_enemies.reserve(enemyCount);
    
    uint32_t grid = (uint32_t)std::ceil(std::sqrt((double)enemyCount));
    float spacing = (ARENA_MAX - ARENA_MIN) / (grid + 1);
    
    for (uint32_t i = 0; i < enemyCount; ++i) {
        Enemy e{};
        e.x = ARENA_MIN + spacing * ((i % grid) + 1);
        e.y = ARENA_MIN + spacing * ((i / grid) + 1);
        e.alive = true;
        m_enemies.push_back(e);
    }
    rebuildInstances();
    m_stats.enemyCount = enemyCount;
}

void Game::update(float dt, const InputState& input, JobSystem* jobs) {
    (void)jobs;
    m_time += dt;
    updateHero(dt, input);
    rebuildInstances();
}

void Game::updateHero(float dt, const InputState& input) {
    m_hero.velX = m_hero.velY = 0.0f;
    if (input.moveUp) m_hero.velY += 1.0f;
    if (input.moveDown) m_hero.velY -= 1.0f;
    if (input.moveRight) m_hero.velX += 1.0f;
    if (input.moveLeft) m_hero.velX -= 1.0f;
    
    float len = std::sqrt(m_hero.velX * m_hero.velX + m_hero.velY * m_hero.velY);
    if (len > 0.0f) {
        m_hero.velX = (m_hero.velX / len) * m_hero.speed;
        m_hero.velY = (m_hero.velY / len) * m_hero.speed;
    }
    
    m_hero.x += m_hero.velX * dt;
    m_hero.y += m_hero.velY * dt;
    m_hero.x = std::fmax(ARENA_MIN, std::fmin(ARENA_MAX, m_hero.x));
    m_hero.y = std::fmax(ARENA_MIN, std::fmin(ARENA_MAX, m_hero.y));
}

void Game::rebuildInstances() {
    m_instances.clear();
    m_instances.reserve(1 + m_enemies.size());
    
    InstanceData hero{};
    hero.offsetX = m_hero.x; hero.offsetY = m_hero.y;
    hero.colorR = 0.2f; hero.colorG = 0.4f; hero.colorB = 1.0f;
    hero.scale = 0.4f;
    m_instances.push_back(hero);
    
    for (const auto& e : m_enemies) {
        if (e.alive) {
            InstanceData inst{};
            inst.offsetX = e.x; inst.offsetY = e.y;
            inst.colorR = 1.0f; inst.colorG = 0.2f; inst.colorB = 0.2f;
            inst.scale = 0.15f;
            m_instances.push_back(inst);
        }
    }
    m_stats.enemyCount = (uint32_t)(m_instances.size() - 1);
}

}
