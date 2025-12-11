#include "core/Game.h"
#include "core/JobSystem.h"
#include <cmath>
#include <algorithm>
#include <chrono>
#include <random>

namespace Legionfall {

// Random number generator for respawning
static std::mt19937 s_rng(12345);

void Game::init(uint32_t enemyCount) {
    m_hero = Hero{};
    m_hero.x = 0.0f;
    m_hero.y = 0.0f;
    m_hero.speed = 8.0f;
    m_hero.health = 100;
    m_hero.maxHealth = 100;
    m_hero.killCount = 0;
    m_hero.pulsePhase = 0.0f;

    m_enemies.clear();
    m_initialEnemyCount = enemyCount;
    spawnEnemiesInGrid(enemyCount);
    rebuildInstances();

    m_stats.enemyCount = enemyCount;
    m_stats.aliveCount = enemyCount;
    m_stats.killCount = 0;
    m_stats.heroHealth = m_hero.health;
    m_stats.parallelEnabled = m_parallelEnabled;
    m_stats.heavyWorkEnabled = m_heavyWorkEnabled;
    m_stats.cameraFollowEnabled = m_cameraFollowEnabled;
    m_stats.chaseModeEnabled = m_chaseModeEnabled;
    m_time = 0.0f;
}

void Game::update(float dt, const InputState& input, JobSystem* jobs) {
    // Handle toggle inputs
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
    
    if (input.toggleChaseMode && !m_toggleChasePressed) {
        m_chaseModeEnabled = !m_chaseModeEnabled;
        m_stats.chaseModeEnabled = m_chaseModeEnabled;
    }
    m_toggleChasePressed = input.toggleChaseMode;

    m_time += dt;
    
    updateHero(dt, input);
    
    // Time enemy updates
    auto startUpdate = std::chrono::high_resolution_clock::now();
    
    if (m_parallelEnabled && jobs != nullptr && jobs->threadCount() > 0) {
        updateEnemiesParallel(dt, jobs);
    } else {
        updateEnemiesSingleThreaded(dt);
    }
    
    auto endUpdate = std::chrono::high_resolution_clock::now();
    m_stats.updateTimeMs = std::chrono::duration<double, std::milli>(endUpdate - startUpdate).count();
    
    // Check hero-enemy collisions
    checkCollisions();
    
    // Update stats
    m_stats.heroX = m_hero.x;
    m_stats.heroY = m_hero.y;
    m_stats.killCount = m_hero.killCount;
    m_stats.heroHealth = m_hero.health;
    
    rebuildInstances();
}

void Game::updateHero(float dt, const InputState& input) {
    // Update pulse phase
    m_hero.pulsePhase += dt * 4.0f;
    if (m_hero.pulsePhase > 6.28318f) {
        m_hero.pulsePhase -= 6.28318f;
    }
    
    // Update damage flash
    if (m_hero.damageFlash > 0.0f) {
        m_hero.damageFlash -= dt * 4.0f;
        if (m_hero.damageFlash < 0.0f) m_hero.damageFlash = 0.0f;
    }
    
    // Update shockwave effect
    if (m_hero.shockwaveAlpha > 0.0f) {
        m_hero.shockwaveRadius += dt * 15.0f;
        m_hero.shockwaveAlpha -= dt * 3.0f;
        if (m_hero.shockwaveAlpha < 0.0f) {
            m_hero.shockwaveAlpha = 0.0f;
            m_hero.shockwaveRadius = 0.0f;
        }
    }
    
    // Update attack cooldown
    if (m_hero.attackCooldown > 0.0f) {
        m_hero.attackCooldown -= dt;
    }
    
    // Handle attack input
    m_hero.attackTriggered = false;
    if (input.attack && m_hero.attackCooldown <= 0.0f) {
        m_hero.attackTriggered = true;
        m_hero.attackCooldown = m_hero.attackCooldownMax;
        performAttack();
    }
    
    // Calculate velocity from input
    float vx = 0.0f, vy = 0.0f;
    
    if (input.moveUp)    vy += 1.0f;
    if (input.moveDown)  vy -= 1.0f;
    if (input.moveRight) vx += 1.0f;
    if (input.moveLeft)  vx -= 1.0f;

    float len = std::sqrt(vx * vx + vy * vy);
    if (len > 0.0f) {
        vx = (vx / len) * m_hero.speed;
        vy = (vy / len) * m_hero.speed;
    }
    
    m_hero.velX = vx;
    m_hero.velY = vy;

    m_hero.x += vx * dt;
    m_hero.y += vy * dt;
    
    m_hero.x = std::clamp(m_hero.x, -ARENA_HALF + 0.5f, ARENA_HALF - 0.5f);
    m_hero.y = std::clamp(m_hero.y, -ARENA_HALF + 0.5f, ARENA_HALF - 0.5f);
}

void Game::performAttack() {
    // Start shockwave visual
    m_hero.shockwaveRadius = 0.5f;
    m_hero.shockwaveAlpha = 1.0f;
    
    // Kill enemies within attack radius
    float attackRadiusSq = m_hero.attackRadius * m_hero.attackRadius;
    
    for (auto& e : m_enemies) {
        if (!e.alive) continue;
        
        float dx = e.x - m_hero.x;
        float dy = e.y - m_hero.y;
        float distSq = dx * dx + dy * dy;
        
        if (distSq < attackRadiusSq) {
            e.alive = false;
            e.deathTimer = RESPAWN_DELAY;
            e.deathX = e.x;
            e.deathY = e.y;
            m_hero.killCount++;
        }
    }
}

void Game::checkCollisions() {
    if (!m_chaseModeEnabled) return;  // No damage in wave mode
    
    float heroRadiusSq = m_hero.radius * m_hero.radius;
    
    for (auto& e : m_enemies) {
        if (!e.alive) continue;
        
        float dx = e.x - m_hero.x;
        float dy = e.y - m_hero.y;
        float distSq = dx * dx + dy * dy;
        
        // Collision!
        if (distSq < heroRadiusSq) {
            m_hero.health -= 1;  // Lose 1 health per frame of contact
            m_hero.damageFlash = 1.0f;
            
            // Push enemy back slightly
            float dist = std::sqrt(distSq);
            if (dist > 0.01f) {
                e.x += (dx / dist) * 0.5f;
                e.y += (dy / dist) * 0.5f;
            }
        }
    }
    
    // Clamp health
    if (m_hero.health < 0) m_hero.health = 0;
}

void Game::respawnEnemy(Enemy& e) {
    std::uniform_real_distribution<float> edgeDist(-ARENA_HALF + 0.5f, ARENA_HALF - 0.5f);
    std::uniform_int_distribution<int> sideDist(0, 3);
    std::uniform_real_distribution<float> phaseDist(0.0f, 6.28318f);
    std::uniform_real_distribution<float> chaseSpeedDist(1.5f, 4.0f);
    
    int side = sideDist(s_rng);
    float pos = edgeDist(s_rng);
    
    switch (side) {
        case 0: e.x = -ARENA_HALF + 0.2f; e.y = pos; break;  // Left
        case 1: e.x = ARENA_HALF - 0.2f;  e.y = pos; break;  // Right
        case 2: e.x = pos; e.y = -ARENA_HALF + 0.2f; break;  // Bottom
        case 3: e.x = pos; e.y = ARENA_HALF - 0.2f;  break;  // Top
    }
    
    e.baseX = e.x;
    e.baseY = e.y;
    e.phase = phaseDist(s_rng);
    e.chaseSpeed = chaseSpeedDist(s_rng);
    e.alive = true;
    e.deathTimer = 0.0f;
}

void Game::updateEnemiesSingleThreaded(float dt) {
    float heroX = m_hero.x;
    float heroY = m_hero.y;
    float currentTime = m_time;
    bool chaseMode = m_chaseModeEnabled;
    bool heavyWork = m_heavyWorkEnabled;
    
    for (size_t i = 0; i < m_enemies.size(); ++i) {
        Enemy& e = m_enemies[i];
        
        // Handle dead enemies (respawn timer)
        if (!e.alive) {
            e.deathTimer -= dt;
            if (e.deathTimer <= 0.0f) {
                respawnEnemy(e);
            }
            continue;
        }
        
        if (chaseMode) {
            float dx = heroX - e.x;
            float dy = heroY - e.y;
            float dist = std::sqrt(dx * dx + dy * dy);
            
            if (dist > 0.1f) {
                dx /= dist;
                dy /= dist;
                
                float wobble = std::sin(currentTime * 3.0f + e.phase * 2.0f) * 0.3f;
                dx += std::cos(e.phase + currentTime) * wobble * 0.5f;
                dy += std::sin(e.phase + currentTime) * wobble * 0.5f;
                
                float wobbleLen = std::sqrt(dx * dx + dy * dy);
                if (wobbleLen > 0.0f) {
                    dx /= wobbleLen;
                    dy /= wobbleLen;
                }
                
                e.x += dx * e.chaseSpeed * dt;
                e.y += dy * e.chaseSpeed * dt;
            }
        } else {
            float waveX = std::sin(currentTime * 1.5f + e.phase) * 0.3f;
            float waveY = std::cos(currentTime * 2.0f + e.phase * 1.3f) * 0.3f;
            e.x = e.baseX + waveX * e.speed;
            e.y = e.baseY + waveY * e.speed;
        }
        
        if (heavyWork) {
            float result = doHeavyWork(e.x, e.y);
            e.x += result * 0.0001f;
        }
        
        e.x = std::clamp(e.x, -ARENA_HALF, ARENA_HALF);
        e.y = std::clamp(e.y, -ARENA_HALF, ARENA_HALF);
    }
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
    
    // First pass: update alive enemies (parallel-safe)
    size_t perJob = enemyCount / numJobs;
    size_t remainder = enemyCount % numJobs;
    
    float heroX = m_hero.x;
    float heroY = m_hero.y;
    float currentTime = m_time;
    bool chaseMode = m_chaseModeEnabled;
    bool heavyWork = m_heavyWorkEnabled;
    float deltaTime = dt;
    
    size_t start = 0;
    for (size_t i = 0; i < numJobs; ++i) {
        size_t count = perJob + (i < remainder ? 1 : 0);
        size_t end = start + count;
        
        jobs->schedule([this, start, end, heroX, heroY, currentTime, chaseMode, heavyWork, deltaTime]() {
            for (size_t j = start; j < end; ++j) {
                Enemy& e = m_enemies[j];
                
                // Skip dead enemies in parallel (handle respawn in main thread)
                if (!e.alive) continue;
                
                if (chaseMode) {
                    float dx = heroX - e.x;
                    float dy = heroY - e.y;
                    float dist = std::sqrt(dx * dx + dy * dy);
                    
                    if (dist > 0.1f) {
                        dx /= dist;
                        dy /= dist;
                        
                        float wobble = std::sin(currentTime * 3.0f + e.phase * 2.0f) * 0.3f;
                        dx += std::cos(e.phase + currentTime) * wobble * 0.5f;
                        dy += std::sin(e.phase + currentTime) * wobble * 0.5f;
                        
                        float wobbleLen = std::sqrt(dx * dx + dy * dy);
                        if (wobbleLen > 0.0f) {
                            dx /= wobbleLen;
                            dy /= wobbleLen;
                        }
                        
                        e.x += dx * e.chaseSpeed * deltaTime;
                        e.y += dy * e.chaseSpeed * deltaTime;
                    }
                } else {
                    float waveX = std::sin(currentTime * 1.5f + e.phase) * 0.3f;
                    float waveY = std::cos(currentTime * 2.0f + e.phase * 1.3f) * 0.3f;
                    e.x = e.baseX + waveX * e.speed;
                    e.y = e.baseY + waveY * e.speed;
                }
                
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
    
    // Second pass: handle dead enemy respawns (single-threaded, not performance critical)
    for (auto& e : m_enemies) {
        if (!e.alive) {
            e.deathTimer -= dt;
            if (e.deathTimer <= 0.0f) {
                respawnEnemy(e);
            }
        }
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

    // === HERO ===
    float pulse = std::sin(m_hero.pulsePhase) * 0.5f + 0.5f;
    float heroScale = 0.55f + pulse * 0.1f;
    
    // Flash when attacking or damaged
    float attackFlash = (m_hero.shockwaveAlpha > 0.5f) ? 1.0f : 0.0f;
    float damageFlash = m_hero.damageFlash;
    
    InstanceData hero{};
    hero.offsetX = m_hero.x;
    hero.offsetY = m_hero.y;
    
    if (damageFlash > 0.0f) {
        // Flash red when damaged
        hero.colorR = 1.0f;
        hero.colorG = 0.2f;
        hero.colorB = 0.2f;
    } else {
        hero.colorR = 0.3f + pulse * 0.4f + attackFlash * 0.5f;
        hero.colorG = 0.8f + pulse * 0.2f + attackFlash * 0.2f;
        hero.colorB = 1.0f;
    }
    hero.scale = heroScale + attackFlash * 0.3f;
    m_instances.push_back(hero);

    // === ENEMIES ===
    float heroX = m_hero.x;
    float heroY = m_hero.y;
    
    for (const auto& e : m_enemies) {
        if (!e.alive) continue;
        
        InstanceData inst{};
        inst.offsetX = e.x;
        inst.offsetY = e.y;
        
        float dx = e.x - heroX;
        float dy = e.y - heroY;
        float dist = std::sqrt(dx * dx + dy * dy);
        float proximity = 1.0f - std::clamp(dist / 8.0f, 0.0f, 1.0f);
        
        inst.colorR = 0.8f + proximity * 0.2f;
        inst.colorG = 0.3f - proximity * 0.2f;
        inst.colorB = 0.1f;
        inst.scale = 0.18f + proximity * 0.05f;
        m_instances.push_back(inst);
    }

    m_stats.aliveCount = aliveCount;
}

void Game::spawnEnemiesInGrid(uint32_t count) {
    m_enemies.reserve(count);
    
    std::uniform_real_distribution<float> phaseDist(0.0f, 6.28318f);
    std::uniform_real_distribution<float> speedDist(0.5f, 1.5f);
    std::uniform_real_distribution<float> chaseSpeedDist(1.5f, 4.0f);
    
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
        e.phase = phaseDist(s_rng);
        e.speed = speedDist(s_rng);
        e.chaseSpeed = chaseSpeedDist(s_rng);
        e.alive = true;
        e.deathTimer = 0.0f;
        
        float distSq = e.baseX * e.baseX + e.baseY * e.baseY;
        if (distSq < 6.0f) {
            e.alive = false;
            e.deathTimer = 0.5f;  // Respawn quickly
        }
        
        m_enemies.push_back(e);
    }
}

}