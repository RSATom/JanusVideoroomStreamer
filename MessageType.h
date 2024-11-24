#pragma once

enum class MessageType {
    Keepalive,
    CreateSession,
    AttachPlugin,
    Features,
    Watch,
    Start,
    Trickle,
};
