#include "WsClient.h"

#include <deque>
#include <algorithm>
#include <map>

#include "CxxPtr/libwebsocketsPtr.h"
#include "CxxPtr/JanssonPtr.h"
#include "CxxPtr/GlibPtr.h"

#include "Common/MessageBuffer.h"
#include "Common/LwsSource.h"

#include "Log.h"


namespace {

enum {
    RX_BUFFER_SIZE = 512,
    PING_INTERVAL = 20,
};

enum {
    PROTOCOL_ID,
};

#if LWS_LIBRARY_VERSION_MAJOR < 3
enum {
    LWS_CALLBACK_CLIENT_CLOSED = LWS_CALLBACK_CLOSED
};
#endif

struct SessionData
{
    bool terminateSession = false;
    MessageBuffer incomingMessage;
    std::deque<MessageBuffer> sendMessages;
    std::unique_ptr<Session > session;
};

// Should contain only POD types,
// since created inside libwebsockets on session create.
struct SessionContextData
{
    lws* wsi;
    SessionData* data;
};

const auto Log = ClientLog;

}

struct WsClient::Private
{
    Private(
        WsClient*,
        const Config&,
        GMainLoop*,
        const CreateSession&,
        const Disconnected&);

    bool init();
    int httpCallback(lws*, lws_callback_reasons, void* user, void* in, size_t len);
    int wsCallback(lws*, lws_callback_reasons, void* user, void* in, size_t len);
    bool onMessage(SessionContextData*, const MessageBuffer&);

    void send(SessionContextData*, MessageBuffer*);
    void sendMessage(SessionContextData*, const char* message);

    void connect();
    bool onConnected(SessionContextData*);


    WsClient *const owner;
    Config config;
    GMainLoop* loop = nullptr;
    CreateSession createSession;
    Disconnected disconnected;

#if !defined(LWS_WITH_GLIB)
    LwsSourcePtr lwsSourcePtr;
#endif
    LwsContextPtr contextPtr;

    lws* connection = nullptr;
    bool connected = false;
};

WsClient::Private::Private(
    WsClient* owner,
    const Config& config,
    GMainLoop* loop,
    const WsClient::CreateSession& createSession,
    const Disconnected& disconnected) :
    owner(owner), config(config), loop(loop),
    createSession(createSession), disconnected(disconnected)
{
}

int WsClient::Private::wsCallback(
    lws* wsi,
    lws_callback_reasons reason,
    void* user,
    void* in, size_t len)
{
    SessionContextData* scd = static_cast<SessionContextData*>(user);
    switch(reason) {
#if !defined(LWS_WITH_GLIB)
        case LWS_CALLBACK_ADD_POLL_FD:
        case LWS_CALLBACK_DEL_POLL_FD:
        case LWS_CALLBACK_CHANGE_MODE_POLL_FD:
            return LwsSourceCallback(lwsSourcePtr, wsi, reason, in, len);
#endif
        case LWS_CALLBACK_CLIENT_ESTABLISHED: {
            Log()->info("Connection to server established.");

            std::unique_ptr<Session> session =
                createSession(
                    std::bind(
                        &Private::sendMessage,
                        this,
                        scd,
                        std::placeholders::_1));
            if(!session)
                return -1;

            scd->data =
                new SessionData {
                    .terminateSession = false,
                    .incomingMessage ={},
                    .sendMessages = {},
                    .session = std::move(session)};
            scd->wsi = wsi;

            connected = true;

            if(!onConnected(scd))
                return -1;

            break;
        }
        case LWS_CALLBACK_CLIENT_RECEIVE_PONG:
            Log()->trace("PONG");
            break;
        case LWS_CALLBACK_CLIENT_RECEIVE:
            if(scd->data->incomingMessage.onReceive(wsi, in, len)) {
                if(Log()->level() <= spdlog::level::trace) {
                    std::string logMessage;
                    logMessage.reserve(scd->data->incomingMessage.size());
                    std::remove_copy(
                        scd->data->incomingMessage.data(),
                        scd->data->incomingMessage.data() + scd->data->incomingMessage.size(),
                        std::back_inserter(logMessage), '\r');

                    Log()->trace("-> WsClient: {}", logMessage);
                }

                if(!onMessage(scd, scd->data->incomingMessage))
                    return -1;

                scd->data->incomingMessage.clear();
            }

            break;
        case LWS_CALLBACK_CLIENT_WRITEABLE:
            if(scd->data->terminateSession)
                return -1;

            if(!scd->data->sendMessages.empty()) {
                MessageBuffer& buffer = scd->data->sendMessages.front();
                if(!buffer.writeAsText(wsi)) {
                    Log()->error("Write failed.");
                    return -1;
                }

                scd->data->sendMessages.pop_front();

                if(!scd->data->sendMessages.empty())
                    lws_callback_on_writable(wsi);
            }

            break;
        case LWS_CALLBACK_CLIENT_CLOSED:
            Log()->info("Connection to server is closed.");

            delete scd->data;
            scd = nullptr;

            connection = nullptr;
            connected = false;

            if(disconnected)
                disconnected();

            break;
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            Log()->error("Can not connect to server.");

            delete scd->data;
            scd = nullptr;

            connection = nullptr;
            connected = false;

            if(disconnected)
                disconnected();

            break;
        default:
            break;
    }

    return 0;
}

bool WsClient::Private::init()
{
    auto WsCallback =
        [] (lws* wsi, lws_callback_reasons reason, void* user, void* in, size_t len) -> int {
            lws_context* context = lws_get_context(wsi);
            Private* p = static_cast<Private*>(lws_context_user(context));

            return p->wsCallback(wsi, reason, user, in, len);
        };

    static const lws_protocols protocols[] = {
        {
            "janus-protocol",
            WsCallback,
            sizeof(SessionContextData),
            RX_BUFFER_SIZE,
            PROTOCOL_ID,
            nullptr
        },
        { nullptr, nullptr, 0, 0, 0, nullptr } /* terminator */
    };

    lws_context_creation_info wsInfo {};
    wsInfo.gid = -1;
    wsInfo.uid = -1;
    wsInfo.port = CONTEXT_PORT_NO_LISTEN;
    wsInfo.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    wsInfo.ssl_cipher_list = "DEFAULT@SECLEVEL=1";
#if defined(LWS_WITH_GLIB)
    wsInfo.options |= LWS_SERVER_OPTION_GLIB;
    wsInfo.foreign_loops = reinterpret_cast<void**>(&loop);
#endif
    wsInfo.protocols = protocols;
#if LWS_LIBRARY_VERSION_NUMBER < 4000000
    wsInfo.ws_ping_pong_interval = PING_INTERVAL;
#else
    lws_retry_bo_t retryPolicy {};
    retryPolicy.secs_since_valid_ping = PING_INTERVAL;
    wsInfo.retry_and_idle_policy = &retryPolicy;
#endif
    wsInfo.user = this;

    contextPtr.reset(lws_create_context(&wsInfo));
    lws_context* context = contextPtr.get();
    if(!context)
        return false;

#if !defined(LWS_WITH_GLIB)
    lwsSourcePtr = LwsSourceNew(context, g_main_context_get_thread_default());
    if(!lwsSourcePtr)
        return false;
#endif

    return true;
}

void WsClient::Private::connect()
{
    if(connection)
        return;

    if(config.janusUrl.empty()) {
        Log()->error("Missing Janus URL.");
        return;
    }

    bool useSecureConnection = true;

    std::vector<char> urlBuffer;
    urlBuffer.reserve(config.janusUrl.size() + 1);
    urlBuffer.assign(config.janusUrl.begin(), config.janusUrl.end());
    urlBuffer.push_back('\0');

    const char* prot;
    const char* ads;
    int port;
    const char* path;

    if(0 != lws_parse_uri(urlBuffer.data(), &prot, &ads, &port, &path)) {
        Log()->error("Invalid URL.");
        return;
    }

    if(0 == strcmp(prot, "ws"))
        useSecureConnection = false;
    else if(0 == strcmp(prot, "wss"))
        useSecureConnection = true;
    else {
        Log()->error("Only \"ws://\" or \"wss://\" URLs are supported.");
        return;
    }

    if(port <= 0) {
        if(useSecureConnection)
            port = 443;
        else
            port = 80;
    }

    Log()->info("Connecting to {}...", config.janusUrl);

    struct lws_client_connect_info connectInfo = {};
    connectInfo.context = contextPtr.get();
    connectInfo.address = ads;
    connectInfo.host = ads;
    connectInfo.port = port;
    connectInfo.path = path;

    if(useSecureConnection)
        connectInfo.ssl_connection = LCCSCF_USE_SSL;
    connectInfo.protocol = "janus-protocol";

    connection = lws_client_connect_via_info(&connectInfo);
    connected = false;
}

bool WsClient::Private::onConnected(SessionContextData* scd)
{
    return scd->data->session->onConnected();
}

bool WsClient::Private::onMessage(
    SessionContextData* scd,
    const MessageBuffer& message)
{
    json_error_t jsonError;
    JsonPtr jsonMessage(json_loadb(message.data(), message.size(), 0, &jsonError));
    if(!jsonMessage)
        return false;

    if(!scd->data->session->handleMessage(jsonMessage)) {
        Log()->debug("Fail handle message. Forcing session disconnect...");
        return false;
    }

    return true;
}

void WsClient::Private::send(SessionContextData* scd, MessageBuffer* message)
{
    assert(!message->empty());

    scd->data->sendMessages.emplace_back(std::move(*message));

    lws_callback_on_writable(scd->wsi);
}

void WsClient::Private::sendMessage(
    SessionContextData* scd,
    const char* message)
{
    if(!message) {
        scd->data->terminateSession = true;
        lws_callback_on_writable(scd->wsi);
        return;
    }

    if(Log()->level() <= spdlog::level::trace) {
        std::string logMessage;
        for(const char* c = message; *c != '\0'; ++c) {
            if(*c != '\r')
                logMessage.push_back(*c);
        }

        Log()->trace("WsClient -> : {}", logMessage);
    }

    MessageBuffer requestMessage;
    requestMessage.assign(message);
    send(scd, &requestMessage);
}

WsClient::WsClient(
    const Config& config,
    GMainLoop* loop,
    const CreateSession& createSession,
    const Disconnected& disconnected) noexcept:
    _p(std::make_unique<Private>(this, config, loop, createSession, disconnected))
{
}

WsClient::~WsClient()
{
}

bool WsClient::init() noexcept
{
    return _p->init();
}

void WsClient::connect() noexcept
{
    _p->connect();
}
