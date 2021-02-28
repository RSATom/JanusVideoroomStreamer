#include "Session.h"

#include "CxxPtr/CPtr.h"
#include "CxxPtr/JanssonPtr.h"


namespace {

enum {
    KEEPALIVE_TIMEOUT = 30,
    TIMEOUT_CHECK_INTERVAL = 15,
    UPDATE_PARTICIPANTS_INTERVAL = 60,
};

char const * const Plugin = "janus.plugin.videoroom";

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
    const std::function<std::unique_ptr<WebRTCPeer> ()>& createPeer,
    const std::function<void (const char*)>& sendMessage) noexcept:
    _config(config), _createPeer(createPeer), _sendMessage(sendMessage),
    _lastMessageTimer(g_timer_new())
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

    if(_config->trackParticipants) {
        const GSourceFunc updateParticipantsTimeoutCallback =
            [] (gpointer userData) -> gboolean {
                static_cast<Session*>(userData)->updateParticipants();
                return TRUE;
            };

        _updateParticipantsTimeout =
            g_timeout_add_seconds(
                UPDATE_PARTICIPANTS_INTERVAL,
                updateParticipantsTimeoutCallback, this);
    }
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
            case MessageType::Join:
                return handleJoinReply(jsonMessagePtr);
            case MessageType::Publish:
                return handlePublishReply(jsonMessagePtr);
            case MessageType::UnPublish:
                return handleUnPublishReply(jsonMessagePtr);
            case MessageType::Trickle:
                return handleTrickleReply(jsonMessagePtr);
            case MessageType::ListParticipants:
                return handleListParticipantsReply(jsonMessagePtr);
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

    sendJoin();

    return true;
}

void Session::sendJoin()
{
    JsonPtr jsonMessagePtr(json_object());
    json_t* jsonMessage = jsonMessagePtr.get();

    json_object_set_new(
        jsonMessage,
        "transaction", json_string(std::to_string(_nextTransaction++).c_str()));
    json_object_set_new(jsonMessage, "session_id", json_integer(_session));
    json_object_set_new(jsonMessage, "handle_id", json_integer(_handleId));
    json_object_set_new(jsonMessage, "janus", json_string("message"));
    json_object_set_new(jsonMessage, "plugin", json_string(Plugin));

    json_t* jsonBody = json_object();
    json_object_set_new(jsonMessage, "body", jsonBody);

    json_object_set_new(jsonBody, "request", json_string("join"));
    json_object_set_new(jsonBody, "ptype", json_string("publisher"));
    json_object_set_new(jsonBody, "room", json_integer(_config->room));
    json_object_set_new(jsonBody, "display", json_string(_config->display.c_str()));

    sendMessage(MessageType::Join, jsonMessagePtr);
}

bool Session::handleJoinReply(const JsonPtr& jsonMessagePtr)
{
    if(_session == 0 || _handleId == 0)
        return false;

    if(ExtractJanus(jsonMessagePtr) != "event")
        return false;

    json_t* jsonMessage = jsonMessagePtr.get();

    json_t* plugindataJson = json_object_get(jsonMessage, "plugindata");
    if(!plugindataJson)
        return false;

    json_t* dataJson = json_object_get(plugindataJson, "data");
    if(!dataJson)
        return false;

    if(ExtractString(dataJson, "videoroom") != "joined")
        return false;

    if(_config->trackParticipants)
        updateParticipants();
    else
        startStream();

    return true;
}

void Session::sendPublish(const std::string& sdp)
{
    JsonPtr jsonMessagePtr(json_object());
    json_t* jsonMessage = jsonMessagePtr.get();

    json_object_set_new(
        jsonMessage,
        "transaction", json_string(std::to_string(_nextTransaction++).c_str()));
    json_object_set_new(jsonMessage, "session_id", json_integer(_session));
    json_object_set_new(jsonMessage, "handle_id", json_integer(_handleId));
    json_object_set_new(jsonMessage, "janus", json_string("message"));
    json_object_set_new(jsonMessage, "plugin", json_string(Plugin));

    json_t* jsonBody = json_object();
    json_object_set_new(jsonMessage, "body", jsonBody);

    json_object_set_new(jsonBody, "request", json_string("configure"));

    json_object_set_new(jsonBody, "audio", json_boolean(false));
    json_object_set_new(jsonBody, "video", json_boolean(true));
    json_object_set_new(jsonBody, "data", json_boolean(false));

    json_t* jsep = json_object();
    json_object_set_new(jsonMessage, "jsep", jsep);

    json_object_set_new(jsep, "type", json_string("offer"));
    json_object_set_new(jsep, "sdp", json_string(sdp.c_str()));

    sendMessage(MessageType::Publish, jsonMessagePtr);
}

bool Session::handlePublishReply(const JsonPtr& jsonMessagePtr)
{
    if(_session == 0 || _handleId == 0)
        return false;

    if(ExtractJanus(jsonMessagePtr) != "event")
        return false;

    json_t* jsonMessage = jsonMessagePtr.get();

    json_t* plugindataJson = json_object_get(jsonMessage, "plugindata");
    if(!plugindataJson)
        return false;

    json_t* dataJson = json_object_get(plugindataJson, "data");
    if(!dataJson)
        return false;

    if(ExtractString(dataJson, "videoroom") != "event")
        return false;

    if(ExtractString(dataJson, "configured") != "ok")
        return false;

    json_t* jsepJson = json_object_get(jsonMessage, "jsep");
    if(!jsepJson)
        return false;

    const std::string type = ExtractString(jsepJson, "type");
    if(type != "answer")
        return false;

    const std::string sdp = ExtractString(jsepJson, "sdp");

    if(!_streamerPtr)
        return false;

    _streamerPtr->setRemoteSdp(sdp);

    _streamerPtr->play();

    return true;
}

/*
void Session::sendJoinAndConfigure(const std::string& sdp)
{
    JsonPtr jsonMessagePtr(json_object());
    json_t* jsonMessage = jsonMessagePtr.get();

    json_object_set_new(
        jsonMessage,
        "transaction", json_string(std::to_string(_nextTransaction++).c_str()));
    json_object_set_new(jsonMessage, "session_id", json_integer(_session));
    json_object_set_new(jsonMessage, "handle_id", json_integer(_handleId));
    json_object_set_new(jsonMessage, "janus", json_string("message"));
    json_object_set_new(jsonMessage, "plugin", json_string(Plugin));

    json_t* jsonBody = json_object();
    json_object_set_new(jsonMessage, "body", jsonBody);

    json_object_set_new(jsonBody, "request", json_string("joinandconfigure"));
    json_object_set_new(jsonBody, "ptype", json_string("publisher"));
    json_object_set_new(jsonBody, "room", json_integer(_config->room));
    json_object_set_new(jsonBody, "display", json_string(_config->display.c_str()));

    json_object_set_new(jsonBody, "audio", json_boolean(false));
    json_object_set_new(jsonBody, "video", json_boolean(true));
    json_object_set_new(jsonBody, "data", json_boolean(false));

    json_t* jsep = json_object();
    json_object_set_new(jsonMessage, "jsep", jsep);

    json_object_set_new(jsep, "type", json_string("offer"));
    json_object_set_new(jsep, "sdp", json_string(sdp.c_str()));

    sendMessage(MessageType::JoinAndConfigure, jsonMessagePtr);
}

bool Session::handleJoinAndConfigureReply(const JsonPtr& jsonMessagePtr)
{
    if(_session == 0 || _handleId == 0)
        return false;

    if(ExtractJanus(jsonMessagePtr) != "event")
        return false;

    json_t* jsonMessage = jsonMessagePtr.get();

    json_t* plugindataJson = json_object_get(jsonMessage, "plugindata");
    if(!plugindataJson)
        return false;

    json_t* dataJson = json_object_get(plugindataJson, "data");
    if(!dataJson)
        return false;

    if(ExtractString(dataJson, "videoroom") != "joined")
        return false;

    json_t* jsepJson = json_object_get(jsonMessage, "jsep");
    if(!jsepJson)
        return false;

    const std::string type = ExtractString(jsepJson, "type");
    if(type != "answer")
        return false;

    const std::string sdp = ExtractString(jsepJson, "sdp");

    if(!_streamerPtr)
        return false;

    _streamerPtr->setRemoteSdp(sdp);

    _streamerPtr->play();

    return true;
}
*/

void Session::sendUnPublish()
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

    json_object_set_new(jsonBody, "request", json_string("unpublish"));

    sendMessage(MessageType::UnPublish, jsonMessagePtr);
}

bool Session::handleUnPublishReply(const JsonPtr& jsonMessagePtr)
{
    if(_session == 0 || _handleId == 0)
        return false;

    if(ExtractJanus(jsonMessagePtr) != "event")
        return false;

    json_t* jsonMessage = jsonMessagePtr.get();

    json_t* plugindataJson = json_object_get(jsonMessage, "plugindata");
    if(!plugindataJson)
        return false;

    json_t* dataJson = json_object_get(plugindataJson, "data");
    if(!dataJson)
        return false;

    if(ExtractString(dataJson, "videoroom") != "event")
        return false;

    if(ExtractString(dataJson, "unpublished") != "ok")
        return false;

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


void Session::sendListParticipants()
{
    JsonPtr jsonMessagePtr(json_object());
    json_t* jsonMessage = jsonMessagePtr.get();

    json_object_set_new(
        jsonMessage,
        "transaction", json_string(std::to_string(_nextTransaction++).c_str()));
    json_object_set_new(jsonMessage, "session_id", json_integer(_session));
    json_object_set_new(jsonMessage, "handle_id", json_integer(_handleId));
    json_object_set_new(jsonMessage, "janus", json_string("message"));
    json_object_set_new(jsonMessage, "plugin", json_string(Plugin));

    json_t* jsonBody = json_object();
    json_object_set_new(jsonMessage, "body", jsonBody);

    json_object_set_new(jsonBody, "request", json_string("listparticipants"));
    json_object_set_new(jsonBody, "room", json_integer(_config->room));

    sendMessage(MessageType::ListParticipants, jsonMessagePtr);
}

bool Session::handleListParticipantsReply(const JsonPtr& jsonMessagePtr)
{
    if(_session == 0 || _handleId == 0)
        return false;

    if(ExtractJanus(jsonMessagePtr) != "success")
        return false;

    json_t* jsonMessage = jsonMessagePtr.get();

    json_t* plugindataJson = json_object_get(jsonMessage, "plugindata");
    if(!plugindataJson)
        return false;

    json_t* dataJson = json_object_get(plugindataJson, "data");
    if(!dataJson)
        return false;

    if(ExtractString(dataJson, "videoroom") != "participants")
        return false;

    json_t* participantsJson = json_object_get(dataJson, "participants");
    if(!participantsJson)
        return false;

    if(!json_is_array(participantsJson))
        return false;

    if(json_array_size(participantsJson) > 1)
        startStream();
    else
        stopStream();

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

        if(!_streamerPtr)
            return false;

        _streamerPtr->addIceCandidate(mLineIndex, candidate);
    }

    return true;
}

void Session::streamerPrepared()
{
    std::string sdp;
    if(_streamerPtr->sdp(&sdp))
        sendPublish(sdp);
    else
        disconnect();
}

void Session::iceCandidate(unsigned mlineIndex, const std::string& candidate)
{
    sendTrickle(mlineIndex, candidate);
}

void Session::eos()
{
}


void Session::updateParticipants()
{
    if(_session == 0 || _handleId == 0) {
        disconnect();
        return;
    }

    sendListParticipants();
}

void Session::startStream()
{
    if(_streamerPtr)
        return;

    _streamerPtr = _createPeer();

    _streamerPtr->prepare(
        _config->iceServers,
        std::bind(
            &Session::streamerPrepared,
            this),
        std::bind(
            &Session::iceCandidate,
            this,
            std::placeholders::_1,
            std::placeholders::_2),
        std::bind(
            &Session::eos,
            this));
}

void Session::stopStream()
{
    if(!_streamerPtr)
        return;

    _streamerPtr->stop();
    _streamerPtr.reset();

    sendUnPublish();
}
