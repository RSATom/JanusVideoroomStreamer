#pragma once

enum class MessageType {
    Keepalive,
    CreateSession,
    AttachPlugin,
    Join,
    Publish,
    UnPublish,
    JoinAndConfigure,
    Trickle,
    ListParticipants,
};
