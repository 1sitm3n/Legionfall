// Terminal playtest harness.
//
// The real game is Win32 + Vulkan, so it won't run on macOS or Linux. This runs
// the actual simulation - the same Game::update, the same enemy AI, the same
// JobSystem - and draws it with ANSI colour instead of a swapchain. Everything
// below the renderer is the real thing, so it's a genuine playtest of the game
// logic and the scheduler. It just isn't the Vulkan build.
//
// Each terminal cell is two vertical pixels using a half block, so the picture
// is twice as tall as the row count.
//
//   ./build/lf_playtest [enemyCount]
//   ./build/lf_playtest [enemyCount] --frames N   headless, for smoke tests
//   ./build/lf_playtest --dump-state [--sequential]  raw instance dump on stdout
//
// --dump-state is how tools/check_determinism.sh proves the parallel enemy
// update gives bit-identical results to the sequential one. It has to be one
// run per process because Game.cpp keeps its RNG in a file-static that never
// gets reseeded, so a second Game in the same process starts from a different
// random stream.
//
// Controls match the game: WASD move, SPACE shockwave, P parallel, H heavy,
// T chase, C camera, +/- enemies, R restart, Q or ESC quit.

#include "core/Game.h"
#include "core/JobSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

using namespace Legionfall;
using Clock = std::chrono::steady_clock;

namespace {

termios       g_savedTerm;
bool          g_termSaved = false;
volatile sig_atomic_t g_quit = 0;

void restoreTerminal() {
    if (g_termSaved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_savedTerm);
        g_termSaved = false;
    }
    std::fputs("\x1b[?25h"   // show cursor
               "\x1b[?1049l" // leave alternate screen
               "\x1b[0m", stdout);
    std::fflush(stdout);
}

void onSignal(int) { g_quit = 1; }

// Raw mode so we get keys without waiting for enter, and without echo.
void setupTerminal() {
    if (tcgetattr(STDIN_FILENO, &g_savedTerm) == 0) g_termSaved = true;
    termios t = g_savedTerm;
    t.c_lflag &= ~(unsigned)(ICANON | ECHO);
    t.c_cc[VMIN]  = 0;      // non-blocking read
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);

    std::atexit(restoreTerminal);
    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);

    std::fputs("\x1b[?1049h"   // alternate screen, so the scrollback survives
               "\x1b[?25l"     // hide cursor
               "\x1b[2J", stdout);
}

void terminalSize(int& cols, int& rows) {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    } else {
        cols = 100;
        rows = 30;
    }
}

struct Keys {
    bool up=false, down=false, left=false, right=false;
    bool attack=false, parallel=false, heavy=false, camera=false, chase=false;
    bool more=false, less=false, restart=false, quit=false;
};

// Terminals don't report key-up, so movement is held for a short window after
// the last press. Feels close enough to held keys for a playtest.
struct HeldKey {
    double remaining = 0.0;
    void press() { remaining = 0.12; }
    bool  down(double dt) { remaining -= dt; return remaining > 0.0; }
};

Keys pollInput(HeldKey& hu, HeldKey& hd, HeldKey& hl, HeldKey& hr, double dt) {
    Keys k;
    char buf[64];
    const ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    for (ssize_t i = 0; i < n; ++i) {
        switch (buf[i]) {
            case 'w': case 'W': hu.press(); break;
            case 's': case 'S': hd.press(); break;
            case 'a': case 'A': hl.press(); break;
            case 'd': case 'D': hr.press(); break;
            case ' ': k.attack   = true; break;
            case 'p': case 'P': k.parallel = true; break;
            case 'h': case 'H': k.heavy    = true; break;
            case 'c': case 'C': k.camera   = true; break;
            case 't': case 'T': k.chase    = true; break;
            case '+': case '=': k.more     = true; break;
            case '-': case '_': k.less     = true; break;
            case 'r': case 'R': k.restart  = true; break;
            case 'q': case 'Q': case 27:   k.quit = true; break;
            default: break;
        }
    }
    k.up    = hu.down(dt);
    k.down  = hd.down(dt);
    k.left  = hl.down(dt);
    k.right = hr.down(dt);
    return k;
}

struct Pixel { unsigned char r=0, g=0, b=0; };

void appendColour(std::string& out, const char* lead, Pixel p) {
    char tmp[32];
    std::snprintf(tmp, sizeof(tmp), "%s%u;%u;%um", lead, p.r, p.g, p.b);
    out += tmp;
}

} // namespace

int main(int argc, char** argv) {
    std::uint32_t enemyCount = 20000;
    int  headlessFrames = 0;
    bool dumpState = false;
    bool forceSequential = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            headlessFrames = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--dump-state") == 0) {
            dumpState = true;
        } else if (std::strcmp(argv[i], "--sequential") == 0) {
            forceSequential = true;
        } else {
            const int v = std::atoi(argv[i]);
            if (v > 0) enemyCount = (std::uint32_t)v;
        }
    }
    // No TTY means nobody is watching - run a fixed number of frames and exit
    // rather than spinning forever. Lets CI smoke-test the sim.
    const bool headless = dumpState || headlessFrames > 0 || !isatty(STDIN_FILENO);
    if (headless && headlessFrames == 0) headlessFrames = 120;
    if (dumpState) { headlessFrames = 200; enemyCount = 4000; }

    if (!headless) setupTerminal();

    JobSystem jobs;
    Game      game;
    game.init(enemyCount);

    HeldKey hu, hd, hl, hr;
    auto last = Clock::now();

    // Frame time smoothed a little - the raw number jitters too much to read.
    double smoothedFrameMs  = 16.0;
    double smoothedUpdateMs = 0.0;

    std::string frame;
    frame.reserve(1 << 20);

    int framesRun = 0;
    while (!g_quit) {
        if (headless && framesRun >= headlessFrames) break;
        ++framesRun;
        const auto now = Clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        if (dt > 0.1) dt = 0.1;          // don't let a stall teleport everyone

        // Fixed step for the determinism dump. Wall-clock dt would make the two
        // runs simulate different amounts of time and the comparison would be
        // meaningless - which is exactly the mistake I made the first time.
        if (dumpState) dt = 1.0 / 60.0;

        Keys k;
        if (!headless) {
            k = pollInput(hu, hd, hl, hr, dt);
            if (k.quit) break;
        } else if (dumpState) {
            // Fixed script so both runs see identical input.
            k.parallel = (framesRun == 1 && forceSequential);   // toggle off once
            k.right    = (framesRun / 10) % 2 == 0;
            k.attack   = (framesRun % 25) == 0;
        } else {
            // Drive it a bit so the parallel path and combat actually run.
            k.right  = (framesRun / 20) % 2 == 0;
            k.up     = (framesRun / 30) % 2 == 0;
            k.attack = (framesRun % 25) == 0;
        }

        InputState in;
        in.moveUp = k.up; in.moveDown = k.down;
        in.moveLeft = k.left; in.moveRight = k.right;
        in.attack = k.attack;
        in.toggleParallel = k.parallel;
        in.toggleHeavyWork = k.heavy;
        in.toggleCameraFollow = k.camera;
        in.toggleChaseMode = k.chase;
        in.increaseEnemies = k.more;
        in.decreaseEnemies = k.less;
        in.restart = k.restart;

        jobs.resetStats();
        const auto t0 = Clock::now();
        game.update((float)dt, in, &jobs);
        const double updateMs =
            std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

        const ProfilingStats st = game.getStats();
        const auto js = jobs.stats();

        smoothedUpdateMs = smoothedUpdateMs * 0.9 + updateMs * 0.1;
        smoothedFrameMs  = smoothedFrameMs  * 0.9 + (dt * 1000.0) * 0.1;

        if (dumpState) continue;

        if (headless) {
            if (framesRun % 40 == 0) {
                std::printf("frame %4d  update %6.3fms  alive %6u  kills %5u  "
                            "jobs %5llu  stolen %5llu\n",
                            framesRun, updateMs, st.aliveCount, st.killCount,
                            (unsigned long long)js.executed,
                            (unsigned long long)js.stolen);
            }
            continue;
        }

        // ---- draw ----
        int cols, rows;
        terminalSize(cols, rows);
        const int hudRows = 7;
        const int gridW = std::max(20, cols);
        const int gridH = std::max(10, rows - hudRows);
        const int pxH   = gridH * 2;            // half blocks

        static std::vector<Pixel> px;
        px.assign((std::size_t)gridW * pxH, Pixel{});

        const float half = 10.0f;               // Game::ARENA_HALF
        for (const InstanceData& e : game.getInstanceData()) {
            const float nx = (e.offsetX + half) / (2.0f * half);
            const float ny = (half - e.offsetY) / (2.0f * half);
            if (nx < 0.0f || nx >= 1.0f || ny < 0.0f || ny >= 1.0f) continue;

            const int cx = (int)(nx * (float)gridW);
            const int cy = (int)(ny * (float)pxH);
            Pixel& p = px[(std::size_t)cy * gridW + cx];

            // Brighter wins, so the hero and shockwave stay visible in a crowd.
            const unsigned char r = (unsigned char)std::clamp(e.colorR * 255.0f, 0.0f, 255.0f);
            const unsigned char g = (unsigned char)std::clamp(e.colorG * 255.0f, 0.0f, 255.0f);
            const unsigned char b = (unsigned char)std::clamp(e.colorB * 255.0f, 0.0f, 255.0f);
            if (r + g + b >= p.r + p.g + p.b) { p.r = r; p.g = g; p.b = b; }
        }

        frame.clear();
        frame += "\x1b[H";                       // home, no clear - less flicker

        for (int y = 0; y < gridH; ++y) {
            Pixel lastTop{1,1,1}, lastBot{1,1,1};
            bool first = true;
            for (int x = 0; x < gridW; ++x) {
                const Pixel top = px[(std::size_t)(y * 2) * gridW + x];
                const Pixel bot = px[(std::size_t)(y * 2 + 1) * gridW + x];
                // Only emit an escape when the colour actually changes.
                if (first || std::memcmp(&top, &lastTop, sizeof(Pixel)) != 0) {
                    appendColour(frame, "\x1b[38;2;", top);
                    lastTop = top;
                }
                if (first || std::memcmp(&bot, &lastBot, sizeof(Pixel)) != 0) {
                    appendColour(frame, "\x1b[48;2;", bot);
                    lastBot = bot;
                }
                first = false;
                frame += "▀";               // upper half block
            }
            frame += "\x1b[0m\x1b[K\n";
        }

        // ---- HUD ----
        char line[512];
        frame += "\x1b[0m";

        std::snprintf(line, sizeof(line),
            "\x1b[1m LEGIONFALL \x1b[0m\x1b[2m terminal playtest - simulation is real, renderer is not\x1b[0m\x1b[K\n");
        frame += line;

        std::snprintf(line, sizeof(line),
            " fps %6.1f   frame %6.2fms   update \x1b[1m%6.3fms\x1b[0m   enemies %6u   alive %6u\x1b[K\n",
            smoothedFrameMs > 0.0 ? 1000.0 / smoothedFrameMs : 0.0,
            smoothedFrameMs, smoothedUpdateMs, st.enemyCount, st.aliveCount);
        frame += line;

        std::snprintf(line, sizeof(line),
            " hp %s%3d\x1b[0m   kills %5u   wave %2d   %s\x1b[K\n",
            st.heroHealth > 40 ? "\x1b[32m" : "\x1b[31m", st.heroHealth,
            st.killCount, st.waveNumber,
            game.isGameOver() ? "\x1b[1;31mGAME OVER - press R\x1b[0m" : "");
        frame += line;

        std::snprintf(line, sizeof(line),
            " [P]arallel %s   [H]eavy %s   [T]chase %s   [C]amera %s   workers %zu\x1b[K\n",
            st.parallelEnabled  ? "\x1b[32mON \x1b[0m" : "\x1b[31mOFF\x1b[0m",
            st.heavyWorkEnabled ? "\x1b[33mON \x1b[0m" : "\x1b[2mOFF\x1b[0m",
            st.chaseModeEnabled ? "\x1b[32mON \x1b[0m" : "\x1b[2mOFF\x1b[0m",
            st.cameraFollowEnabled ? "\x1b[32mON \x1b[0m" : "\x1b[2mOFF\x1b[0m",
            jobs.threadCount());
        frame += line;

        // Scheduler telemetry for this frame only - resetStats() runs each loop.
        std::snprintf(line, sizeof(line),
            " \x1b[36mscheduler\x1b[0m  jobs %5llu   stolen \x1b[1m%5llu\x1b[0m   "
            "submitter ran %4llu   CAS aborts %5llu\x1b[K\n",
            (unsigned long long)js.executed, (unsigned long long)js.stolen,
            (unsigned long long)js.mainExecuted, (unsigned long long)js.stealAborts);
        frame += line;

        // Per-worker bar so you can watch the stealing rebalance live.
        frame += " \x1b[2mper-worker:\x1b[0m ";
        std::uint64_t peak = 1;
        for (std::size_t i = 0; i < jobs.workerStatsCount(); ++i)
            peak = std::max(peak, jobs.worker(i).executed);
        for (std::size_t i = 0; i < jobs.workerStatsCount(); ++i) {
            const auto w = jobs.worker(i);
            const int lvl = (int)((w.executed * 8) / peak);
            static const char* blocks[9] = {" ","▁","▂","▃","▄",
                                            "▅","▆","▇","█"};
            frame += blocks[std::clamp(lvl, 0, 8)];
        }
        std::snprintf(line, sizeof(line), "  \x1b[2m(bar height = jobs run this frame)\x1b[0m\x1b[K\n");
        frame += line;

        std::snprintf(line, sizeof(line),
            "\x1b[2m WASD move  SPACE shockwave  P parallel  H heavy  T chase  C camera  +/- enemies  R restart  Q quit\x1b[0m\x1b[K");
        frame += line;

        std::fwrite(frame.data(), 1, frame.size(), stdout);
        std::fflush(stdout);

        // Cap at ~60fps. The sim can go faster but there's no point redrawing a
        // terminal quicker than this.
        const double spent = std::chrono::duration<double>(Clock::now() - now).count();
        if (spent < 1.0 / 60.0) {
            usleep((useconds_t)((1.0 / 60.0 - spent) * 1e6));
        }
    }

    if (!headless) restoreTerminal();

    if (dumpState) {
        const auto& d = game.getInstanceData();
        std::fwrite(d.data(), sizeof(InstanceData), d.size(), stdout);
        std::fprintf(stderr, "%s: %zu instances\n",
                     forceSequential ? "sequential" : "parallel", d.size());
        return 0;
    }

    const ProfilingStats fin = game.getStats();
    std::printf("Left after wave %d with %u kills.\n", fin.waveNumber, fin.killCount);
    return 0;
}
