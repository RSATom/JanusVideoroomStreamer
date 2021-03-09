#include <deque>

#include <CxxPtr/CPtr.h>
#include <CxxPtr/GlibPtr.h>
#include "CxxPtr/libconfigDestroy.h"

#include "Common/ConfigHelpers.h"
#include "Common/LwsLog.h"

#include "GstStreaming/LibGst.h"

#include "GstStreaming/GstTestStreamer.h"
#include "GstStreaming/GstPipelineStreamer.h"
#include "GstStreaming/GstReStreamer.h"

#include "Log.h"
#include "Config.h"
#include "WsClient.h"


enum {
    DEFAULT_RECONNECT_TIMEOUT = 5,
};

static const auto Log = ClientLog;

static bool LoadConfig(Config* config)
{
    const std::deque<std::string> configDirs = ::ConfigDirs();
    if(configDirs.empty())
        return false;

    Config loadedConfig = *config;

    bool someConfigFound = false;
    for(const std::string& configDir: configDirs) {
        const std::string configFile = configDir + "/janus-videoroom-streamer.conf";
        if(!g_file_test(configFile.c_str(),  G_FILE_TEST_IS_REGULAR)) {
            Log()->info("Config \"{}\" not found", configFile);
            continue;
        }

        someConfigFound = true;

        config_t config;
        config_init(&config);
        ConfigDestroy ConfigDestroy(&config);

        Log()->info("Loading config \"{}\"", configFile);
        if(!config_read_file(&config, configFile.c_str())) {
            Log()->error("Fail load config. {}. {}:{}",
                config_error_text(&config),
                configFile,
                config_error_line(&config));
            return false;
        }

        config_setting_t* targetConfig = config_lookup(&config, "janus");
        if(targetConfig && CONFIG_TRUE == config_setting_is_group(targetConfig)) {
            const char* url = nullptr;
            if(CONFIG_TRUE == config_setting_lookup_string(targetConfig, "url", &url)) {
                loadedConfig.janusUrl = url;
            }
            const char* cipherList = nullptr;
            if(CONFIG_TRUE == config_setting_lookup_string(targetConfig, "cipher-list", &cipherList))
            {
                loadedConfig.cipherList = cipherList;
            }
            int timeout = 0;
            if(CONFIG_TRUE == config_setting_lookup_int(targetConfig, "reconnect-timeout", &timeout)) {
                loadedConfig.reconnectTimeout = static_cast<unsigned>(timeout);
            }
            const char* display = nullptr;
            if(CONFIG_TRUE == config_setting_lookup_string(targetConfig, "display", &display)) {
                loadedConfig.display = display;
            }
            int room = 0;
            if(CONFIG_TRUE == config_setting_lookup_int(targetConfig, "room", &room)) {
                loadedConfig.room = room;
            }
        }
        config_setting_t* streamerConfig = config_lookup(&config, "streamer");
        if(streamerConfig && CONFIG_TRUE == config_setting_is_group(streamerConfig)) {
            const char* test = nullptr;
            if(CONFIG_TRUE == config_setting_lookup_string(streamerConfig, "test", &test)) {
                loadedConfig.streamer.type = StreamerConfig::Type::Test;
                loadedConfig.streamer.source = test;
            }

            const char* videocodec = nullptr;
            if(config_setting_lookup_string(streamerConfig, "videocodec", &videocodec)) {
                if(0 == strcmp(videocodec, "h264"))
                    loadedConfig.streamer.videocodec = GstStreaming::Videocodec::h264;
                else if(0 == strcmp(videocodec, "vp8"))
                    loadedConfig.streamer.videocodec = GstStreaming::Videocodec::vp8;
            }

            const char* pipeline = nullptr;
            if(CONFIG_TRUE == config_setting_lookup_string(streamerConfig, "pipeline", &pipeline)) {
                loadedConfig.streamer.type = StreamerConfig::Type::Pipeline;
                loadedConfig.streamer.source = pipeline;
            }

            const char* url = nullptr;
            if(CONFIG_TRUE == config_setting_lookup_string(streamerConfig, "restream", &url)) {
                loadedConfig.streamer.type = StreamerConfig::Type::ReStreamer;
                loadedConfig.streamer.source = url;
            }
        }
        config_setting_t* debugConfig = config_lookup(&config, "debug");
        if(debugConfig && CONFIG_TRUE == config_setting_is_group(debugConfig)) {
            int logLevel = 0;
            if(CONFIG_TRUE == config_setting_lookup_int(debugConfig, "log-level", &logLevel)) {
                if(logLevel > 0) {
                    loadedConfig.logLevel =
                        static_cast<spdlog::level::level_enum>(
                            spdlog::level::critical - std::min<int>(logLevel, spdlog::level::critical));
                }
            }
            int lwsLogLevel = 0;
            if(CONFIG_TRUE == config_setting_lookup_int(debugConfig, "lws-log-level", &lwsLogLevel)) {
                if(lwsLogLevel > 0) {
                    loadedConfig.lwsLogLevel =
                        static_cast<spdlog::level::level_enum>(
                            spdlog::level::critical - std::min<int>(lwsLogLevel, spdlog::level::critical));
                }
            }
        }
    }

    if(!someConfigFound)
        return false;

    bool success = true;

    if(loadedConfig.janusUrl.empty()) {
        Log()->error("Missing Janus URL");
        success = false;
    }

    if(success)
        *config = loadedConfig;

    return success;
}

static std::unique_ptr<WebRTCPeer>
CreatePeer(const Config* config)
{
    switch(config->streamer.type) {
    case StreamerConfig::Type::Test:
        return
            std::make_unique<GstTestStreamer>(
                config->streamer.source,
                config->streamer.videocodec);
    case StreamerConfig::Type::Pipeline:
        return
            std::make_unique<GstPipelineStreamer>(config->streamer.source);
    case StreamerConfig::Type::ReStreamer:
        return
            std::make_unique<GstReStreamer>(config->streamer.source);
    default:
        return
            std::make_unique<GstTestStreamer>();
    }
}

static std::unique_ptr<Session> CreateSession(
    Config* config,
    const std::function<void (const char*) noexcept>& sendMessage) noexcept
{
    return
        std::make_unique<Session>(
            config,
            std::bind(CreatePeer, config),
            sendMessage);
}

static void ClientDisconnected(
    const Config* config,
    WsClient* client) noexcept
{
    const unsigned reconnectTimeout =
        config->reconnectTimeout > 0 ?
            config->reconnectTimeout :
            DEFAULT_RECONNECT_TIMEOUT;

    Log()->info("Scheduling reconnect in {} seconds...", reconnectTimeout);

    GSourcePtr timeoutSourcePtr(g_timeout_source_new_seconds(reconnectTimeout));
    GSource* timeoutSource = timeoutSourcePtr.get();
    g_source_set_callback(timeoutSource,
        [] (gpointer userData) -> gboolean {
            static_cast<WsClient*>(userData)->connect();
            return false;
        }, client, nullptr);
    g_source_attach(timeoutSource, g_main_context_get_thread_default());
}

int main(int /*argc*/, char** /*argv*/)
{
    LibGst libGst;

    Config config {};
    if(!LoadConfig(&config))
        return -1;

    InitLwsLogger(config.lwsLogLevel);
    InitJanusClientLogger(config.logLevel);

    GMainLoopPtr loopPtr(g_main_loop_new(nullptr, FALSE));
    GMainLoop* loop = loopPtr.get();

    WsClient client(
        config,
        loop,
        std::bind(
            CreateSession,
            &config,
            std::placeholders::_1),
        std::bind(ClientDisconnected, &config, &client));

    if(client.init()) {
        client.connect();
        g_main_loop_run(loop);
    } else
        return -1;

    return 0;
}
