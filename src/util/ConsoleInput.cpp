#include "ConsoleInput.h"

#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

#include "AUI/Common/ALogger.h"
#include "AUI/Thread/AThread.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
constexpr auto LOG_TAG = "ConsoleInput";

std::string trim(std::string s) {
    constexpr auto WHITESPACE = " \t\r\n\v\f";
    const auto begin = s.find_first_not_of(WHITESPACE);
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = s.find_last_not_of(WHITESPACE);
    return s.substr(begin, end - begin + 1);
}
}   // namespace

namespace util {

struct ConsoleInput::Impl {
    struct Request {
        std::string prompt;
        std::function<void(std::string)> callback;
        _<AAbstractThread> callerThread;
    };

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Request> queue;
    bool eof = false;

    void run() {
        for (;;) {
            Request request;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return !queue.empty(); });
                request = std::move(queue.front());
                queue.pop_front();
            }

            std::cout << request.prompt << std::flush;

            std::string line;
            if (!std::getline(std::cin, line)) {
                std::unique_lock lock(mutex);
                eof = true;
                ALogger::info(LOG_TAG)
                    << "stdin is closed (EOF); cannot read user input. If you run Kuni in Docker, make sure the "
                       "container is started interactively (docker run -it / stdin_open: true and tty: true in "
                       "docker-compose.yml).";
                continue;
            }

            auto value = trim(std::move(line));
            if (auto thread = std::move(request.callerThread)) {
                thread->enqueue([callback = std::move(request.callback), value = std::move(value)]() mutable {
                    callback(std::move(value));
                });
            } else {
                request.callback(std::move(value));
            }
        }
    }
};

ConsoleInput::ConsoleInput() : mImpl(new Impl) {
#ifdef _WIN32
    // Windows consoles default to a legacy code page, which turns prompts and Russian log output into mojibake and
    // mangles anything non-ASCII typed by the user.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    // detached daemon thread: it outlives the app and is reaped on process exit. It must never be joined because it
    // spends most of its lifetime inside a blocking std::getline.
    std::thread([impl = mImpl] { impl->run(); }).detach();
}

ConsoleInput& ConsoleInput::inst() {
    static ConsoleInput instance;
    return instance;
}

void ConsoleInput::requestLine(std::string prompt, std::function<void(std::string)> callback) {
    {
        std::unique_lock lock(mImpl->mutex);
        if (mImpl->eof) {
            ALogger::err(LOG_TAG) << "Ignoring input request (stdin is closed): " << prompt;
            return;
        }
        mImpl->queue.push_back(Impl::Request {
          .prompt = std::move(prompt),
          .callback = std::move(callback),
          .callerThread = AThread::current(),
        });
    }
    mImpl->cv.notify_one();
}

}   // namespace util
