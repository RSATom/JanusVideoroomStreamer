#pragma once

enum class MessageType {
    Keepalive,
    CreateSession,
    AttachPlugin,
    JoinAndConfigure,
    Trickle,
};
