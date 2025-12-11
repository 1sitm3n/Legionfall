#include "core/Game.h"
#include "core/JobSystem.h"
#include <cmath>
#include <algorithm>
#include <chrono>
#include <random>

namespace Legionfall {

void Game::init(uint32_t enemyCount) {
    m_hero = Hero{};
    m_hero.x = 0.0f;
    m_hero.y = 0.0f;
    m_hero.speed = 8.0f;
    m_hero.pulsePhase = 0.0f;

    m_enemies.clear();
    spawnEnemiesInGrid(enemyCount);
    rebuildInstances();

    m_stats.enemyCount = enemyCount;
    m_stats.aliveCount = enemyCount;
    m_stats.parallelEnabled = m_parallelEnabled;
    m_stats.heavyWorkEnabled = m_heavyWorkEnabled;
    m_stats.cameraFollowEnabled = m_cameraFollowEnabled;
    m_time = 0.0f;
}

void Game::update(float dt, const InputState& input, JobSystem* jobs) {
    // Handle toggle inputs (edge detection)
    if (input.toggleParallel && !m_toggleParallelPressed) {
        m_parallelEnabled = !m_parallelEnabled;
        m_stats.parallelEnabled = m_parallelEnabled;
    }
    m_toggleParallelPressed = input.toggleParallel;
    
    if (input.toggleHeavyWork && !m_toggleHeavyPressed) {
        m_heavyWorkEnabled = !m_heavyWorkEnabled;
        m_stats.heavyWorkEnabled = m_heavyWorkEnabled;
    }
    m_toggleHeavyPressed = input.toggleHeavyWork;
    
    if (input.toggleCameraFollow && !m_toggleCameraPressed) {
        m_cameraFollowEnabled = !m_cameraFollowEnabled;
        m_stats.cameraFollowEnabled = m_cameraFollowEnabled;
    }
    m_toggleCameraPressed = input.toggleCameraFollow;

    m_time += dt;
    
    updateHero(dt, input);
    
    // Time the enemy update
    auto startUpdate = std::chrono::high_resolution_clock::now();
    
    if (m_parallelEnabled && jobs != nullptr && jobs->threadCount() > 0) {
        updateEnemiesParallel(dt, jobs);
    } else {
        updateEnemiesSingleThreaded(dt);
    }
    
    auto endUpdate = std::chrono::high_resolution_clock::now();
    m_stats.updateTimeMs = std::chrono::duration<double, std::milli>(endUpdate - startUpdate).count();
    
    // Update stats
    m_stats.heroX = m_hero.x;
    m_stats.heroY = m_hero.y;
    
    rebuildInstances();
}

void Game::updateHero(float dt, const InputState& input) {
    // Update pulse phase for visual effect
    m_hero.pulsePhase += dt * 4.0f;
    if (m_hero.pulsePhase > 6.28318f) {
        m_hero.pulsePhase -= 6.28318f;
    }
    
    // Update attack cooldown
    if (m_hero.attackCooldown > 0.0f) {
        m_hero.attackCooldown -= dt;
    }
    
    // Handle attack input (will be used in Phase 6)
    m_hero.attackTriggered = false;
    if (input.attack && m_hero.attackCooldown <= 0.0f) {
        m_hero.attackTriggered = true;
        m_hero.attackCooldown = m_hero.attackCooldownMax;
    }
    
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
    
    // Store velocity for potential use
    m_hero.velX = vx;
    m_hero.velY = vy;

    // Apply velocity
    m_hero.x += vx * dt;
    m_hero.y += vy * dt;
    
    // Clamp to arena bounds
    m_hero.x = std::clamp(m_hero.x, -ARENA_HALF + 0.5f, ARENA_HALF - 0.5f);
    m_hero.y = std::clamp(m_hero.y, -ARENA_HALF + 0.5f, ARENA_HALF - 0.5f);
}

void Game::updateEnemiesSingleThreaded(float dt) {
    updateEnemySlice(0, m_enemies.size(), dt);
}

void Game::updateEnemiesParallel(float dt, JobSystem* jobs) {
    size_t enemyCount = m_enemies.size();
    if (enemyCount == 0) return;
    
    size_t numJobs = std::min(jobs->threadCount(), size_t(8));
    numJobs = std::max(numJobs, size_t(1));
    
    if (enemyCount < numJobs * 50) {
        updateEnemiesSingleThreaded(dt);
        return;
    }
    
    size_t perJob = enemyCount / numJobs;
    size_t remainder = enemyCount % numJobs;
    
    float currentTime = m_time;
    bool heavyWork = m_heavyWorkEnabled;
    
    size_t start = 0;
    for (size_t i = 0; i < numJobs; ++i) {
        size_t count = perJob + (i < remainder ? 1 : 0);
        size_t end = start + count;
        
        jobs->schedule([this, start, end, currentTime, heavyWork]() {
            for (size_t j = start; j < end; ++j) {
                Enemy& e = m_enemies[j];
                if (!e.alive) continue;
                
                float waveX = std::sin(currentTime * 1.5f + e.phase) * 0.3f;
                float waveY = std::cos(currentTime * 2.0f + e.phase * 1.3f) * 0.3f;
                
                e.x = e.baseX + waveX * e.speed;
                e.y = e.baseY + waveY * e.speed;
                
                if (heavyWork) {
                    float result = 0.0f;
                    for (int k = 0; k < 50; ++k) {
                        result += std::sin(e.x * (float)k * 0.1f) * std::cos(e.y * (float)k * 0.1f);
                        result = std::tanh(result);
                    }
                    e.x += result * 0.0001f;
                }
                
                e.x = std::clamp(e.x, -ARENA_HALF, ARENA_HALF);
                e.y = std::clamp(e.y, -ARENA_HALF, ARENA_HALF);
            }
        });
        
        start = end;
    }
    
    jobs->wait();
}

void Game::updateEnemySlice(size_t start, size_t end, float dt) {
    (void)dt;
    
    for (size_t i = start; i < end; ++i) {
        Enemy& e = m_enemies[i];
        if (!e.alive) continue;
        
        float waveX = std::sin(m_time * 1.5f + e.phase) * 0.3f;
        float waveY = std::cos(m_time * 2.0f + e.phase * 1.3f) * 0.3f;
        
        e.x = e.baseX + waveX * e.speed;
        e.y = e.baseY + waveY * e.speed;
        
        if (m_heavyWorkEnabled) {
            float result = doHeavyWork(e.x, e.y);
            e.x += result * 0.0001f;
        }
        
        e.x = std::clamp(e.x, -ARENA_HALF, ARENA_HALF);
        e.y = std::clamp(e.y, -ARENA_HALF, ARENA_HALF);
    }
}

float Game::doHeavyWork(float x, float y) {
    float result = 0.0f;
    for (int i = 0; i < 50; ++i) {
        result += std::sin(x * (float)i * 0.1f) * std::cos(y * (float)i * 0.1f);
        result = std::tanh(result);
    }
    return result;
}

void Game::rebuildInstances() {
    m_instances.clear();
    
    uint32_t aliveCount = 0;
    for (const auto& e : m_enemies) {
        if (e.alive) aliveCount++;
    }
    m_instances.reserve(1 + aliveCount);

    // === HERO === (Instance 0)
    // Pulsing cyan/white color, larger size
    float pulse = std::sin(m_hero.pulsePhase) * 0.5f + 0.5f; // 0 to 1
    float heroScale = 0.55f + pulse * 0.1f; // Pulsing size
    
    InstanceData hero{};
    hero.offsetX = m_hero.x;
    hero.offsetY = m_hero.y;
    
    // Bright cyan that pulses to white
    hero.colorR = 0.3f + pulse * 0.7f;
    hero.colorG = 0.8f + pulse * 0.2f;
    hero.colorB = 1.0f;
    hero.scale = heroScale;
    m_instances.push_back(hero);

    // === ENEMIES ===
    for (const auto& e : m_enemies) {
        if (!e.alive) continue;
        
        InstanceData inst{};
        inst.offsetX = e.x;
        inst.offsetY = e.y;
        
        // Color gradient based on position (red to orange)
        float t = (e.baseX + ARENA_HALF) / (2.0f * ARENA_HALF);
        inst.colorR = 1.0f;
        inst.colorG = 0.2f + t * 0.3f;
        inst.colorB = 0.1f;
        inst.scale = 0.18f;
        m_instances.push_back(inst);
    }

    m_stats.aliveCount = aliveCount;
}

void Game::spawnEnemiesInGrid(uint32_t count) {
    m_enemies.reserve(count);
    
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> phaseDist(0.0f, 6.28318f);
    std::uniform_real_distribution<float> speedDist(0.5f, 1.5f);
    
    uint32_t gridSize = (uint32_t)std::ceil(std::sqrt((double)count));
    float spacing = (ARENA_HALF * 2.0f - 2.0f) / (float)(gridSize);
    float startX = -ARENA_HALF + 1.0f + spacing * 0.5f;
    float startY = -ARENA_HALF + 1.0f + spacing * 0.5f;

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t col = i % gridSize;
        uint32_t row = i / gridSize;
        
        Enemy e{};
        e.baseX = startX + col * spacing;
        e.baseY = startY + row * spacing;
        e.x = e.baseX;
        e.y = e.baseY;
        e.phase = phaseDist(rng);
        e.speed = speedDist(rng);
        e.alive = true;
        
        // Clear area around hero spawn
        float distSq = e.baseX * e.baseX + e.baseY * e.baseY;
        if (distSq < 6.0f) {
            e.alive = false;
        }
        
        m_enemies.push_back(e);
    }
}

}