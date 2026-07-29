#pragma once

#include <functional>
#include <string>

namespace util {

/**
 * @brief Non-blocking stdin reader.
 * @details
 * Reads lines from stdin on a dedicated background thread.
 *
 * Blocking on `std::cin` from the UI thread is fatal for Kuni: the UI thread pumps tdlib updates
 * (TelegramClientImpl::update), so any blocking read freezes the whole Telegram event pipeline. That is exactly what
 * used to happen during the authorization flow - while the user was typing the phone number, tdlib updates (including
 * the follow-up `authorizationStateWaitCode`) were not processed, and the client appeared to hang silently.
 *
 * Requests are queued in FIFO order; the callback is invoked on the thread that issued the request.
 */
class ConsoleInput {
public:
    static ConsoleInput& inst();

    /**
     * @brief Requests a single line of user input.
     * @param prompt human readable prompt printed to stdout before reading.
     * @param callback invoked with the trimmed line, on the calling thread.
     * @details
     * Returns immediately. If stdin is closed (EOF), the callback is never called and a warning is logged.
     */
    void requestLine(std::string prompt, std::function<void(std::string)> callback);

private:
    ConsoleInput();
    ~ConsoleInput() = default;

    struct Impl;
    Impl* mImpl;
};

}   // namespace util
