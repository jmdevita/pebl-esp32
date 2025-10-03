#include "ResilienceManager.h"

// Static member definitions
std::vector<QueuedMessage> ResilienceManager::messageQueue;
ConnectionHealth ResilienceManager::health = {true, 0, 0, 0, 0, 0};
uint32_t ResilienceManager::heartbeatInterval = 15000;  // Default 15 seconds
uint32_t ResilienceManager::heartbeatTimeout = 30000;   // Default 30 seconds

// External log function (defined in main.cpp)
extern void logMessage(int level, const char* module, const char* message, const char* kvPairs);

void ResilienceManager::logMessage(const char* level, const char* module, const char* message, const char* details) {
    int logLevel = 2; // INFO by default
    if (strcmp(level, "ERROR") == 0) logLevel = 0;
    else if (strcmp(level, "WARN") == 0) logLevel = 1;
    else if (strcmp(level, "DEBUG") == 0) logLevel = 3;

    ::logMessage(logLevel, module, message, details);
}

void ResilienceManager::init(uint32_t hbInterval, uint32_t hbTimeout) {
    heartbeatInterval = hbInterval;
    heartbeatTimeout = hbTimeout;
    health.lastHeartbeat = millis();
    health.isHealthy = true;

    char buf[128];
    snprintf(buf, sizeof(buf), "interval=%lu timeout=%lu", hbInterval, hbTimeout);
    logMessage("INFO", "RESILIENCE", "Initialized", buf);
}

void ResilienceManager::recordHeartbeat() {
    uint32_t now = millis();
    uint32_t timeSince = now - health.lastHeartbeat;

    health.lastHeartbeat = now;
    health.missedHeartbeats = 0;

    if (!health.isHealthy) {
        markConnectionRestored();
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "time_since_last=%lu", timeSince);
    logMessage("DEBUG", "RESILIENCE", "Heartbeat recorded", buf);
}

bool ResilienceManager::checkHeartbeatTimeout() {
    // This function is kept for metrics tracking only
    // It no longer triggers disconnections - that's handled by WebSocket events
    uint32_t timeSince = getTimeSinceLastHeartbeat();

    if (timeSince > heartbeatTimeout) {
        health.missedHeartbeats = timeSince / heartbeatInterval;

        // Just log for debugging, don't trigger any connection changes
        char buf[128];
        snprintf(buf, sizeof(buf), "timeout=%lu missed=%lu", timeSince, health.missedHeartbeats);
        logMessage("DEBUG", "RESILIENCE", "Heartbeat metrics", buf);

        return true;
    }

    return false;
}

uint32_t ResilienceManager::getTimeSinceLastHeartbeat() {
    return millis() - health.lastHeartbeat;
}

bool ResilienceManager::queueMessage(const String& json) {
    if (messageQueue.size() >= MAX_QUEUE_SIZE) {
        logMessage("WARN", "RESILIENCE", "Queue full, dropping oldest message");
        messageQueue.erase(messageQueue.begin());
    }

    QueuedMessage msg;
    msg.json = json;
    msg.timestamp = millis();
    msg.retryCount = 0;

    messageQueue.push_back(msg);

    char buf[64];
    snprintf(buf, sizeof(buf), "queue_size=%zu", messageQueue.size());
    logMessage("INFO", "RESILIENCE", "Message queued", buf);

    return true;
}

String ResilienceManager::getNextQueuedMessage() {
    if (messageQueue.empty()) {
        return "";
    }

    QueuedMessage& msg = messageQueue.front();

    // Check if message is too old (> 5 minutes)
    if (millis() - msg.timestamp > 300000) {
        logMessage("WARN", "RESILIENCE", "Dropping stale message");
        messageQueue.erase(messageQueue.begin());
        return getNextQueuedMessage(); // Recursive call to get next
    }

    // Check retry count
    if (msg.retryCount >= MAX_RETRY_COUNT) {
        logMessage("WARN", "RESILIENCE", "Max retries reached, dropping message");
        messageQueue.erase(messageQueue.begin());
        return getNextQueuedMessage();
    }

    msg.retryCount++;
    return msg.json;
}

void ResilienceManager::clearMessageQueue() {
    size_t queueSize = messageQueue.size();
    messageQueue.clear();

    if (queueSize > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "cleared=%zu", queueSize);
        logMessage("INFO", "RESILIENCE", "Queue cleared", buf);
    }
}

bool ResilienceManager::processQueuedMessages() {
    if (messageQueue.empty() || !health.isHealthy) {
        return false;
    }

    size_t processed = 0;
    size_t initialSize = messageQueue.size();

    logMessage("INFO", "RESILIENCE", "Processing queued messages");

    while (!messageQueue.empty() && health.isHealthy) {
        String msg = getNextQueuedMessage();
        if (!msg.isEmpty()) {
            // In real implementation, this would send the message
            // For now, we just remove it from queue
            messageQueue.erase(messageQueue.begin());
            processed++;
        }
    }

    if (processed > 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "processed=%zu remaining=%zu", processed, messageQueue.size());
        logMessage("INFO", "RESILIENCE", "Queue processed", buf);
    }

    return processed > 0;
}

void ResilienceManager::markConnectionLost() {
    if (health.isHealthy) {
        health.isHealthy = false;
        health.lastDowntimeStart = millis();
        health.reconnectAttempts++;

        logMessage("WARN", "RESILIENCE", "Connection marked as unhealthy");
    }
}

void ResilienceManager::markConnectionRestored() {
    if (!health.isHealthy) {
        uint32_t downtimeDuration = millis() - health.lastDowntimeStart;
        health.totalDowntime += downtimeDuration;
        health.isHealthy = true;

        char buf[128];
        snprintf(buf, sizeof(buf), "downtime_ms=%lu total_downtime_ms=%lu",
                 downtimeDuration, health.totalDowntime);
        logMessage("INFO", "RESILIENCE", "Connection restored", buf);

        // Process any queued messages
        processQueuedMessages();
    }
}

uint32_t ResilienceManager::getTotalDowntime() {
    uint32_t total = health.totalDowntime;

    // Add current downtime if we're still down
    if (!health.isHealthy && health.lastDowntimeStart > 0) {
        total += (millis() - health.lastDowntimeStart);
    }

    return total;
}

String ResilienceManager::getHealthStatus() {
    JsonDocument doc;

    doc["healthy"] = health.isHealthy;
    doc["time_since_heartbeat"] = getTimeSinceLastHeartbeat();
    doc["missed_heartbeats"] = health.missedHeartbeats;
    doc["reconnect_attempts"] = health.reconnectAttempts;
    doc["total_downtime_ms"] = getTotalDowntime();
    doc["queue_size"] = messageQueue.size();

    // Calculate uptime percentage
    uint32_t totalTime = millis();
    uint32_t uptime = totalTime - getTotalDowntime();
    float uptimePercent = (totalTime > 0) ? (float(uptime) / float(totalTime) * 100.0) : 100.0;
    doc["uptime_percent"] = uptimePercent;

    String status;
    serializeJson(doc, status);
    return status;
}

void ResilienceManager::resetMetrics() {
    health.missedHeartbeats = 0;
    health.reconnectAttempts = 0;
    health.totalDowntime = 0;
    health.lastDowntimeStart = 0;
    clearMessageQueue();

    logMessage("INFO", "RESILIENCE", "Metrics reset");
}

void ResilienceManager::checkHealth() {
    static uint32_t lastCheck = 0;
    uint32_t now = millis();

    // Check every 5 seconds
    if (now - lastCheck < 5000) {
        return;
    }
    lastCheck = now;

    // Just track heartbeat metrics, don't trigger disconnections
    // The server and WebSocket library handle actual connection management
    uint32_t timeSince = getTimeSinceLastHeartbeat();
    if (timeSince > heartbeatTimeout) {
        health.missedHeartbeats = timeSince / heartbeatInterval;
        // Note: We do NOT call markConnectionLost() here anymore
        // Connection state is managed by WebSocket events only
    }

    // Log periodic health status in debug mode
    static uint32_t lastStatusLog = 0;
    if (now - lastStatusLog > 60000) {  // Every minute
        lastStatusLog = now;
        String status = getHealthStatus();
        logMessage("DEBUG", "RESILIENCE", "Health check", status.c_str());
    }
}