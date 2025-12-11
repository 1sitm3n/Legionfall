#include "core/Game.h"
#include "core/JobSystem.h"
#include <cmath>
#include <algorithm>
#include <chrono>
#include <random>
#include <iostream>

namespace Legionfall {

static std::mt19937 s_rng(12345);

void Game::init(uint32_t enemyCount) {
    m_initialEnemyCount = enemyCount;
    m_targetEnemyCount = enemyCount;
    
    m_hero = Hero{};
    m_hero.x = 0.0f;
    m_hero.y = 0.0f;
    m_hero.speed = 8.0f;
    m_hero.health = 100;
    m_hero.maxHealth = 100;
    m_hero.killCount = 0;
    m_hero.waveNumber = 1;
    m_hero.pulsePhase = 0.0f;

    m_enemies.clear();
    spawnEnemiesInGrid(enemyCount);
    rebuildInstances();

    m_stats.enemyCount = enemyCount;
    m_stats.parallelEnabled = m_parallelEnabled;
    m_stats.heavyWorkEnabled = m_heavyWorkEnabled;
    m_stats.cameraFollowEnabled = m_cameraFollowEnabled;
    m_stats.chaseModeEnabled = m_chaseModeEnabled;
    m_time = 0.0f;
}

void Game::restart() {
    init(m_targetEnemyCount);
}

void Game::adjustEnemyCount(int delta) {
    int newCount = (int)m_targetEnemyCount + delta;
    newCount = std::clamp(newCount, (int)MIN_ENEMIES, (int)MAX_ENEMIES);
    m_targetEnemyCount = (uint32_t)newCount;
    
    // Reinitialize with new count
    init(m_targetEnemyCount);
    std::cout << "[Game] Enemy count adjusted to: " << m_targetEnemyCount << std::endl;
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
    
    // Handle enemy count adjustment
    if (input.increaseEnemies && !m_increasePressed) {
        adjustEnemyCount(1000);
    }
    m_increasePressed = input.increaseEnemies;
    
    if (input.decreaseEnemies && !m_decreasePressed) {
        adjustEnemyCount(-1000);
    }
    m_decreasePressed = input.decreaseEnemies;

    // Don't update if game over
    if (m_hero.health <= 0) {
        rebuildInstances();
        return;
    }

    m_time += dt;
    
    updateHero(dt, input);
    
    // Time enemy updates
    auto startUpdate = std::chrono::high_resolution_clock::now();
    
    if (m_parallelEnabled && jobs != nullptr && jobs->threadCount() > 0) {
        updateEnemiesParallel(dt, jobs);
        m_stats.threadCount = jobs->threadCount();
    } else {
        updateEnemiesSingleThreaded(dt);
        m_stats.threadCount = 1;
    }
    
    auto endUpdate = std::chrono::high_resolution_clock::now();
    m_stats.updateTimeMs = std::chrono::duration<double, std::milli>(endUpdate - startUpdate).count();
    
    checkCollisions();
    
    // Update stats
    m_stats.heroX = m_hero.x;
    m_stats.heroY = m_hero.y;
    m_stats.killCount = m_hero.killCount;
    m_stats.heroHealth = m_hero.health;
    m_stats.waveNumber = m_hero.waveNumber;
    
    rebuildInstances();
}

void Game::updateHero(float dt, const InputState& input) {
    m_hero.pulsePhase += dt * 4.0f;
    if (m_hero.pulsePhase > 6.28318f) m_hero.pulsePhase -= 6.28318f;
    
    if (m_hero.damageFlash > 0.0f) {
        m_hero.damageFlash -= dt * 4.0f;
        if (m_hero.damageFlash < 0.0f) m_hero.damageFlash = 0.0f;
    }
    
    if (m_hero.shockwaveAlpha > 0.0f) {
        m_hero.shockwaveRadius += dt * 20.0f;
        m_hero.shockwaveAlpha -= dt * 2.5f;
        if (m_hero.shockwaveAlpha < 0.0f) {
            m_hero.shockwaveAlpha = 0.0f;
            m_hero.shockwaveRadius = 0.0f;
        }
    }
    
    if (m_hero.attackCooldown > 0.0f) {
        m_hero.attackCooldown -= dt;
    }
    
    m_hero.attackTriggered = false;
    if (input.attack && m_hero.attackCooldown <= 0.0f) {
        m_hero.attackTriggered = true;
        m_hero.attackCooldown = m_hero.attackCooldownMax;
        performAttack();
    }
    
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
    m_hero.shockwaveRadius = 0.5f;
    m_hero.shockwaveAlpha = 1.0f;
    
    float attackRadiusSq = m_hero.attackRadius * m_hero.attackRadius;
    int killsThisAttack = 0;
    
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
            killsThisAttack++;
        }
    }
    
    // Wave progression: every 100 kills, increase difficulty
    int newWave = (m_hero.killCount / 100) + 1;
    if (newWave > m_hero.waveNumber) {
        m_hero.waveNumber = newWave;
        // Enemies get faster each wave
        for (auto& e : m_enemies) {
            e.chaseSpeed *= 1.05f;
        }
    }
}

void Game::checkCollisions() {
    if (!m_chaseModeEnabled) return;
    
    float heroRadiusSq = m_hero.radius * m_hero.radius;
    
    for (auto& e : m_enemies) {
        if (!e.alive) continue;
        
        float dx = e.x - m_hero.x;
        float dy = e.y - m_hero.y;
        float distSq = dx * dx + dy * dy;
        
        if (distSq < heroRadiusSq) {
            m_hero.health -= 1;
            m_hero.damageFlash = 1.0f;
            
            float dist = std::sqrt(distSq);
            if (dist > 0.01f) {
                e.x += (dx / dist) * 0.5f;
                e.y += (dy / dist) * 0.5f;
            }
        }
    }
    
    if (m_hero.health < 0) m_hero.health = 0;
}

void Game::respawnEnemy(Enemy& e) {
    std::uniform_real_distribution<float> edgeDist(-ARENA_HALF + 0.5f, ARENA_HALF - 0.5f);
    std::uniform_int_distribution<int> sideDist(0, 3);
    std::uniform_real_distribution<float> phaseDist(0.0f, 6.28318f);
    
    // Chase speed increases with wave
    float baseSpeed = 1.5f + m_hero.waveNumber * 0.2f;
    float maxSpeed = 4.0f + m_hero.waveNumber * 0.3f;
    std::uniform_real_distribution<float> chaseSpeedDist(baseSpeed, maxSpeed);
    
    int side = sideDist(s_rng);
    float pos = edgeDist(s_rng);
    
    switch (side) {
        case 0: e.x = -ARENA_HALF + 0.2f; e.y = pos; break;
        case 1: e.x = ARENA_HALF - 0.2f;  e.y = pos; break;
        case 2: e.x = pos; e.y = -ARENA_HALF + 0.2f; break;
        case 3: e.x = pos; e.y = ARENA_HALF - 0.2f;  break;
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
        
        if (!e.alive) {
            e.deathTimer -= dt;
            if (e.deathTimer <= 0.0f) respawnEnemy(e);
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
                if (wobbleLen > 0.0f) { dx /= wobbleLen; dy /= wobbleLen; }
                
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
                        if (wobbleLen > 0.0f) { dx /= wobbleLen; dy /= wobbleLen; }
                        
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
    
    for (auto& e : m_enemies) {
        if (!e.alive) {
            e.deathTimer -= dt;
            if (e.deathTimer <= 0.0f) respawnEnemy(e);
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

void Game::addArenaBoundaryInstances() {
    // Create visible boundary markers around the arena
    float boundaryScale = 0.15f;
    float spacing = 1.0f;
    
    // Pulsing boundary color
    float pulse = std::sin(m_time * 2.0f) * 0.3f + 0.5f;
    
    for (float pos = -ARENA_HALF; pos <= ARENA_HALF; pos += spacing) {
        // Top edge
        InstanceData top{};
        top.offsetX = pos;
        top.offsetY = ARENA_HALF;
        top.colorR = 0.2f; top.colorG = 0.3f + pulse * 0.2f; top.colorB = 0.5f;
        top.scale = boundaryScale;
        m_instances.push_back(top);
        
        // Bottom edge
        InstanceData bottom{};
        bottom.offsetX = pos;
        bottom.offsetY = -ARENA_HALF;
        bottom.colorR = 0.2f; bottom.colorG = 0.3f + pulse * 0.2f; bottom.colorB = 0.5f;
        bottom.scale = boundaryScale;
        m_instances.push_back(bottom);
        
        // Left edge
        InstanceData left{};
        left.offsetX = -ARENA_HALF;
        left.offsetY = pos;
        left.colorR = 0.2f; left.colorG = 0.3f + pulse * 0.2f; left.colorB = 0.5f;
        left.scale = boundaryScale;
        m_instances.push_back(left);
        
        // Right edge
        InstanceData right{};
        right.offsetX = ARENA_HALF;
        right.offsetY = pos;
        right.colorR = 0.2f; right.colorG = 0.3f + pulse * 0.2f; right.colorB = 0.5f;
        right.scale = boundaryScale;
        m_instances.push_back(right);
    }
}

void Game::addShockwaveInstances() {
    if (m_hero.shockwaveAlpha <= 0.0f) return;
    
    // Create ring of triangles for shockwave effect
    int segments = 24;
    float radius = m_hero.shockwaveRadius;
    float alpha = m_hero.shockwaveAlpha;
    
    for (int i = 0; i < segments; ++i) {
        float angle = (float)i / (float)segments * 6.28318f;
        
        InstanceData wave{};
        wave.offsetX = m_hero.x + std::cos(angle) * radius;
        wave.offsetY = m_hero.y + std::sin(angle) * radius;
        wave.colorR = 0.5f + alpha * 0.5f;
        wave.colorG = 0.8f + alpha * 0.2f;
        wave.colorB = 1.0f;
        wave.scale = 0.2f * alpha;
        m_instances.push_back(wave);
    }
}

void Game::rebuildInstances() {
    m_instances.clear();
    
    uint32_t aliveCount = 0;
    for (const auto& e : m_enemies) {
        if (e.alive) aliveCount++;
    }
    
    // Reserve space: boundary + shockwave + hero + enemies
    m_instances.reserve(200 + 24 + 1 + aliveCount);
    
    // Add arena boundary first (drawn behind everything)
    addArenaBoundaryInstances();
    
    // Add shockwave effect
    addShockwaveInstances();

    // === HERO ===
    float pulse = std::sin(m_hero.pulsePhase) * 0.5f + 0.5f;
    float heroScale = 0.55f + pulse * 0.1f;
    float attackFlash = (m_hero.shockwaveAlpha > 0.5f) ? 1.0f : 0.0f;
    float damageFlash = m_hero.damageFlash;
    
    // Game over effect
    bool gameOver = m_hero.health <= 0;
    
    InstanceData hero{};
    hero.offsetX = m_hero.x;
    hero.offsetY = m_hero.y;
    
    if (gameOver) {
        // Gray when dead
        hero.colorR = 0.3f;
        hero.colorG = 0.3f;
        hero.colorB = 0.3f;
        hero.scale = heroScale * 0.8f;
    } else if (damageFlash > 0.0f) {
        hero.colorR = 1.0f;
        hero.colorG = 0.2f;
        hero.colorB = 0.2f;
        hero.scale = heroScale;
    } else {
        hero.colorR = 0.3f + pulse * 0.4f + attackFlash * 0.5f;
        hero.colorG = 0.8f + pulse * 0.2f + attackFlash * 0.2f;
        hero.colorB = 1.0f;
        hero.scale = heroScale + attackFlash * 0.3f;
    }
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
        
        // More vibrant colors, danger indication
        inst.colorR = 0.8f + proximity * 0.2f;
        inst.colorG = 0.25f - proximity * 0.15f;
        inst.colorB = 0.05f + proximity * 0.1f;
        inst.scale = 0.18f + proximity * 0.06f;
        m_instances.push_back(inst);
    }

    m_stats.aliveCount = aliveCount;
    m_stats.enemyCount = (uint32_t)m_enemies.size();
}

void Game::spawnEnemiesInGrid(uint32_t count) {
    m_enemies.clear();
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
            e.deathTimer = 0.5f;
        }
        
        m_enemies.push_back(e);
    }
}

}