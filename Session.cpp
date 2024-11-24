#include "Session.h"

#include <cassert>

#include "CxxPtr/CPtr.h"
#include "CxxPtr/JanssonPtr.h"

#include "RtStreaming/GstRtStreaming/GstClient.h"


namespace {

enum {
    KEEPALIVE_TIMEOUT = 30,
    TIMEOUT_CHECK_INTERVAL = 15,
};

char const * const Plugin = "janus.plugin.ustreamer";

std::string ExtractString(json_t* json, const char* name)
{
    json_t* valueJson = json_object_get(json, name);
    if(valueJson && json_is_string(valueJson))
        return json_string_value(valueJson);

    return std::string();
}

json_int_t ExtractInt(json_t* json, const char* name)
{
    json_t* valueJson = json_object_get(json, name);
    if(valueJson && json_is_integer(valueJson))
        return json_integer_value(valueJson);

    return 0;
}

inline std::string ExtractTransaction(const JsonPtr& jsonMessagePtr)
{
    return ExtractString(jsonMessagePtr.get(), "transaction");
}

inline json_int_t ExtractSession(const JsonPtr& jsonMessagePtr)
{
    return ExtractInt(jsonMessagePtr.get(), "session_id");
}

inline std::string ExtractJanus(const JsonPtr& jsonMessagePtr)
{
    return ExtractString(jsonMessagePtr.get(), "janus");
}

}

Session::Session(
    const Config* config,
    const std::function<void (const char*)>& sendMessage) noexcept:
    _config(config), _sendMessage(sendMessage),
    _lastMessageTimer(g_timer_new()),
    _viewPeerPtr(std::make_unique<GstClient>())
{
    const GSourceFunc timeoutCallback =
        [] (gpointer userData) -> gboolean {
            static_cast<Session*>(userData)->checkTimeout();
            return TRUE;
        };

    _keepaliveTimeout =
        g_timeout_add_seconds(
            TIMEOUT_CHECK_INTERVAL,
            timeoutCallback, this);
}

Session::~Session()
{
    g_source_remove(_keepaliveTimeout);
}

void Session::disconnect()
{
    _sendMessage(nullptr);
}

void Session::sendMessage(const JsonPtr& jsonMessagePtr)
{
    CharPtr messagePtr(json_dumps(jsonMessagePtr.get(), JSON_INDENT(2)));

    g_timer_reset(_lastMessageTimer.get());

    _sendMessage(messagePtr.get());
}

void Session::sendMessage(MessageType messageType, const JsonPtr& jsonMessagePtr)
{
    const std::string transaction = ExtractTransaction(jsonMessagePtr);
    if(!transaction.empty())
        _sentMessages.emplace(transaction, messageType);

    CharPtr messagePtr(json_dumps(jsonMessagePtr.get(), JSON_INDENT(2)));

    g_timer_reset(_lastMessageTimer.get());

    _sendMessage(messagePtr.get());
}

void Session::sendKeepalive()
{
    JsonPtr jsonMessagePtr(json_object());
    json_t* jsonMessage = jsonMessagePtr.get();

    json_object_set_new(
        jsonMessage,
        "transaction", json_string(std::to_string(_nextTransaction++).c_str()));
    json_object_set_new(jsonMessage, "session_id", json_integer(_session));
    json_object_set_new(jsonMessage, "janus", json_string("keepalive"));

    sendMessage(MessageType::Keepalive, jsonMessagePtr);
}

bool Session::onConnected() noexcept
{
    sendCreateSession();

    g_timer_start(_lastMessageTimer.get());

    return true;
}

void Session::checkTimeout()
{
    if(g_timer_elapsed(_lastMessageTimer.get(), nullptr) > KEEPALIVE_TIMEOUT)
        sendKeepalive();
}

bool Session::handleMessage(const JsonPtr& jsonMessagePtr) noexcept
{
    const std::string transaction = ExtractTransaction(jsonMessagePtr);
    if(!transaction.empty()) {
        const auto it = _sentMessages.find(transaction);
        if(it != _sentMessages.end()) {
            if(ExtractJanus(jsonMessagePtr) == "ack") {
                switch(it->second) {
                case MessageType::Keepalive:
                case MessageType::Trickle:
                    // any other reply is not expected for such message types
                    _sentMessages.erase(it);
                default:
                    break;
                }

                return true;
            }

            _sentMessages.erase(it);

            switch(it->second) {
            case MessageType::CreateSession:
                return handleCreateSessionReply(jsonMessagePtr);
            case MessageType::AttachPlugin:
                return handleAttachPluginReply(jsonMessagePtr);
            case MessageType::Features:
                return handleFeaturesReply(jsonMessagePtr);
            case MessageType::Watch:
                return handleWatchReply(jsonMessagePtr);
            case MessageType::Start:
                return handleStartReply(jsonMessagePtr);
            case MessageType::Trickle:
                return handleTrickleReply(jsonMessagePtr);
            default:
                break;
            }
        }

        return false;
    } else
        return handleEvent(jsonMessagePtr);
}

void Session::sendCreateSession()
{
    JsonPtr jsonMessagePtr(json_object());
    json_t* jsonMessage = jsonMessagePtr.get();

    json_object_set_new(
        jsonMessage,
        "transaction", json_string(std::to_string(_nextTransaction++).c_str()));
    json_object_set_new(jsonMessage, "janus", json_string("create"));

    sendMessage(MessageType::CreateSession, jsonMessagePtr);
}

bool Session::handleCreateSessionReply(const JsonPtr& jsonMessagePtr)
{
    if(_session != 0)
        return false;

    if(ExtractJanus(jsonMessagePtr) != "success")
        return false;

    json_t* jsonMessage = jsonMessagePtr.get();

    json_t* dataJson = json_object_get(jsonMessage, "data");
    if(!dataJson)
        return false;

    _session = ExtractInt(dataJson, "id");
    if(!_session)
        return false;

    sendAttachPlugin();

    return true;
}

void Session::sendAttachPlugin()
{
    JsonPtr jsonMessagePtr(json_object());
    json_t* jsonMessage = jsonMessagePtr.get();

    json_object_set_new(
        jsonMessage,
        "transaction", json_string(std::to_string(_nextTransaction++).c_str()));
    json_object_set_new(jsonMessage, "session_id", json_integer(_session));
    json_object_set_new(jsonMessage, "janus", json_string("attach"));
    json_object_set_new(jsonMessage, "plugin", json_string(Plugin));

    sendMessage(MessageType::AttachPlugin, jsonMessagePtr);
}

bool Session::handleAttachPluginReply(const JsonPtr& jsonMessagePtr)
{
    if(_session == 0 || _handleId != 0)
        return false;

    if(ExtractJanus(jsonMessagePtr) != "success")
        return false;

    json_t* jsonMessage = jsonMessagePtr.get();

    json_t* dataJson = json_object_get(jsonMessage, "data");
    if(!dataJson)
        return false;

    _handleId = ExtractInt(dataJson, "id");
    if(!_handleId)
        return false;

    sendFeatures();
    sendWatch();

    return true;
}

void Session::sendFeatures()
{
    JsonPtr jsonMessagePtr(json_object());
    json_t* jsonMessage = jsonMessagePtr.get();

    json_object_set_new(
        jsonMessage,
        "transaction", json_string(std::to_string(_nextTransaction++).c_str()));
    json_object_set_new(jsonMessage, "session_id", json_integer(_session));
    json_object_set_new(jsonMessage, "handle_id", json_integer(_handleId));
    json_object_set_new(jsonMessage, "janus", json_string("message"));

    json_t* jsonBody = json_object();
    json_object_set_new(jsonMessage, "body", jsonBody);

    json_object_set_new(jsonBody, "request", json_string("features"));

   sendMessage(MessageType::Features, jsonMessagePtr);
}

bool Session::handleFeaturesReply(const JsonPtr& jsonMessagePtr)
{
    return true;
}

void Session::sendWatch()
{
    JsonPtr jsonMessagePtr(json_object());
    json_t* jsonMessage = jsonMessagePtr.get();

    json_object_set_new(
        jsonMessage,
        "transaction", json_string(std::to_string(_nextTransaction++).c_str()));
    json_object_set_new(jsonMessage, "session_id", json_integer(_session));
    json_object_set_new(jsonMessage, "handle_id", json_integer(_handleId));
    json_object_set_new(jsonMessage, "janus", json_string("message"));

    json_t* jsonBody = json_object();
    json_object_set_new(jsonMessage, "body", jsonBody);

    json_object_set_new(jsonBody, "request", json_string("watch"));

    json_t* jsonParams = json_object();
    json_object_set_new(jsonBody, "params", jsonParams);

    json_object_set_new(jsonParams, "audio", json_boolean(true));
    //json_object_set_new(jsonParams, "audio", json_boolean(false));

    sendMessage(MessageType::Watch, jsonMessagePtr);
}

bool Session::handleWatchReply(const JsonPtr& jsonMessagePtr)
{
    return true;
}

void Session::sendStart(const std::string& sdp)
{
    JsonPtr jsonMessagePtr(json_object());
    json_t* jsonMessage = jsonMessagePtr.get();

    json_object_set_new(
        jsonMessage,
        "transaction", json_string(std::to_string(_nextTransaction++).c_str()));
    json_object_set_new(jsonMessage, "session_id", json_integer(_session));
    json_object_set_new(jsonMessage, "handle_id", json_integer(_handleId));
    json_object_set_new(jsonMessage, "janus", json_string("message"));

    json_t* jsonBody = json_object();
    json_object_set_new(jsonMessage, "body", jsonBody);

    json_object_set_new(jsonBody, "request", json_string("start"));

    json_t* jsep = json_object();
    json_object_set_new(jsonMessage, "jsep", jsep);

    json_object_set_new(jsep, "type", json_string("answer"));
    json_object_set_new(jsep, "sdp", json_string(sdp.c_str()));

    sendMessage(MessageType::Start, jsonMessagePtr);
}

bool Session::handleStartReply(const JsonPtr& jsonMessagePtr)
{
    return true;
}

void Session::sendTrickle(unsigned mlineIndex, const std::string& candidate)
{
    JsonPtr jsonMessagePtr(json_object());
    json_t* jsonMessage = jsonMessagePtr.get();

    json_object_set_new(
        jsonMessage,
        "transaction", json_string(std::to_string(_nextTransaction++).c_str()));
    json_object_set_new(jsonMessage, "session_id", json_integer(_session));
    json_object_set_new(jsonMessage, "handle_id", json_integer(_handleId));
    json_object_set_new(jsonMessage, "janus", json_string("trickle"));

    json_t* candidateJson = json_object();
    json_object_set_new(jsonMessage, "candidate", candidateJson);

    if(candidate == "a=end-of-candidates") {
        json_object_set_new(candidateJson, "completed", json_boolean(true));
    } else {
        json_object_set_new(candidateJson, "sdpMLineIndex", json_integer(mlineIndex));
        json_object_set_new(candidateJson, "candidate", json_string(candidate.c_str()));
    }

    sendMessage(MessageType::Trickle, jsonMessagePtr);
}

bool Session::handleTrickleReply(const JsonPtr& /*jsonMessagePtr*/)
{
    return true;
}

bool Session::handleEvent(const JsonPtr& jsonMessagePtr)
{
    json_t* jsonMessage = jsonMessagePtr.get();

    const std::string janus = ExtractJanus(jsonMessagePtr);

    if(janus == "trickle") {
        json_t* candidateJson = json_object_get(jsonMessage, "candidate");

        json_int_t mLineIndex = ExtractInt(candidateJson, "sdpMLineIndex");
        std::string candidate = ExtractString(candidateJson, "candidate");

        if(!_viewPeerPtr)
            return false;

        _viewPeerPtr->addIceCandidate(mLineIndex, candidate);
    } else if(janus == "event") {
        json_t* pluginDataJson = json_object_get(jsonMessage, "plugindata");
        if(!pluginDataJson)
            return false;

        assert(ExtractString(pluginDataJson, "plugin") == "janus.plugin.ustreamer");

        json_t* dataJson = json_object_get(pluginDataJson, "data");
        if(!dataJson)
            return false;

        assert(ExtractString(dataJson, "ustreamer") == "event");

        json_t* resultJson = json_object_get(dataJson, "result");
        if(!resultJson)
            return false;

        const std::string status = ExtractString(resultJson, "status");
        if(status == "features") {
        } else if(status == "started") {
            json_t* jsepJson = json_object_get(jsonMessage, "jsep");
            if(jsepJson) {
                _viewPeerPtr->prepare(
                    std::make_shared<WebRTCConfig>(),
                    std::bind(
                        &Session::receiverPrepared,
                        this),
                    std::bind(
                        &Session::iceCandidate,
                        this,
                        std::placeholders::_1,
                        std::placeholders::_2),
                    std::bind(
                        &Session::eos,
                        this));

                const std::string sdp = ExtractString(jsepJson, "sdp");

                _viewPeerPtr->setRemoteSdp(sdp);
            } else {
                _viewPeerPtr->play();
            }
        }
    }

    return true;
}

void Session::receiverPrepared()
{
    sendStart(_viewPeerPtr->sdp());
}

void Session::iceCandidate(unsigned mlineIndex, const std::string& candidate)
{
    sendTrickle(mlineIndex, candidate);
}

void Session::eos()
{
    disconnect();
}
