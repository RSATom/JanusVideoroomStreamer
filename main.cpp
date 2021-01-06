#include <deque>

#include <glib.h>

#include <libwebsockets.h>

#include <CxxPtr/CPtr.h>
#include <CxxPtr/GlibPtr.h>
#include "CxxPtr/libconfigDestroy.h"

#include "Common/ConfigHelpers.h"
#include "Common/LwsLog.h"

#include "Client/Log.h"

#include "GstStreaming/LibGst.h"

#include "GstStreaming/GstTestStreamer.h"
#include "GstStreaming/GstReStreamer.h"

#include "Log.h"
#include "Config.h"
#include "WsClient.h"


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
            const char* type = nullptr;
            config_setting_lookup_string(streamerConfig, "type", &type);

            if(nullptr == type || 0 == strcmp(type, "restreamer"))
                loadedConfig.streamer.type = StreamerConfig::Type::ReStreamer;
            else if(0 == strcmp(type, "test"))
                loadedConfig.streamer.type = StreamerConfig::Type::Test;

            const char* uri = nullptr;
            if(CONFIG_TRUE == config_setting_lookup_string(streamerConfig, "url", &uri) ||
               CONFIG_TRUE == config_setting_lookup_string(streamerConfig, "uri", &uri))
            {
                loadedConfig.streamer.uri = uri;
            }

            const char* videocodec = nullptr;
            if(config_setting_lookup_string(streamerConfig, "videocodec", &videocodec)) {
                if(0 == strcmp(videocodec, "h264"))
                    loadedConfig.streamer.videocodec = GstStreaming::Videocodec::h264;
                else if(0 == strcmp(videocodec, "vp8"))
                    loadedConfig.streamer.videocodec = GstStreaming::Videocodec::vp8;
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
        return std::make_unique<GstTestStreamer>(config->streamer.uri, config->streamer.videocodec);
    case StreamerConfig::Type::ReStreamer:
        return std::make_unique<GstReStreamer>(config->streamer.uri);
    default:
        return std::make_unique<GstTestStreamer>();
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

static void ClientDisconnected() noexcept
{
}

int main(int argc, char *argv[])
{
    LibGst libGst;

    Config config {};
    if(!LoadConfig(&config))
        return -1;

    InitLwsLogger(config.lwsLogLevel);
    InitWsClientLogger(config.logLevel);
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
        ClientDisconnected);

    if(client.init()) {
        client.connect();
        g_main_loop_run(loop);
    } else
        return -1;

    return 0;
}
