#ifdef _WIN32
#include "FileWatcher.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

#include <AUI/Thread/AThread.h>

namespace util {

/**
 * @brief Windows implementation of FileWatcher based on polling.
 * @details
 * Kuni watches a handful of small text files (config.toml, prompts) to hot-reload them. A one-second poll of the last
 * write time is precise enough for that and avoids the ReadDirectoryChangesW machinery, which behaves differently on
 * network shares, WSL-mounted paths and OneDrive-backed folders - all common setups on Windows.
 */
struct FileWatcher::Impl {
    struct Watch {
        std::filesystem::path path;
        std::filesystem::file_time_type lastWriteTime {};
    };

    int nextWd = 1;
    std::map<int, Watch> watches;
    std::mutex mutex;
    std::atomic_bool running { true };
    FileWatcher* parent = nullptr;
    std::thread thread;

    ~Impl() {
        running = false;
        if (thread.joinable()) {
            thread.join();
        }
    }

    void run() {
        using namespace std::chrono_literals;
        while (running) {
            std::this_thread::sleep_for(1s);

            std::map<int, Watch> snapshot;
            {
                std::lock_guard lock(mutex);
                snapshot = watches;
            }

            for (auto& [wd, watch] : snapshot) {
                std::error_code ec;
                auto time = std::filesystem::last_write_time(watch.path, ec);
                if (ec || time == watch.lastWriteTime) {
                    continue;
                }
                {
                    std::lock_guard lock(mutex);
                    if (auto it = watches.find(wd); it != watches.end()) {
                        it->second.lastWriteTime = time;
                    }
                }
                if (parent != nullptr) {
                    (*parent) ^ parent->fired(Event { wd });
                }
            }
        }
    }
};

FileWatcher::FileWatcher() : mImpl(std::make_unique<Impl>()) {
    mImpl->parent = this;
    mImpl->thread = std::thread([impl = mImpl.get()] { impl->run(); });
}

FileWatcher::~FileWatcher() = default;

int FileWatcher::addWatch(const APath& path, Mask mask) {
    std::filesystem::path fsPath(path.absolute().toStdString());

    std::error_code ec;
    auto time = std::filesystem::last_write_time(fsPath, ec);

    std::lock_guard lock(mImpl->mutex);
    int wd = mImpl->nextWd++;
    mImpl->watches[wd] = Impl::Watch { .path = std::move(fsPath), .lastWriteTime = ec ? std::filesystem::file_time_type {} : time };
    return wd;
}

}   // namespace util
#endif
