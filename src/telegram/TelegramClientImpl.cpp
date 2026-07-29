//
// Created by alex2772 on 3/2/26.
//

#include "TelegramClientImpl.h"

#include <cstdlib>
#include <string>

#include "config.h"
#include "AUI/Common/ATimer.h"
#include "AUI/Util/kAUI.h"
#include "util/ConsoleInput.h"

using namespace std::chrono_literals;

namespace {
static constexpr auto LOG_TAG = "TelegramClient";
}   // namespace

TelegramClientImpl::TelegramClientImpl() : mTgUpdateTimer(_new<ATimer>(1s)) {
    ALOG_TRACE(LOG_TAG) << "TelegramClientImpl::TelegramClientImpl";
    setSlotsCallsOnlyOnMyThread(true);

    // tdlib is very quiet by default which makes login issues impossible to debug. Allow bumping verbosity without a
    // rebuild: KUNI_TDLIB_VERBOSITY=3 ./kuni (0 = fatal only, 1 = errors, 2 = warnings, 3 = info, 5+ = debug).
    int tdlibVerbosity = 1;
    if (const char* env = std::getenv("KUNI_TDLIB_VERBOSITY")) {
        try {
            tdlibVerbosity = std::stoi(env);
        } catch (...) {
            ALogger::info(LOG_TAG) << "KUNI_TDLIB_VERBOSITY is not a number: " << env;
        }
    }
    td::ClientManager::execute(td::td_api::make_object<td::td_api::setLogVerbosityLevel>(tdlibVerbosity));
    initClientManager();

    AObject::connect(mTgUpdateTimer->fired, me::update);
    mTgUpdateTimer->start();
}

AFuture<ITelegramClient::Object> TelegramClientImpl::sendQuery(td::td_api::object_ptr<td::td_api::Function> f) {
    ALOG_TRACE(LOG_TAG) << "sendQuery " << td::td_api::to_string(f);
    if (mQueryCountLastUpdate++ >= 20) {
        // Telegram is strict about using 3rdparty telegram clients. For this reason, we have to ensure that we wouldn't
        // trigger their security leading to ban of the account.
        ALogger::info(LOG_TAG) << "Too many calls to tdlib! Throttling...\n" << AStacktrace::capture(1, 8);
        co_await AThread::asyncSleep(1s);
    }

    auto query_id = ++mCurrentQueryId;
    AFuture<ITelegramClient::Object> result;
    mHandlers.emplace(query_id, [result](Object object) { result.supplyValue(std::move(object)); });
    mClientManager->send(mClientId, query_id, std::move(f));
    co_return co_await result;
}

void TelegramClientImpl::sendAuthQuery(td::td_api::object_ptr<td::td_api::Function> f, std::string description) {
    ALOG_TRACE(LOG_TAG) << "sendAuthQuery " << description;
    auto queryId = ++mCurrentQueryId;

    // NOTE: authorization queries deliberately bypass sendQuery(). sendQuery() is a coroutine whose AFuture used to be
    // discarded at the call site, so tdlib errors (PHONE_NUMBER_INVALID, PHONE_CODE_INVALID, FLOOD_WAIT, ...) were
    // silently swallowed and the login just went quiet - see issue #68. Here the result is always reported.
    mHandlers.emplace(queryId, [this, self = shared_from_this(), description](Object object) {
        if (object && object->get_id() == td::td_api::error::ID) {
            auto& error = static_cast<td::td_api::error&>(*object);
            ALogger::err(LOG_TAG) << "[Authentication] " << description << " failed: " << error.code_ << " "
                                  << error.message_;
            if (error.code_ == 400 && mPendingAuthPromptState == 0 && mRetryAuthPrompt) {
                // bad input (wrong code/password/phone): tdlib does not re-emit the authorization state, so we have to
                // ask the user again ourselves.
                auto retry = mRetryAuthPrompt;
                retry();
            }
            return;
        }
        ALogger::info(LOG_TAG) << "[Authentication] " << description << " accepted by Telegram.";
    });
    mClientManager->send(mClientId, queryId, std::move(f));
}

void TelegramClientImpl::promptAuth(
    std::int32_t stateId, std::string prompt, std::function<void(const std::string&)> action) {
    if (mPendingAuthPromptState == stateId) {
        // the very same question is already waiting for an answer; don't queue a duplicate.
        return;
    }
    mPendingAuthPromptState = stateId;
    mRetryAuthPrompt = [this, self = shared_from_this(), stateId, prompt, action] {
        mPendingAuthPromptState = 0;
        promptAuth(stateId, prompt, action);
    };

    ALogger::info(LOG_TAG) << "[Authentication] input required: " << prompt;
    util::ConsoleInput::inst().requestLine(
        "[Authentication] " + prompt, [this, self = shared_from_this(), stateId, action](const std::string& value) {
            if (mPendingAuthPromptState == stateId) {
                mPendingAuthPromptState = 0;
            }
            if (value.empty()) {
                ALogger::info(LOG_TAG) << "[Authentication] empty input, asking again.";
                if (mRetryAuthPrompt) {
                    auto retry = mRetryAuthPrompt;
                    retry();
                }
                return;
            }
            action(value);
        });
}

void TelegramClientImpl::initClientManager() {
    ALOG_TRACE(LOG_TAG) << "initClientManager";
    mClientManager = std::make_unique<td::ClientManager>();
    mClientId = mClientManager->create_client_id();
    sendQueryWithResult(td::td_api::make_object<td::td_api::getOption>("version"))
        .onSuccess([](const td::td_api::object_ptr<td::td_api::OptionValue>& object) {
            td::td_api::downcast_call(
                *const_cast<td::td_api::object_ptr<td::td_api::OptionValue>&>(object),
                aui::lambda_overloaded {
                  [](const td::td_api::optionValueString& u) { ALogger::info(LOG_TAG) << "Tdlib version: " << u.value_; },
                  [](auto&) {} });
        });
}

void TelegramClientImpl::update() {
    ALOG_TRACE(LOG_TAG) << "update";

    switch (connectionState) {
        case ConnectionState::INITIALIZING:
            ALogger::info(LOG_TAG) << "Connection state: initializing...";
            break;
        case ConnectionState::CONNECTED:
            break;
        case ConnectionState::CONNECTING:
            ALogger::info(LOG_TAG) << "Connection state: connecting... (check VPN/proxy settings)";
            break;
        case ConnectionState::CONNECTING_TO_PROXY:
            ALogger::info(LOG_TAG) << "Connection state: connecting to proxy...";
            break;
        case ConnectionState::UPDATING:
            ALogger::info(LOG_TAG) << "Connection state: updating...";
            break;
        case ConnectionState::WAITING_FOR_NETWORK:
            ALogger::info(LOG_TAG) << "Connection state: waiting for network...";
            break;
    }

    mQueryCountLastUpdate = 0;
    for (;;) {
        auto response = mClientManager->receive(0);
        if (!response.object) {
            return;
        }
        processResponse(std::move(response));
    }
}

void TelegramClientImpl::processResponse(td::ClientManager::Response response) {
    ALOG_TRACE(LOG_TAG) << "processResponse";
    if (!response.object) {
        return;
    }

    if (auto c = mHandlers.contains(response.request_id)) {
        auto handler = std::move(c->second);
        mHandlers.erase(*c);
        handler(std::move(response.object));
        return;
    }

    commonHandler(std::move(response.object));
}

void TelegramClientImpl::commonHandler(td::tl::unique_ptr<td::td_api::Object> object) {
    ALOG_TRACE(LOG_TAG) << "commonHandler";
    // move the ownership from unique_ptr to shared_ptr
    auto objectShared = aui::ptr::manage_shared(object.release());
    emit onEvent(objectShared);
    td::td_api::downcast_call(
        *objectShared,
        aui::lambda_overloaded {
          [this](td::td_api::updateAuthorizationState& update_authorization_state) {
              // always report the state: silent authorization stalls are impossible to debug otherwise (see #68).
              ALogger::info(LOG_TAG) << "[Authentication] state: "
                                     << td::td_api::to_string(update_authorization_state.authorization_state_);
              td::td_api::downcast_call(
                  *update_authorization_state.authorization_state_,
                  aui::lambda_overloaded {
                    [this](td::td_api::authorizationStateWaitTdlibParameters& u) {
                        auto parameters = td::td_api::make_object<td::td_api::setTdlibParameters>();
                        parameters->database_directory_ = "tdlib";
                        parameters->use_message_database_ = true;
                        parameters->use_secret_chats_ = true;

                        parameters->api_id_ = config().telegramApiId;
                        parameters->api_hash_ = config().telegramApiHash;
                        parameters->system_language_code_ = "en";
                        parameters->device_model_ = "Desktop";
                        parameters->application_version_ = AUI_PP_STRINGIZE(AUI_CMAKE_PROJECT_VERSION);
                        sendAuthQuery(std::move(parameters), "setTdlibParameters");
                    },
                    [this](td::td_api::authorizationStateReady& u) {
                        ALogger::info(LOG_TAG) << "[Authentication] logged in.";
                        mPendingAuthPromptState = 0;
                        emit loggedIn;
                    },
                    [this](td::td_api::authorizationStateWaitPhoneNumber& s) {
                        promptAuth(
                            td::td_api::authorizationStateWaitPhoneNumber::ID,
                            "Enter phone number in international format (e.g. +79001234567): ",
                            [this](const std::string& value) {
                                auto params = td::td_api::make_object<td::td_api::setAuthenticationPhoneNumber>();
                                params->phone_number_ = value;

                                // tdlib accepts a null `settings`, but being explicit avoids surprises with newer
                                // tdlib versions and disables the call-based flows we can't handle headlessly.
                                auto settings =
                                    td::td_api::make_object<td::td_api::phoneNumberAuthenticationSettings>();
                                settings->allow_flash_call_ = false;
                                settings->allow_missed_call_ = false;
                                settings->is_current_phone_number_ = false;
                                settings->allow_sms_retriever_api_ = false;
                                params->settings_ = std::move(settings);

                                sendAuthQuery(std::move(params), "setAuthenticationPhoneNumber");
                            });
                    },
                    [this](td::td_api::authorizationStateWaitPassword& s) {
                        if (!s.password_hint_.empty()) {
                            ALogger::info(LOG_TAG) << "[Authentication] password hint: " << s.password_hint_;
                        }
                        promptAuth(
                            td::td_api::authorizationStateWaitPassword::ID,
                            "Enter your cloud (2FA) password: ",
                            [this](const std::string& value) {
                                auto params = td::td_api::make_object<td::td_api::checkAuthenticationPassword>();
                                params->password_ = value;
                                sendAuthQuery(std::move(params), "checkAuthenticationPassword");
                            });
                    },
                    [this](td::td_api::authorizationStateWaitCode& s) {
                        if (s.code_info_) {
                            ALogger::info(LOG_TAG)
                                << "[Authentication] where to look for the code: "
                                << td::td_api::to_string(s.code_info_);
                        }
                        promptAuth(
                            td::td_api::authorizationStateWaitCode::ID,
                            "Enter the login code Telegram has sent you: ",
                            [this](const std::string& value) {
                                auto params = td::td_api::make_object<td::td_api::checkAuthenticationCode>();
                                params->code_ = value;
                                sendAuthQuery(std::move(params), "checkAuthenticationCode");
                            });
                    },
                    [this](td::td_api::authorizationStateWaitEmailAddress& s) {
                        promptAuth(
                            td::td_api::authorizationStateWaitEmailAddress::ID,
                            "Enter the email address linked to your Telegram account: ",
                            [this](const std::string& value) {
                                auto params = td::td_api::make_object<td::td_api::setAuthenticationEmailAddress>();
                                params->email_address_ = value;
                                sendAuthQuery(std::move(params), "setAuthenticationEmailAddress");
                            });
                    },
                    [this](td::td_api::authorizationStateWaitEmailCode& s) {
                        promptAuth(
                            td::td_api::authorizationStateWaitEmailCode::ID,
                            "Enter the code sent to your email: ",
                            [this](const std::string& value) {
                                auto params = td::td_api::make_object<td::td_api::checkAuthenticationEmailCode>();
                                params->code_ = td::td_api::make_object<td::td_api::emailAddressAuthenticationCode>(
                                    value);
                                sendAuthQuery(std::move(params), "checkAuthenticationEmailCode");
                            });
                    },
                    [this](td::td_api::authorizationStateWaitOtherDeviceConfirmation& s) {
                        ALogger::info(LOG_TAG)
                            << "[Authentication] Telegram requires confirmation from another device. Open Telegram on "
                               "your phone -> Settings -> Devices -> Link Desktop Device and scan this link as a QR "
                               "code: "
                            << s.link_;
                    },
                    [this](td::td_api::authorizationStateWaitRegistration& s) {
                        promptAuth(
                            td::td_api::authorizationStateWaitRegistration::ID,
                            "This phone number is not registered in Telegram. Enter first name (or Ctrl+C to abort): ",
                            [this](const std::string& firstName) {
                                util::ConsoleInput::inst().requestLine(
                                    "[Authentication] Enter last name (may be empty): ",
                                    [this, firstName](const std::string& lastName) {
                                        auto params = td::td_api::make_object<td::td_api::registerUser>();
                                        params->first_name_ = firstName;
                                        params->last_name_ = lastName;
                                        sendAuthQuery(std::move(params), "registerUser");
                                    });
                            });
                    },
                    [this](td::td_api::authorizationStateLoggingOut& u) {
                        ALogger::info(LOG_TAG) << "[Authentication] logging out...";
                        mPendingAuthPromptState = 0;
                    },
                    [this](td::td_api::authorizationStateClosed& u) {
                        ALogger::info(LOG_TAG) << "[Authentication] session closed, restarting tdlib client...";
                        mPendingAuthPromptState = 0;
                        getThread()->enqueue([this, self = shared_from_this()] { initClientManager(); });
                    },
                    [this](auto& v) { ALogger::info(LOG_TAG) << "Stub: " << td::td_api::to_string(v); },
                  });
          },

          [this](td::td_api::updateConnectionState& u) {
              td::td_api::downcast_call(
                  *u.state_,
                  aui::lambda_overloaded {
                    [&](td::td_api::connectionStateReady&) {
                        connectionState = ConnectionState::CONNECTED;
                        ALogger::info(LOG_TAG) << "Connection state: connected";
                        mWaitForConnection.supplyValue();
                    },
                    [&](td::td_api::connectionStateConnecting&) { connectionState = ConnectionState::CONNECTING; },
                    [&](td::td_api::connectionStateConnectingToProxy&) {
                        connectionState = ConnectionState::CONNECTING_TO_PROXY;
                    },
                    [&](td::td_api::connectionStateWaitingForNetwork&) {
                        connectionState = ConnectionState::WAITING_FOR_NETWORK;
                    },

                    [&](td::td_api::connectionStateUpdating&) { connectionState = ConnectionState::UPDATING; },
                  });
          },
          [this](td::td_api::updateOption& u) {
              if (u.name_ == "my_id") {
                  td::td_api::downcast_call(
                      *u.value_,
                      aui::lambda_overloaded {
                        [&](td::td_api::optionValueInteger& i) { mMyId = i.value_; },
                        [&](auto&) {},
                      });
              }
          },
          // ── User cache updates ──────────────────────────────────────────
          [this](td::td_api::updateUser& u) {
              if (auto dst = mUserCache.contains(u.user_->id_)) {
                  dst->second->tg = std::move(*u.user_);
              }
          },
          [this](td::td_api::updateUserStatus& u) {
              if (auto dst = mUserCache.contains(u.user_id_)) {
                  dst->second->tg.status_ = std::move(u.status_);
              }
          },
          // ── Chat cache updates ───────────────────────────────────────────
          [this](td::td_api::updateChatTitle& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.title_ = std::move(u.title_);
              }
          },
          [this](td::td_api::updateChatPhoto& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.photo_ = std::move(u.photo_);
              }
          },
          [this](td::td_api::updateChatPermissions& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.permissions_ = std::move(u.permissions_);
              }
          },
          [this](td::td_api::updateChatLastMessage& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.last_message_ = std::move(u.last_message_);
              }
          },
          [this](td::td_api::updateChatReadInbox& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.last_read_inbox_message_id_ = u.last_read_inbox_message_id_;
                  dst->second->tg.unread_count_ = u.unread_count_;
              }
          },
          [this](td::td_api::updateChatReadOutbox& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.last_read_outbox_message_id_ = u.last_read_outbox_message_id_;
              }
          },
          [this](td::td_api::updateChatUnreadMentionCount& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.unread_mention_count_ = u.unread_mention_count_;
              }
          },
          [this](td::td_api::updateChatUnreadReactionCount& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.unread_reaction_count_ = u.unread_reaction_count_;
              }
          },
          [this](td::td_api::updateChatNotificationSettings& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.notification_settings_ = std::move(u.notification_settings_);
              }
          },
          [this](td::td_api::updateChatIsMarkedAsUnread& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.is_marked_as_unread_ = u.is_marked_as_unread_;
              }
          },
          [this](td::td_api::updateChatBlockList& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.block_list_ = std::move(u.block_list_);
              }
          },
          [this](td::td_api::updateChatHasScheduledMessages& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.has_scheduled_messages_ = u.has_scheduled_messages_;
              }
          },
          [this](td::td_api::updateChatDraftMessage& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.draft_message_ = std::move(u.draft_message_);
              }
          },
          [this](td::td_api::updateChatMessageAutoDeleteTime& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.message_auto_delete_time_ = u.message_auto_delete_time_;
              }
          },
          [this](td::td_api::updateChatEmojiStatus& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.emoji_status_ = std::move(u.emoji_status_);
              }
          },
          [this](td::td_api::updateChatBackground& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.background_ = std::move(u.background_);
              }
          },
          [this](td::td_api::updateChatTheme& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.theme_ = std::move(u.theme_);
              }
          },
          [this](td::td_api::updateChatReplyMarkup& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.reply_markup_message_id_ = u.reply_markup_message_->id_;
              }
          },
          [this](td::td_api::updateMessageSendSucceeded& u) {
              const auto oldId = u.old_message_id_;
              const auto newId = u.message_->id_;
              auto& cacheForThisChat = mMessageCache[u.message_->chat_id_];
              auto dst = cacheForThisChat[newId] = cacheForThisChat[oldId];
              if (dst == nullptr) {
                  dst = cacheForThisChat[newId] = cacheForThisChat[oldId] = _new<Cached<td::td_api::message>>();
              }
              dst->tg = std::move(*u.message_);
              if (!dst->populated.hasResult()) {
                  dst->populated.supplyValue();
              }
          },
          [this](td::td_api::updateChatPosition& u) {
              auto chat = getChat(u.chat_id_);
              if (!chat.hasValue()) {
                  return;
              }
              for (auto& i : (*chat)->positions_) {
                  if (i->list_->get_id() == u.position_->list_->get_id()) {
                      i = std::move(u.position_);
                      return;
                  }
              }
              (*chat)->positions_.push_back(std::move(u.position_));
          },
          [this](td::td_api::updateMessageSendFailed& u) {
              const auto oldId = u.old_message_id_;
              const auto newId = u.message_->id_;
              auto& cacheForThisChat = mMessageCache[u.message_->chat_id_];
              auto dst = cacheForThisChat[newId] = cacheForThisChat[oldId];
              if (dst == nullptr) {
                  dst = cacheForThisChat[newId] = cacheForThisChat[oldId] = _new<Cached<td::td_api::message>>();
              }
              dst->tg = std::move(*u.message_);
              if (!dst->populated.hasResult()) {
                  dst->populated.supplyValue();
              }
          },
          [&](auto& i) {},
        });
}