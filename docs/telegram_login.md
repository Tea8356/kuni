# Telegram login (tdlib authorization)

This document describes how Kuni logs into Telegram and how to debug it. It exists because the flow used to fail
silently - see issue #68.

## How it works

`TelegramClientImpl` polls tdlib once a second from the UI thread (`TelegramClientImpl::update`) and reacts to
`updateAuthorizationState`. Every state transition is logged, so the log always shows where the login is stuck:

```
[Authentication] state: authorizationStateWaitPhoneNumber
[Authentication] input required: Enter phone number in international format (e.g. +79001234567):
[Authentication] setAuthenticationPhoneNumber accepted by Telegram.
[Authentication] state: authorizationStateWaitCode
[Authentication] where to look for the code: authenticationCodeTypeTelegramMessage
[Authentication] Enter the login code Telegram has sent you:
[Authentication] logged in.
```

Handled states: phone number, login code, cloud (2FA) password, email address, email code, registration of a new
number, confirmation from another device (QR link) and logout/closed.

## Why input never blocks the client

User input is read by `util::ConsoleInput` on a dedicated thread, and the answer is delivered back to the UI thread.
This is not a stylistic choice: the UI thread drives the tdlib receive loop, so reading `std::cin` there froze the
whole Telegram pipeline. The next authorization state (`authorizationStateWaitCode`) was never processed and the
client went quiet right after the phone number was entered - the exact symptom reported in #68.

Consequences worth knowing:

- prompts and tdlib updates are interleaved in the log; the prompt line may be followed by unrelated log output while
  you type. Just keep typing - the input is still being read.
- an empty line is rejected and the question is asked again.

## Errors are always reported

Authorization queries are sent through `TelegramClientImpl::sendAuthQuery`, which logs Telegram's reply, including
errors. Previously they went through a coroutine whose result was discarded, so rejections were silently swallowed.

Common errors:

| Error | Meaning |
|---|---|
| `PHONE_NUMBER_INVALID` | wrong format; use the international form, e.g. `+79001234567` |
| `PHONE_CODE_INVALID` / `PHONE_CODE_EXPIRED` | wrong or stale code; Kuni asks again automatically |
| `PASSWORD_HASH_INVALID` | wrong cloud password |
| `FLOOD_WAIT_x` | Telegram throttled this number for `x` seconds; wait it out, retrying makes it worse |
| `API_ID_INVALID` / `API_ID_PUBLISHED_FLOOD` | bad or overused `telegram_api_id` / `telegram_api_hash` |
| `AUTH_KEY_UNREGISTERED`, `SESSION_REVOKED` | the session was killed from another device; delete the `tdlib` directory and log in again |

After a rejected input Kuni re-asks the same question instead of going silent.

## Debugging tools

- `KUNI_TDLIB_VERBOSITY=3 ./kuni` - tdlib's own logs (`5` and above for debug). No rebuild required.
- `AUI_TRACE=1 ./kuni` - Kuni's trace log, including every query sent to tdlib.
- The `tdlib` directory next to the executable holds the session. Delete it to start the login from scratch.

## Docker

stdin must be attached, otherwise Kuni cannot ask anything and logs a warning about EOF:

- `docker run -it ...`
- or `stdin_open: true` and `tty: true` for the service in `docker-compose.yml`.
