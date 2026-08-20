<div align="center">

# LEGIONFALL

### A High-Performance Micro-Strategy Arena Demo

**Pure C++20 | Vulkan 1.2+ | Win32 | No Engine**

[![C++](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://isocpp.org/)
[![Vulkan](https://img.shields.io/badge/Vulkan-1.2+-red.svg)](https://www.vulkan.org/)
[![Platform](https://img.shields.io/badge/Platform-Windows-lightgrey.svg)](https://www.microsoft.com/windows)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![CI](https://github.com/1sitm3n/Legionfall/actions/workflows/ci.yml/badge.svg)](https://github.com/1sitm3n/Legionfall/actions/workflows/ci.yml)

*A technical demonstration of real-time rendering and parallel computation, featuring GPU-instanced rendering of tens of thousands of entities with multi-threaded AI processing.*

[Features](#-features) • [Architecture](#-architecture) • [Building](#-building) • [Controls](#-controls) • [Performance](#-performance) • [Technical Deep-Dive](#-technical-deep-dive)

---

</div>

##  Overview

**Legionfall** is a from-scratch game engine and micro-strategy arena demo built without any game engine or framework dependencies. The project demonstrates professional-grade systems programming, modern graphics API usage, and high-performance computing techniques.

The core challenge: render and simulate **thousands of AI-controlled enemies** chasing a player-controlled hero in real-time, while maintaining smooth frame rates and responsive controls.

### Why No Engine?

This project intentionally avoids Unity, Unreal, Godot, and similar engines to demonstrate:

- **Deep understanding** of graphics programming fundamentals
- **Systems-level thinking** about memory, threading, and GPU communication
- **Problem-solving skills** when frameworks aren't available
- **Ownership of the entire technology stack** from window creation to pixel output

---

## Features

###  Gameplay
- **Hero Control** — Smooth WASD movement with normalized diagonal speed
- **Shockwave Attack** — Area-of-effect ability that eliminates nearby enemies
- **Wave System** — Progressive difficulty scaling every 100 kills
- **Health & Combat** — Contact damage, visual feedback, and game over state
- **Enemy Respawn** — Defeated enemies respawn at arena edges after a delay

### ️ Rendering
- **GPU Instancing** — Single draw call renders all entities (hero + 50,000 enemies)
- **Dynamic Instance Buffer** — Per-frame uploads of position, colour, and scale data
- **Orthographic Projection** — Clean top-down arena view with aspect ratio correction
- **Camera System** — Smooth follow mode with interpolated tracking
- **Visual Effects** — Pulsing hero, proximity-based enemy colours, shockwave rings, arena boundaries

### Performance
- **Lock-free work stealing** — Chase-Lev deque per worker, no global queue, no mutex on the dispatch path
- **Dynamic load balancing** — fixed-size chunks, so fast cores steal from slow ones instead of waiting at the barrier
- **Zero allocation** — jobs hold their closure inline; nothing calls into the allocator after startup
- **Parallel vs Sequential Toggle** — Real-time comparison of threading performance
- **Heavy Work Mode** — Artificial computation load for stress testing
- **Live Profiling** — FPS, update time, frame time, thread count displayed in real-time
- **Adjustable Entity Count** — Scale from 100 to 50,000 enemies on-the-fly

### ️ Engine Systems
- **Custom JobSystem** — Lock-free work-stealing scheduler (Chase-Lev deques)
- **Vulkan Renderer** — Complete pipeline: swapchain, render pass, shaders, synchronization
- **Win32 Platform Layer** — Native window management and input handling
- **Game State Management** — Clean separation of simulation and presentation

---


##  Architecture
```
Legionfall/
├── CMakeLists.txt              # Build configuration with shader compilation
├── README.md                   # This file
├── .gitignore                  # Build artifacts exclusion
│
├── src/
│   ├── core/
│   │   ├── Game.h/.cpp         # Game state, hero, enemies, combat logic
│   │   └── JobSystem.h/.cpp    # Multi-threaded task scheduler
│   │
│   ├── render/
│   │   └── Renderer.h/.cpp     # Vulkan rendering backend
│   │
│   └── platform/
│       └── Win32VulkanApp.cpp  # Entry point, window, input, main loop
│
├── tests/tests.cpp             # 16 tests, no external framework
└── bench/bench.cpp             # old vs new, three workloads
│
└── shaders/
    ├── instanced.vert          # Vertex shader with instancing support
    └── instanced.frag          # Fragment shader for colored triangles
```

### System Interaction Diagram
```
┌─────────────────────────────────────────────────────────────────┐
│                        Main Loop (Win32)                        │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────────────┐  │
│  │   Input     │───▶│    Game     │───▶│     Renderer        │  │
│  │  Handling   │    │   Update    │    │    (Vulkan)         │  │
│  └─────────────┘    └──────┬──────┘    └─────────────────────┘  │
│                            │                                    │
│                     ┌──────▼──────┐                             │
│                     │  JobSystem  │                             │
│                     │  (Parallel) │                             │
│                     └─────────────┘                             │
└─────────────────────────────────────────────────────────────────┘
```

### Data Flow
```
1. Input Phase
   Win32 WM_KEYDOWN/UP → InputState struct → Game::update()

2. Simulation Phase
   Game::updateHero()           — Player movement & attack
   Game::updateEnemies()        — AI logic (parallel or sequential)
   Game::checkCollisions()      — Combat resolution
   Game::rebuildInstances()     — Prepare GPU data

3. Render Phase
   Renderer::updateInstanceBuffer()  — Upload to GPU
   Renderer::drawFrame()             — Vulkan command submission
   vkQueuePresentKHR()               — Display result
```

---

## Scheduler

The job system was rewritten from a mutex-and-condvar thread pool to a lock-free
work-stealing scheduler.

| | Old | New |
|---|---|---|
| Queue | one global `std::queue` behind a mutex | per-worker Chase-Lev deque |
| Dispatch path | blocking | lock-free |
| Task storage | `std::function` — heap allocation each | inline, no allocation |
| `wait()` | submitter sleeps on a condvar | submitter helps run jobs |
| Load balancing | equal item counts per thread | fixed chunks, stolen on demand |
| Known bug | lost wakeup in `wait()` — could hang | fixed, and the condvar removed from that path |

### Measured

Apple M3 (4P + 4E), 7 workers + the submitting thread, median of 15 runs.
Reproduce with `./build/lf_bench`.

| Workload | Old | New | |
|---|---|---|---|
| **uniform** — 512 identical jobs | 1.32 ms | 1.23 ms | **1.08x** |
| **skewed** — cost clustered in the first 12% | 4.53 ms | 1.15 ms | **3.95x** |
| **tiny** — 200k near-empty jobs | 112.31 ms | 49.37 ms | **2.27x** |

The honest reading: most of the 3.95x is the load balancing, not the lock-free
data structure. Chunking finely enough for the stealer to rebalance is what wins
on skewed work — you'd get much of that from the old pool with smaller chunks.
Lock-free earns its place on `tiny`, where the mutex is genuinely contended and
every task is a `malloc`, and in the bounded worst case, which the benchmark
can't show.

### The fences are load-bearing

Chase-Lev's 2005 correctness proof assumes sequential consistency. On ARM it
isn't safe as written — the owner's store to `bottom` can float past its load of
`top`, and then the owner and a thief both take the same job. The implementation
uses the orderings from Lê et al. (PPoPP 2013).

That claim is reproducible rather than folklore:

```bash
cmake -S . -B build-broken -DCMAKE_BUILD_TYPE=Release -DLF_BREAK_POP_FENCE=ON
cmake --build build-broken
./build-broken/lf_tests deque_steal_no_duplication
```

That downgrades one fence to `relaxed`. On an M3 the duplication test then fails
on about a quarter of runs:

```
FAIL tests.cpp:155  prev == 0  (1 vs 0)          <- job claimed twice
FAIL tests.cpp:201  totalClaimed == kItems  (200001 vs 200000)
```

CI runs this as a canary job that is *expected to fail*. If it ever passes, the
test has stopped being sensitive to the thing it exists to catch.

---

## Playtesting without Windows

The game proper is Win32 + Vulkan. `tools/playtest.cpp` runs the **real
simulation** — the same `Game::update`, the same enemy AI, the same JobSystem —
and draws it with ANSI colour instead of a swapchain, so you can play it on
macOS or Linux:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/lf_playtest 20000
```

Controls are the same as the game: `WASD` move, `SPACE` shockwave, `P` parallel,
`H` heavy, `T` chase, `C` camera, `+`/`-` enemies, `R` restart, `Q` quit.

The HUD shows live scheduler telemetry — jobs run, jobs stolen, how many the
submitting thread picked up in `wait()`, and a per-worker bar so you can watch
the stealing rebalance frame by frame.

Everything below the renderer is the real thing. It is not the Vulkan build, and
it does not pretend to be.

### The parallel path is verified against the sequential one

There's no cross-enemy dependency in the enemy update, so chunking the range
differently must not change a single float:

```bash
./tools/check_determinism.sh
# PASS - parallel path is bit-identical to sequential (77920 bytes)
```

That runs the simulation twice — once forced sequential, once parallel — for 200
fixed-timestep frames and compares the raw instance buffers byte for byte. It
runs in CI.

One run per process on purpose: `Game.cpp` keeps its RNG in a file-static that
`init()` never reseeds, so two `Game` objects in one process start from
different random streams. Worth knowing independently — it also means `restart()`
isn't reproducible.

---

## Building the scheduler (any platform)

The scheduler, its tests and the benchmark build without Vulkan or Windows, so
you can run all of it on macOS or Linux:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure    # 15 tests
./build/lf_bench                              # benchmark
```

Sanitizers — both clean, both run in CI:

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLF_TSAN=ON
cmake --build build-tsan && ctest --test-dir build-tsan --output-on-failure
```

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLF_ASAN=ON
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
```

---

##  Building the game (Windows)


### Prerequisites

| Requirement | Version | Notes |
|-------------|---------|-------|
| **Windows** | 10/11 64-bit | Target platform |
| **Visual Studio** | 2022 | MSVC compiler with C++20 support |
| **Vulkan SDK** | 1.2+ | [Download from LunarG](https://vulkan.lunarg.com/) |
| **CMake** | 3.20+ | Build system generator |

### Build Instructions
```powershell
# Clone the repository
git clone https://github.com/1sitm3n/Legionfall.git
cd Legionfall

# Create build directory
mkdir build
cd build

# Configure with CMake
cmake .. -G "Visual Studio 17 2022" -A x64

# Build (Debug)
cmake --build . --config Debug

# Build (Release - optimized)
cmake --build . --config Release

# Run
.\Debug\Legionfall.exe
# or
.\Release\Legionfall.exe
```

### Troubleshooting

| Issue | Solution |
|-------|----------|
| Vulkan not found | Ensure `VULKAN_SDK` environment variable is set |
| Shader compilation fails | Check that `glslc.exe` is in your PATH |
| Black screen | Verify GPU supports Vulkan 1.2+ |
| Low FPS | Try Release build; check GPU driver updates |

---

##  Controls

### Gameplay

| Key | Action |
|-----|--------|
| `W` `A` `S` `D` | Move hero |
| `↑` `←` `↓` `→` | Move hero (alternate) |
| `SPACE` | **Shockwave Attack** — Eliminate nearby enemies |
| `R` | Restart game (after game over) |
| `ESC` | Exit application |

### Debug & Performance

| Key | Action |
|-----|--------|
| `P` | Toggle **Parallel**/Sequential enemy updates |
| `H` | Toggle **Heavy** work mode (stress test) |
| `T` | Toggle chase AI on/off (peaceful mode) |
| `C` | Toggle **Camera** follow mode |
| `+` | Increase enemy count (+1000) |
| `-` | Decrease enemy count (-1000) |

---

##  Performance

### Benchmarks

Tested on: **NVIDIA GeForce RTX 4090 Laptop GPU** | **Intel Core i9-13980HX**

| Enemy Count | Parallel FPS | Sequential FPS | Speedup |
|-------------|--------------|----------------|---------|
| 1,000 | 144+ | 144+ | ~1.0x |
| 5,000 | 144+ | 140 | ~1.0x |
| 10,000 | 144+ | 95 | ~1.5x |
| 25,000 | 120 | 45 | ~2.7x |
| 50,000 | 75 | 22 | ~3.4x |

*With Heavy Work mode enabled, parallel speedup increases to 6-8x.*

### Profiling Metrics

The application displays real-time performance data:
```
HP: 85 | Kills: 1247 | Wave: 13 | FPS: 144 | 0.42ms | 4975 alive | PAR(8)
       │          │        │        │       │            │         │
       │          │        │        │       │            │         └─ Thread count
       │          │        │        │       │            └─ Active enemies
       │          │        │        │       └─ Update time (AI computation)
       │          │        │        └─ Frames per second
       │          │        └─ Difficulty wave
       │          └─ Total enemies defeated
       └─ Hero health points
```

---

##  Technical Deep-Dive


### GPU Instancing Strategy

Traditional rendering would require one draw call per entity — with 50,000 enemies, that's 50,000 draw calls per frame, which is catastrophically slow.

**Solution**: GPU instancing renders all entities in a **single draw call**.
```cpp
// Vertex shader receives per-instance data
layout(location = 1) in vec2 inOffset;    // World position
layout(location = 2) in vec3 inColor;     // RGB color
layout(location = 3) in float inScale;    // Size multiplier

void main() {
    vec2 worldPos = inPosition * inScale + inOffset;
    vec2 ndcPos = (worldPos - viewOffset) * viewScale;
    gl_Position = vec4(ndcPos, 0.0, 1.0);
}
```

**Instance data structure** (32 bytes, GPU-aligned):
```cpp
struct InstanceData {
    float offsetX, offsetY;       // 8 bytes  — position
    float colorR, colorG, colorB; // 12 bytes — color
    float scale;                  // 4 bytes  — size
    float padding[2];             // 8 bytes  — alignment
};
```

### JobSystem Implementation

The JobSystem distributes work across CPU cores using a thread pool pattern:
```cpp
class JobSystem {
    std::vector<std::thread> m_workers;      // Worker thread pool
    std::queue<std::function<void()>> m_tasks; // Task queue
    std::atomic<int> m_pendingTasks{0};      // Completion tracking
    
    void schedule(std::function<void()> task); // Add work
    void wait();                                // Block until complete
};
```

**Enemy update parallelisation**:
```cpp
void Game::updateEnemiesParallel(float dt, JobSystem* jobs) {
    size_t perJob = enemies.size() / numJobs;
    
    for (size_t i = 0; i < numJobs; ++i) {
        jobs->schedule([=]() {
            // Each thread processes a slice of enemies
            for (size_t j = start; j < end; ++j) {
                updateSingleEnemy(enemies[j], heroPos, dt);
            }
        });
    }
    
    jobs->wait(); // Synchronize before rendering
}
```


### Vulkan Pipeline

The renderer implements a complete Vulkan graphics pipeline:

1. **Instance Creation** — Application and validation layers
2. **Surface Creation** — Win32 window surface via `vkCreateWin32SurfaceKHR`
3. **Physical Device Selection** — Discrete GPU preference with queue family detection
4. **Logical Device** — Graphics and present queue creation
5. **Swapchain** — Double-buffered presentation with FIFO/Mailbox modes
6. **Render Pass** — Single color attachment with clear and store operations
7. **Graphics Pipeline** — Vertex/fragment shaders, vertex input, dynamic viewport
8. **Command Buffers** — Per-frame recording with synchronisation primitives
9. **Memory Management** — Host-visible buffers for dynamic instance data

### Synchronisation

Vulkan requires explicit synchronisation:
```cpp
// Frame synchronisation objects
VkSemaphore imageAvailableSemaphore; // GPU: image acquired
VkSemaphore renderFinishedSemaphore; // GPU: rendering complete
VkFence inFlightFence;               // CPU-GPU: frame boundary

// Render loop
vkWaitForFences(..., inFlightFence);           // Wait for previous frame
vkAcquireNextImageKHR(..., imageAvailable...); // Get swapchain image
vkQueueSubmit(..., renderFinished...);         // Submit commands
vkQueuePresentKHR(...);                        // Present to screen
```

---

##  Roadmap

### Potential Enhancements

- [ ] **Spatial Partitioning** — Quadtree/grid for O(1) collision detection
- [ ] **Compute Shaders** — Move AI entirely to GPU
- [ ] **Sprite Rendering** — Textured quads instead of colored triangles
- [ ] **Audio System** — Sound effects and music via XAudio2
- [ ] **Particle Effects** — Death explosions, attack trails
- [ ] **Multiple Enemy Types** — Varied behaviors and appearances
- [ ] **Power-ups** — Health, speed boosts, attack upgrades
- [ ] **Leaderboard** — High score persistence

---

##  Learning Resources

This project draws from these excellent resources:

- [Vulkan Tutorial](https://vulkan-tutorial.com/) — Comprehensive Vulkan introduction
- [Vulkan Specification](https://www.khronos.org/registry/vulkan/specs/1.3/html/) — Official API reference
- [Game Programming Patterns](https://gameprogrammingpatterns.com/) — Architecture guidance
- [Handmade Hero](https://handmadehero.org/) — From-scratch game development philosophy

---

##  License

This project is licensed under the MIT License.
```


---

##  Author

**Mehmet Isitmen**

- GitHub: [@1sitm3n](https://github.com/1sitm3n)

---
