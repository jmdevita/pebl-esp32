#ifndef RESILIENCE_MANAGER_H
#define RESILIENCE_MANAGER_H

#include <Arduino.h>
#include <vector>
#include <ArduinoJson.h>

// Message queue entry
struct QueuedMessage {
    String json;
    uint32_t timestamp;
    uint8_t retryCount;
};

// Connection health metrics
struct ConnectionHealth {
    bool isHealthy;
    uint32_t lastHeartbeat;
    uint32_t missedHeartbeats;
    uint32_t reconnectAttempts;
    uint32_t totalDowntime;
    uint32_t lastDowntimeStart;
};

class ResilienceManager {
private:
    static std::vector<QueuedMessage> messageQueue;
    static ConnectionHealth health;
    static uint32_t heartbeatInterval;
    static uint32_t heartbeatTimeout;
    static const size_t MAX_QUEUE_SIZE = 10;
    static const uint8_t MAX_RETRY_COUNT = 3;

    static void logMessage(const char* level, const char* module, const char* message, const char* details = nullptr);

public:
    // Initialize with timing parameters
    static void init(uint32_t hbInterval, uint32_t hbTimeout);

    // Heartbeat management
    static void recordHeartbeat();
    static bool checkHeartbeatTimeout();
    static uint32_t getTimeSinceLastHeartbeat();

    // Message queue management
    static bool queueMessage(const String& json);
    static bool hasQueuedMessages() { return !messageQueue.empty(); }
    static size_t getQueueSize() { return messageQueue.size(); }
    static String getNextQueuedMessage();
    static void clearMessageQueue();
    static bool processQueuedMessages();

    // Connection health tracking
    static void markConnectionLost();
    static void markConnectionRestored();
    static bool isConnectionHealthy() { return health.isHealthy; }
    static uint32_t getReconnectAttempts() { return health.reconnectAttempts; }
    static uint32_t getTotalDowntime();

    // Metrics and status
    static String getHealthStatus();
    static void resetMetrics();

    // Periodic health check (call from loop)
    static void checkHealth();
};

#endif // RESILIENCE_MANAGER_H