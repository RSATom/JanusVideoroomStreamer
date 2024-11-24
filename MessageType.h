#pragma once

enum class MessageType {
    Keepalive,
    CreateSession,
    AttachPlugin,
    Watch,
    Start,
    Trickle,
};
