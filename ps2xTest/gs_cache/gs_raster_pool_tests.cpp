// RasterPool (runtime/gs/gs_raster_pool.h): every row of a job is shaded exactly once, back-to-back
// jobs never see each other's context, and a worker the host schedules late (beforeAdopt delay)
// neither holds the caller up nor touches a job that has already completed.
#include "gs_test_support.h"
#include "runtime/gs/gs_raster_pool.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using namespace GSTest;

namespace
{
    using Clock = std::chrono::steady_clock;

    struct Canvas
    {
        std::vector<std::atomic<int>> hits;
        std::atomic<bool> alive{true};
        explicit Canvas(size_t rows) : hits(rows) {}
    };

    void paint(void *context, int ys, int ye)
    {
        auto *canvas = static_cast<Canvas *>(context);
        require(canvas->alive.load(std::memory_order_acquire), "a chunk ran on a context whose job had completed");
        for (int y = ys; y <= ye; ++y)
            canvas->hits[static_cast<size_t>(y)].fetch_add(1, std::memory_order_relaxed);
    }

    void checkCovered(const Canvas &canvas, int y0, int y1, std::string_view what)
    {
        for (size_t y = 0; y < canvas.hits.size(); ++y)
        {
            const int expected = (static_cast<int>(y) >= y0 && static_cast<int>(y) <= y1) ? 1 : 0;
            if (canvas.hits[y].load() != expected)
                throw std::runtime_error(std::string(what) + ": row " + std::to_string(y) + " shaded " +
                                         std::to_string(canvas.hits[y].load()) + " times, expected " + std::to_string(expected));
        }
    }

    // Many jobs of odd sizes, chunk sizes and starting rows, back to back: rows covered once each.
    void coverage()
    {
        ps2x::gs::RasterPool pool(8);
        require(pool.threads() == 8, "threads = workers + caller");
        for (int job = 0; job < 3000; ++job)
        {
            const int y0 = (job * 7) % 97;
            const int y1 = y0 + (job * 13) % 211;
            const int rows = 2 + (job % 9) * 2 + (job % 2);
            Canvas canvas(static_cast<size_t>(y1) + 4);
            pool.run(y0, y1, rows, paint, &canvas);
            canvas.alive.store(false, std::memory_order_release);
            checkCovered(canvas, y0, y1, "job " + std::to_string(job));
        }
        Canvas empty(8);
        pool.run(5, 4, 2, paint, &empty);
        checkCovered(empty, 1, 0, "empty range");
    }

    // A serial pool (threads = 1) runs everything on the caller.
    void serial()
    {
        ps2x::gs::RasterPool pool(1);
        require(pool.threads() == 1, "serial pool");
        Canvas canvas(64);
        pool.run(3, 60, 4, paint, &canvas);
        checkCovered(canvas, 3, 60, "serial");
    }

    // Workers that wake late (every adoption delayed 20 ms, longer than a job): the caller
    // completes each job itself, promptly, and the late workers find the job closed instead of
    // shading a context that is gone.
    void lateWorkers()
    {
        ps2x::gs::RasterPool pool(4, []
                                  { std::this_thread::sleep_for(std::chrono::milliseconds(20)); });
        const auto start = Clock::now();
        for (int job = 0; job < 50; ++job)
        {
            auto canvas = std::make_unique<Canvas>(512);
            pool.run(0, 511, 2, paint, canvas.get());
            canvas->alive.store(false, std::memory_order_release);
            checkCovered(*canvas, 0, 511, "late job " + std::to_string(job));
            canvas.reset(); // a straggler touching it now would read freed memory (and fail alive)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        // 50 jobs; waiting for every worker would cost >= 50 x 20 ms = 1 s.
        require(elapsed < 900, "the caller waited for late workers: " + std::to_string(elapsed) + " ms for 50 jobs");
        // Let the stragglers drain so the destructor's join sees them idle.
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }

    // Workers that are merely slow to pick up (1 ms) still share the work when the job is long,
    // and the result is still exact.
    void slowWorkersShare()
    {
        std::atomic<int> adoptions{0};
        ps2x::gs::RasterPool pool(4, [&]
                                  { adoptions.fetch_add(1); std::this_thread::sleep_for(std::chrono::microseconds(200)); });
        for (int job = 0; job < 20; ++job)
        {
            Canvas canvas(4096);
            pool.run(0, 4095, 2, [](void *context, int ys, int ye)
                     {
                         std::this_thread::sleep_for(std::chrono::microseconds(50));
                         paint(context, ys, ye); },
                     &canvas);
            canvas.alive.store(false, std::memory_order_release);
            checkCovered(canvas, 0, 4095, "slow job " + std::to_string(job));
        }
        require(adoptions.load() > 0, "workers never woke");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

int main(int argc, char **argv)
{
    return run(argc, argv, {{"coverage", coverage},
                            {"serial", serial},
                            {"late_workers", lateWorkers},
                            {"slow_workers_share", slowWorkersShare}});
}
