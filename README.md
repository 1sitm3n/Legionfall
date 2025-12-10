# Legionfall – Vulkan Crowd + Job System Demo

Legionfall is a small Vulkan-based rendering demo that focuses on CPU-side parallelism rather than game features. It renders a large crowd of GPU-instanced triangles and updates their positions every frame, using a custom C++ thread-pool job system. At runtime you can toggle between single-threaded and multi-threaded simulation, as well as a heavier per-instance workload, to see how update time and frame time change.

This project is meant as a portfolio piece that sits between game development and high performance computing. It shows Vulkan familiarity, custom engine-style architecture, and a practical use of multithreading and profiling.

---

## Features

Legionfall creates a Win32 window and initialises a Vulkan 1.2 device, surface and swapchain. It sets up a very small render pipeline that draws a single coloured triangle mesh, but it instances that triangle across a grid of two hundred by two hundred instances, for a total of forty thousand animated triangles on screen.

Each instance has its own position offset and colour. The offsets are updated every frame, so the grid appears as a moving, wave-like field. The instance update step is done either on a single core or across a thread pool. This is controlled by a runtime flag so you can switch between both modes while the application is running and compare timings.

On top of that, an optional “heavy work” mode adds extra computation per instance. This exaggerates the difference between single-threaded and parallel updates, making the benefit of the job system very clear in the console output.

The application prints three key metrics once per second: frames per second, the time spent in the simulation step in milliseconds, and the total frame time in milliseconds. These are printed with tags that show whether the current mode is parallel or single-threaded.

---

## Controls

The application runs in a Win32 window and prints profiling information to the console.

Press P while the window is focused to toggle between parallel update and single-threaded update. When parallel update is enabled, the console prefix is “[Parallel]”; when disabled, the prefix becomes “[Single]”.

Press H while the window is focused to toggle heavy work mode on and off. When heavy work mode is enabled, each instance performs extra computation during the update step, increasing the amount of CPU work and making the performance difference between single-threaded and multi-threaded modes much more visible.

Closing the window or pressing the close button will terminate the application cleanly and shut down Vulkan.

---

## High-level Architecture

The project is structured as a simple, self-contained Vulkan “engine” with a thin platform layer.

The platform layer is responsible for creating the Win32 window, processing the message loop, and driving the main loop. It integrates with the Vulkan instance and surface creation, and it owns the main render loop that calls the simulation update, instance buffer upload, and frame rendering.

The Vulkan layer is responsible for creating the instance, surface, physical and logical device, queues, swapchain, image views, render pass, framebuffers, graphics pipeline, command pool, command buffers and synchronisation primitives. It also manages the lifetime of the vertex buffer for the base triangle and the instance buffer for all the per-instance data.

The simulation layer holds the grid of instance data, which includes offsets and colours. It exposes an update function that advances a global time value, walks all instances, computes their animated offsets, and writes them back into the array. The instance data is then uploaded each frame into a host-visible Vulkan buffer and used as a per-instance vertex buffer in the graphics pipeline.

The job system layer is a C++ thread pool implemented in `core/JobSystem.h`. It creates a number of worker threads (typically the number of hardware threads on the CPU) and exposes three primary operations: scheduling jobs, waiting for all scheduled jobs to complete, and querying the current number of worker threads. The simulation code uses this job system to partition the instance array into contiguous chunks, submits one job per worker thread, and waits for all jobs to finish before rendering.

---

## Parallel vs Single-threaded Simulation

The heart of this demo is the comparison between single-threaded and parallel instance updates.

In single-threaded mode, the simulation simply loops over all instances in a single for-loop, computing the base position from the grid coordinate, then applying a time-based sine and cosine offset to create a wave effect. The work for all instances is done on the main thread before rendering.

In parallel mode, the total number of instances is split into roughly equal chunks, one chunk per worker thread. For each chunk, a job is scheduled into the job system. Each worker thread runs the same inner update loop for its own range of indices. After scheduling all chunk jobs, the main thread waits for the job system to signal that there are no more active jobs and no more pending jobs. At that point, all instances have been updated and the frame can continue.

The heavy work flag increases the amount of arithmetic done per instance. With heavy work disabled, parallelism still helps but the benefit is modest on a very fast desktop CPU. With heavy work enabled, the difference between the single-threaded and parallel update times becomes substantial, and the console metrics clearly demonstrate that the job system significantly reduces simulation time while keeping the visual result identical.

---

## Building the Project

The project is built with CMake and MSVC on Windows, and assumes that the Vulkan SDK is installed and configured.

First ensure that the Vulkan SDK is installed and that the `VULKAN_SDK` environment variable is set. Install the Desktop development with C++ workload in Visual Studio so that MSBuild and the MSVC toolchain are available.

Once CMake has generated the Visual Studio project files, build the Debug configuration with:

    cmake --build . --config Debug


This produces Debug\Legionfall.exe inside the build directory. Run the executable directly:

    .\Debug\Legionfall.exe

If you change the code, you can simply rerun the build command and re-launch the executable. The project is small enough that incremental builds are effectively instant on a modern machine.

## Code Highlights

The main entry point lives in src/platform/main.cpp. It sets up the Win32 window, initialises Vulkan, creates the job system, enters the main loop, and handles per-frame timing and profiling output.

The job system implementation is in src/core/JobSystem.h. It uses std::thread, std::mutex, std::condition_variable, and a queue of std::function<void()> jobs. Worker threads run a loop that waits on a condition variable, pops jobs from the queue, executes them, and signals another condition variable when all jobs are done.

The Vulkan setup code includes custom helper functions to choose queue families, select swapchain formats and present modes, create the swapchain, image views, render pass, graphics pipeline, framebuffers, command pool, vertex buffers, instance buffers, and synchronisation objects. The command buffers bind both the vertex buffer for the base triangle and the instance buffer for the per-instance data and issue a single instanced draw call each frame.

The simulation update function is written to be used identically by both the single-threaded path and the parallel path; only the outer loop shape changes. This makes it easier to verify correctness and to reason about performance differences.

## Future Extensions

This demo is intentionally focused. However, it could be extended in several directions to evolve into a more game-like or engine-like project.

A basic orbit or free-fly camera could be added so that you can move around the instanced crowd, which would showcase the scale and density of the rendered geometry. A simple UI overlay could be added to show current mode, heavy work status, number of instances, and per-frame timings rendered directly into the swapchain images instead of only in the console.

On the rendering side, the pipeline could be upgraded to use a proper uniform buffer or push constants for view and projection matrices, and to render a more complex mesh instead of a single triangle. On the simulation side, the instance data could be turned into agents with behaviours, pathfinding, or LOD-aware update patterns to demonstrate more advanced game AI and systems design.

Even in its current state, the project already provides a strong demonstration of C++ engine-style code, Vulkan proficiency, and practical multithreading.

