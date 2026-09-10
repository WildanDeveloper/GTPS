#include "logger.hpp"
#include "server.hpp"

#include <csignal>
#include <cstdlib>

namespace
{
volatile std::sig_atomic_t g_stopSignal = 0;
}

static void onSignal(int value)
{
    g_stopSignal = value;
}

int main()
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    WildanDev::GameServer server;
    if (!server.configure("resources"))
        return EXIT_FAILURE;
    if (!server.start())
        return EXIT_FAILURE;

    WildanDev::logInfo("WildanDev GTPS is running. Press Ctrl+C to stop.");
    while (!g_stopSignal)
        server.runOnce();

    WildanDev::logInfo("Shutdown requested, stopping cleanly.");
    server.shutdown();
    return EXIT_SUCCESS;
}
