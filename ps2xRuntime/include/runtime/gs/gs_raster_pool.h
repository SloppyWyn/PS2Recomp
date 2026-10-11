#pragma once

// Raster workers for the CPU GS backend. The pixels of one primitive are independent (each is
// read-modify-written once per primitive), so its rows are shaded in parallel: the rows are handed
// out in chunks that start on even rows (a column of the frame and Z layouts holds two rows, so no
// two threads share a cache line), every row is computed from its index rather than from the row
// before, and the caller waits for all chunks before the next primitive, which keeps the GS's
// in-order semantics between primitives.
//
// Completion is counted in chunks, not in workers: the caller returns as soon as every chunk is
// done and no worker is inside one, so a worker the host has not scheduled yet (a loaded machine:
// a build on all cores) costs nothing; it finds the job closed when it wakes and goes back to
// sleep. An earlier version waited for every worker to check in and spun with
// std::this_thread::yield() on both sides; under load each yield hands a whole quantum to another
// process, and a title-screen primitive took ~100 ms (run stallwd4: 1.8 VBlank/s, the EE thread in
// RasterPool::run and all seven workers in their yield loops). Waiting here is a short pause spin
// (microseconds) and then the condition variable.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace ps2x::gs
{
    class RasterPool
    {
    public:
        using ChunkFn = void (*)(void *context, int yStart, int yEnd);

        // PS2X_GS_THREADS=<n>: threads per primitive including the caller (1 = serial); default
        // min(8, cores / 2).
        static int defaultThreadCount() noexcept
        {
            const unsigned hw = std::thread::hardware_concurrency();
            int threads = hw >= 4u ? static_cast<int>(std::min(8u, hw / 2u)) : 1;
            if (const char *value = std::getenv("PS2X_GS_THREADS"); value && *value)
                threads = std::max(1, std::min(32, std::atoi(value)));
            return threads;
        }

        static RasterPool &instance()
        {
            static RasterPool pool(defaultThreadCount());
            return pool;
        }

        // threads = workers + the calling thread. beforeAdopt (tests): run by a worker each time
        // it wakes for a job, before it looks at the job; a delay there stands in for a worker the
        // host schedules late.
        explicit RasterPool(int threads, std::function<void()> beforeAdopt = {})
            : m_beforeAdopt(std::move(beforeAdopt))
        {
            for (int i = 1; i < std::max(1, threads); ++i)
                m_workers.emplace_back([this]
                                       { workerMain(); });
        }

        ~RasterPool()
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stop = true;
                m_generation.fetch_add(1, std::memory_order_release);
            }
            m_wake.notify_all();
            for (std::thread &t : m_workers)
                if (t.joinable())
                    t.join();
        }

        RasterPool(const RasterPool &) = delete;
        RasterPool &operator=(const RasterPool &) = delete;

        int threads() const noexcept { return static_cast<int>(m_workers.size()) + 1; }

        // Runs fn(context, ys, ye) over the rows [y0, y1] in chunks of rowsPerChunk rows (rounded
        // to even), on the workers and the calling thread; returns when every chunk is done. One
        // job at a time per pool: run() is not reentrant and not for concurrent callers.
        void run(int y0, int y1, int rowsPerChunk, ChunkFn fn, void *context)
        {
            if (y1 < y0)
                return;
            Job job;
            job.fn = fn;
            job.context = context;
            job.y0 = y0;
            job.y1 = y1;
            job.base = y0 & ~1;
            job.chunkRows = std::max(2, rowsPerChunk & ~1);
            job.chunks = (y1 - job.base + job.chunkRows) / job.chunkRows;
            if (m_workers.empty() || job.chunks <= 1)
            {
                for (int c = 0; c < job.chunks; ++c)
                    runChunk(job, c);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_job = job;
                m_nextChunk.store(0, std::memory_order_relaxed);
                m_done.store(0, std::memory_order_relaxed);
                m_active.store(0, std::memory_order_relaxed);
                m_open = true;
                m_generation.fetch_add(1, std::memory_order_release);
            }
            m_wake.notify_all();
            work(job);

            // Stragglers: a worker inside its last chunk finishes within microseconds unless the
            // host has preempted it; spin briefly, then sleep on the condition variable.
            const auto finished = [&]
            { return m_done.load(std::memory_order_acquire) >= job.chunks &&
                     m_active.load(std::memory_order_acquire) == 0; };
            spinUntil(finished, kCallerSpin);
            std::unique_lock<std::mutex> lock(m_mutex);
            m_finished.wait(lock, finished);
            m_open = false; // a worker waking from now on sees no job
        }

    private:
        struct Job
        {
            ChunkFn fn = nullptr;
            void *context = nullptr;
            int y0 = 0, y1 = 0, base = 0, chunkRows = 2, chunks = 0;
        };

        // Spin budgets (host time, checked every 64 pauses). Workers stay hot across the gaps
        // between a frame's primitives; the caller waits for a preempted straggler's chunk.
        static constexpr std::chrono::microseconds kWorkerSpin{1000};
        static constexpr std::chrono::microseconds kCallerSpin{200};

        template <typename Pred>
        static bool spinUntil(Pred &&pred, std::chrono::microseconds budget) noexcept
        {
            const auto deadline = std::chrono::steady_clock::now() + budget;
            for (;;)
            {
                for (int i = 0; i < 64; ++i)
                {
                    if (pred())
                        return true;
                    cpuRelax();
                }
                if (std::chrono::steady_clock::now() >= deadline)
                    return pred();
            }
        }

        static void cpuRelax() noexcept
        {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
            _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#else
            std::this_thread::yield();
#endif
        }

        // Takes chunks until none are left; counts each one done.
        void work(const Job &job)
        {
            for (;;)
            {
                const int chunk = m_nextChunk.fetch_add(1, std::memory_order_relaxed);
                if (chunk >= job.chunks)
                    return;
                runChunk(job, chunk);
                m_done.fetch_add(1, std::memory_order_release);
            }
        }

        static void runChunk(const Job &job, int chunk)
        {
            const int ys = std::max(job.y0, job.base + chunk * job.chunkRows);
            const int ye = std::min(job.y1, job.base + (chunk + 1) * job.chunkRows - 1);
            if (ys <= ye)
                job.fn(job.context, ys, ye);
        }

        void workerMain()
        {
            uint64_t seen = 0;
            for (;;)
            {
                Job job;
                {
                    // Primitives arrive back to back within a frame: look for the next one for
                    // up to a millisecond before sleeping.
                    spinUntil([&]
                              { return m_generation.load(std::memory_order_acquire) != seen; },
                              kWorkerSpin);
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_wake.wait(lock, [&]
                                { return m_generation.load(std::memory_order_acquire) != seen; });
                    seen = m_generation.load(std::memory_order_acquire);
                    if (m_stop)
                        return;
                    if (m_beforeAdopt)
                    {
                        lock.unlock();
                        m_beforeAdopt();
                        lock.lock();
                        if (m_stop)
                            return;
                    }
                    // Late: the job is closed, or its chunks are all taken already.
                    if (!m_open || m_nextChunk.load(std::memory_order_relaxed) >= m_job.chunks)
                        continue;
                    m_active.fetch_add(1, std::memory_order_acq_rel);
                    job = m_job;
                }
                work(job);
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_active.fetch_sub(1, std::memory_order_acq_rel);
                }
                m_finished.notify_one();
            }
        }

        std::vector<std::thread> m_workers;
        std::function<void()> m_beforeAdopt;
        std::mutex m_mutex;
        std::condition_variable m_wake;     // workers: a new generation
        std::condition_variable m_finished; // the caller: the last straggler is out
        std::atomic<uint64_t> m_generation{0};
        std::atomic<int> m_nextChunk{0};
        std::atomic<int> m_done{0};
        std::atomic<int> m_active{0}; // workers inside work() for the open job (changed under m_mutex)
        bool m_open = false;
        Job m_job;
        bool m_stop = false;
    };
}
