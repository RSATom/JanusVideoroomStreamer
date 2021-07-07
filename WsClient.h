#pragma once

#include <string>
#include <memory>
#include <functional>

#include <glib.h>

#include "Config.h"
#include "Session.h"


class WsClient
{
public:
    typedef std::function<
        std::unique_ptr<Session> (
            const std::function<void (const char*) noexcept>& sendMessage) noexcept> CreateSession;

    typedef std::function<void () noexcept> Disconnected;

    WsClient(
        const Config&,
        GMainLoop*,
        const CreateSession&,
        const Disconnected&) noexcept;
    bool init() noexcept;
    ~WsClient();

    void connect() noexcept;

private:
    struct Private;
    std::unique_ptr<Private> _p;
};
