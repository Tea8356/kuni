#pragma once
#include "ITelegramClient.h"

#include <cstdint>
#include <functional>
#include <string>

#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>
#include <td/telegram/td_api.hpp>

#include "AUI/Common/AMap.h"
#include "AUI/Thread/AAsyncHolder.h"
#include "AUI/Thread/AFuture.h"


class ATimer;

/**
 * @brief Concrete implementation of ITelegramClient using TDLib.
 */
class TelegramClientImpl: public ITelegramClient, public AObject {
public:
    struct StubHandler {
        void operator()(auto& v) const { ALOG_TRACE("TelegramClient") << "Stub: " << to_string(v); }
    };
    TelegramClientImpl();

    AFuture<Object> sendQuery(td::td_api::object_ptr<td::td_api::Function> f) override;

    [[nodiscard]]
    const AFuture<>& waitForConnection() const noexcept override {
        return mWaitForConnection;
    }

    [[nodiscard]] int64_t myId() const override { return mMyId; }

private:
    AFuture<> mWaitForConnection;
    _<ATimer> mTgUpdateTimer;
    std::unique_ptr<td::ClientManager> mClientManager;
    td::ClientManager::ClientId mClientId{};
    AMap<std::uint64_t, std::function<void(Object)>> mHandlers;
    size_t mQueryCountLastUpdate{};
    size_t mCurrentQueryId{};
    int64_t mMyId{};

    /**
     * @brief td_api ID of the authorization state we are currently asking the user about, 0 if none.
     * @details Guards against queuing duplicate console prompts when tdlib re-emits the same authorization state.
     */
    std::int32_t mPendingAuthPromptState{};

    /**
     * @brief Re-asks the last authorization question; used when Telegram rejects the user's input.
     */
    std::function<void()> mRetryAuthPrompt;

    void update();
    void initClientManager();

    /**
     * @brief Sends an authorization-related query, logging whatever Telegram replies (including errors).
     */
    void sendAuthQuery(td::td_api::object_ptr<td::td_api::Function> f, std::string description);

    /**
     * @brief Asks the user for a line of input without blocking the UI thread, then runs action with the answer.
     */
    void promptAuth(std::int32_t stateId, std::string prompt, std::function<void(const std::string&)> action);

    void commonHandler(td::tl::unique_ptr<td::td_api::Object> object);
    void processResponse(td::ClientManager::Response response);
};
