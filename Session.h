#pragma once

#include "CxxPtr/GlibPtr.h"
#include "CxxPtr/JanssonPtr.h"

#include "Config.h"
#include "RtcStreaming/WebRTCPeer.h"

#include "MessageType.h"


class Session
{
public:
    Session(
        const Config*,
        const std::function<std::unique_ptr<WebRTCPeer> ()>& createPeer,
        const std::function<void (const char*)>& sendMessage) noexcept;
    ~Session();

    bool onConnected() noexcept;

    bool handleMessage(const JsonPtr&) noexcept;

private:
    void disconnect();
    void sendMessage(const JsonPtr&);
    void sendMessage(MessageType, const JsonPtr&);
    void checkTimeout();
    void updateParticipants();

    void sendKeepalive();

    void sendCreateSession();
    bool handleCreateSessionReply(const JsonPtr&);

    void sendAttachPlugin();
    bool handleAttachPluginReply(const JsonPtr&);

    void sendJoin();
    bool handleJoinReply(const JsonPtr&);

    void sendPublish(const std::string& sdp);
    bool handlePublishReply(const JsonPtr&);

    // void sendJoinAndConfigure(const std::string& sdp);
    // bool handleJoinAndConfigureReply(const JsonPtr&);

    void sendUnPublish();
    bool handleUnPublishReply(const JsonPtr&);

    void sendTrickle(unsigned mlineIndex, const std::string& candidate);
    bool handleTrickleReply(const JsonPtr&);

    void sendListParticipants();
    bool handleListParticipantsReply(const JsonPtr&);

    bool handleEvent(const JsonPtr&);

    void streamerPrepared();
    void iceCandidate(unsigned mlineIndex, const std::string& candidate);
    void eos();

    void startStream();
    void stopStream();

private:
    const Config *const _config;
    const std::function<std::unique_ptr<WebRTCPeer> ()> _createPeer;
    const std::function<void (const char*)> _sendMessage;

    int _nextTransaction = 1;

    std::map<std::string, MessageType> _sentMessages;

    guint _keepaliveTimeout = 0;
    GTimerPtr _lastMessageTimer;

    guint _updateParticipantsTimeout = 0;

    json_int_t _session = 0;
    json_int_t _handleId = 0;

    std::unique_ptr<WebRTCPeer> _streamerPtr;
};
