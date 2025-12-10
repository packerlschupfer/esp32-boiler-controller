/**
 * @file test_mqtt_integration.cpp
 * @brief Integration tests for MQTT communication with system components
 */

#include <unity.h>
#include <string>
#include <vector>
#include <queue>
#include <functional>
#include <map>
#include <algorithm>

// Mock includes
#include "mocks/MockSystemSettings.h"
#include "mocks/MockSharedSensorReadings.h"
#include "mocks/MockBurnerRequestManager.h"
#include "../../include/shared/Temperature.h"

// Mock MQTT client
class MockMQTTClient {
public:
    struct Message {
        std::string topic;
        std::string payload;
        int qos;
        bool retained;
    };
    
    using MessageCallback = std::function<void(const std::string& topic, const std::string& payload)>;
    
private:
    bool connected_;
    std::string clientId_;
    std::queue<Message> publishedMessages_;
    std::map<std::string, MessageCallback> subscriptions_;
    std::vector<std::string> subscribedTopics_;
    
public:
    MockMQTTClient(const std::string& clientId) 
        : connected_(false), clientId_(clientId) {}
    
    bool connect(const std::string& broker, int port) {
        connected_ = true;
        return true;
    }
    
    bool disconnect() {
        connected_ = false;
        subscriptions_.clear();
        subscribedTopics_.clear();
        return true;
    }
    
    bool isConnected() const {
        return connected_;
    }
    
    bool publish(const std::string& topic, const std::string& payload, 
                int qos = 0, bool retained = false) {
        if (!connected_) return false;
        
        publishedMessages_.push({topic, payload, qos, retained});
        
        // Simulate loopback for subscribed topics
        auto it = subscriptions_.find(topic);
        if (it != subscriptions_.end() && it->second) {
            it->second(topic, payload);
        }
        
        return true;
    }
    
    bool subscribe(const std::string& topic, MessageCallback callback) {
        if (!connected_) return false;
        
        subscriptions_[topic] = callback;
        subscribedTopics_.push_back(topic);
        return true;
    }
    
    bool unsubscribe(const std::string& topic) {
        if (!connected_) return false;
        
        subscriptions_.erase(topic);
        auto it = std::find(subscribedTopics_.begin(), subscribedTopics_.end(), topic);
        if (it != subscribedTopics_.end()) {
            subscribedTopics_.erase(it);
        }
        return true;
    }
    
    void simulateIncomingMessage(const std::string& topic, const std::string& payload) {
        // Direct match
        auto it = subscriptions_.find(topic);
        if (it != subscriptions_.end() && it->second) {
            it->second(topic, payload);
            return;
        }
        
        // Check wildcard subscriptions
        for (const auto& sub : subscriptions_) {
            if (sub.first.find("+") != std::string::npos) {
                // Simple wildcard matching - check prefix
                size_t wildcardPos = sub.first.find("+");
                std::string prefix = sub.first.substr(0, wildcardPos);
                if (topic.substr(0, prefix.length()) == prefix) {
                    sub.second(topic, payload);
                    return;
                }
            }
        }
    }
    
    Message getLastPublishedMessage() {
        if (publishedMessages_.empty()) {
            return {"", "", 0, false};
        }
        Message msg = publishedMessages_.back();
        return msg;
    }
    
    std::vector<Message> getAllPublishedMessages() {
        std::vector<Message> messages;
        while (!publishedMessages_.empty()) {
            messages.push_back(publishedMessages_.front());
            publishedMessages_.pop();
        }
        return messages;
    }
    
    size_t getPublishedMessageCount() const {
        return publishedMessages_.size();
    }
    
    const std::vector<std::string>& getSubscribedTopics() const {
        return subscribedTopics_;
    }
};

// Mock MQTT controller for system
class MockMQTTController {
private:
    MockMQTTClient* client_;
    SystemSettings* settings_;
    SharedSensorReadings* readings_;
    BurnerRequestManager* requestManager_;
    std::string baseTopic_;
    
public:
    MockMQTTController(MockMQTTClient* client, SystemSettings* settings,
                      SharedSensorReadings* readings, BurnerRequestManager* requestManager)
        : client_(client), settings_(settings), readings_(readings),
          requestManager_(requestManager), baseTopic_("esplan/boiler") {}
    
    bool initialize() {
        if (!client_->isConnected()) {
            return false;
        }
        
        // Subscribe to control topics
        client_->subscribe(baseTopic_ + "/control/enable", 
            [this](const std::string& topic, const std::string& payload) {
                handleEnableCommand(payload);
            });
            
        client_->subscribe(baseTopic_ + "/control/heating", 
            [this](const std::string& topic, const std::string& payload) {
                handleHeatingCommand(payload);
            });
            
        client_->subscribe(baseTopic_ + "/control/water", 
            [this](const std::string& topic, const std::string& payload) {
                handleWaterCommand(payload);
            });
            
        client_->subscribe(baseTopic_ + "/params/set/+", 
            [this](const std::string& topic, const std::string& payload) {
                handleParameterSet(topic, payload);
            });
            
        return true;
    }
    
    void publishStatus() {
        if (!client_->isConnected()) return;
        
        // Publish system state
        std::string state = settings_->heatingEnable ? "heating" : "idle";
        client_->publish(baseTopic_ + "/state/system", state, 0, true);
        
        // Publish temperatures
        char buffer[256];
        snprintf(buffer, sizeof(buffer), 
                "{\"boiler_in\":%.1f,\"boiler_out\":%.1f,\"water\":%.1f,\"inside\":%.1f}",
                readings_->boilerTempInput / 10.0f,
                readings_->boilerTempOutput / 10.0f,
                readings_->waterTemp / 10.0f,
                readings_->insideTemp / 10.0f);
        client_->publish(baseTopic_ + "/state/temperatures", buffer);
        
        // Publish burner state
        auto request = requestManager_->getCurrentRequest();
        std::string burnerState = "off";
        if (request.source != BurnerRequestManager::RequestSource::NONE) {
            burnerState = request.powerPercent > 50 ? "full" : "half";
        }
        client_->publish(baseTopic_ + "/state/burner", burnerState);
    }
    
    void publishDiagnostics() {
        if (!client_->isConnected()) return;
        
        // Publish diagnostics
        client_->publish(baseTopic_ + "/diagnostics/health", "ok");
        client_->publish(baseTopic_ + "/diagnostics/uptime", "3600");
    }
    
private:
    void handleEnableCommand(const std::string& payload) {
        if (payload == "on") {
            settings_->heatingEnable = true;
            requestManager_->clearEmergencyStop();
        } else if (payload == "off") {
            settings_->heatingEnable = false;
            requestManager_->emergencyStop();
        }
    }
    
    void handleHeatingCommand(const std::string& payload) {
        if (payload == "on") {
            requestManager_->requestHeating(settings_->heating_target_temperature, 100);
        } else if (payload == "off") {
            requestManager_->clearHeatingRequest();
        }
    }
    
    void handleWaterCommand(const std::string& payload) {
        if (payload == "on") {
            settings_->wHeaterEnable = true;
        } else if (payload == "off") {
            settings_->wHeaterEnable = false;
            requestManager_->clearWaterRequest();
        }
    }
    
    void handleParameterSet(const std::string& topic, const std::string& payload) {
        // Extract parameter name from topic
        size_t lastSlash = topic.find_last_of('/');
        if (lastSlash == std::string::npos) return;
        
        std::string param = topic.substr(lastSlash + 1);
        float value = std::stof(payload);
        
        // Update parameter
        if (param == "heating_target") {
            settings_->heating_target_temperature = tempFromFloat(value);
        } else if (param == "water_low") {
            settings_->wHeaterConfTempLimitLow = tempFromFloat(value);
        }
    }
};

// Test fixtures
static MockMQTTClient* mqttClient = nullptr;
static MockMQTTController* mqttController = nullptr;
static SystemSettings* settings = nullptr;
static SharedSensorReadings* readings = nullptr;
static BurnerRequestManager* requestManager = nullptr;

void setupMQTTIntegration() {
    settings = new SystemSettings();
    readings = new SharedSensorReadings();
    requestManager = new BurnerRequestManager();
    mqttClient = new MockMQTTClient("test-client");
    mqttController = new MockMQTTController(mqttClient, settings, readings, requestManager);
}

void tearDownMQTTIntegration() {
    delete mqttController;
    delete mqttClient;
    delete requestManager;
    delete readings;
    delete settings;
}

// Test MQTT connection and initialization
void test_mqtt_connection() {
    setupMQTTIntegration();
    
    // Connect to broker
    TEST_ASSERT_TRUE(mqttClient->connect("test.broker", 1883));
    TEST_ASSERT_TRUE(mqttClient->isConnected());
    
    // Initialize controller
    TEST_ASSERT_TRUE(mqttController->initialize());
    
    // Verify subscriptions
    auto topics = mqttClient->getSubscribedTopics();
    TEST_ASSERT_EQUAL(4, topics.size());
    TEST_ASSERT(std::find(topics.begin(), topics.end(), 
                         "esplan/boiler/control/enable") != topics.end());
    
    tearDownMQTTIntegration();
}

// Test status publishing
void test_status_publishing() {
    setupMQTTIntegration();
    
    mqttClient->connect("test.broker", 1883);
    mqttController->initialize();
    
    // Set up test data
    readings->boilerTempInput = tempFromFloat(65.5f);
    readings->boilerTempOutput = tempFromFloat(70.2f);
    readings->waterTemp = tempFromFloat(55.0f);
    readings->insideTemp = tempFromFloat(21.5f);
    settings->heatingEnable = true;
    
    // Publish status
    mqttController->publishStatus();
    
    // Verify messages
    auto messages = mqttClient->getAllPublishedMessages();
    TEST_ASSERT_EQUAL(3, messages.size());
    
    // Check system state
    TEST_ASSERT_EQUAL_STRING("esplan/boiler/state/system", messages[0].topic.c_str());
    TEST_ASSERT_EQUAL_STRING("heating", messages[0].payload.c_str());
    TEST_ASSERT_TRUE(messages[0].retained);
    
    // Check temperatures
    TEST_ASSERT_EQUAL_STRING("esplan/boiler/state/temperatures", messages[1].topic.c_str());
    TEST_ASSERT(messages[1].payload.find("65.5") != std::string::npos);
    TEST_ASSERT(messages[1].payload.find("70.2") != std::string::npos);
    
    tearDownMQTTIntegration();
}

// Test remote control commands
void test_remote_control() {
    setupMQTTIntegration();
    
    mqttClient->connect("test.broker", 1883);
    mqttController->initialize();
    
    // Test enable/disable
    mqttClient->simulateIncomingMessage("esplan/boiler/control/enable", "off");
    TEST_ASSERT_FALSE(settings->heatingEnable);
    
    mqttClient->simulateIncomingMessage("esplan/boiler/control/enable", "on");
    TEST_ASSERT_TRUE(settings->heatingEnable);
    
    // Test heating control
    mqttClient->simulateIncomingMessage("esplan/boiler/control/heating", "on");
    auto request = requestManager->getCurrentRequest();
    TEST_ASSERT_EQUAL(BurnerRequestManager::RequestSource::HEATING, request.source);
    
    mqttClient->simulateIncomingMessage("esplan/boiler/control/heating", "off");
    request = requestManager->getCurrentRequest();
    TEST_ASSERT_EQUAL(BurnerRequestManager::RequestSource::NONE, request.source);
    
    tearDownMQTTIntegration();
}

// Test parameter updates via MQTT
void test_parameter_updates() {
    setupMQTTIntegration();
    
    mqttClient->connect("test.broker", 1883);
    mqttController->initialize();
    
    // Update heating target
    mqttClient->simulateIncomingMessage("esplan/boiler/params/set/heating_target", "23.5");
    TEST_ASSERT_EQUAL_INT16(235, settings->heating_target_temperature);  // 23.5 * 10
    
    // Update water low limit
    mqttClient->simulateIncomingMessage("esplan/boiler/params/set/water_low", "48.0");
    TEST_ASSERT_EQUAL_INT16(480, settings->wHeaterConfTempLimitLow);  // 48.0 * 10
    
    tearDownMQTTIntegration();
}

// Test emergency stop via MQTT
void test_emergency_stop() {
    setupMQTTIntegration();
    
    mqttClient->connect("test.broker", 1883);
    mqttController->initialize();
    
    // Start heating
    requestManager->requestHeating(tempFromFloat(70.0f), 100);
    TEST_ASSERT(requestManager->getCurrentRequest().source != 
               BurnerRequestManager::RequestSource::NONE);
    
    // Emergency stop
    mqttClient->simulateIncomingMessage("esplan/boiler/control/enable", "off");
    
    // Verify emergency stop
    TEST_ASSERT_FALSE(settings->heatingEnable);
    TEST_ASSERT_EQUAL(BurnerRequestManager::RequestSource::EMERGENCY, 
                     requestManager->getCurrentRequest().source);
    
    tearDownMQTTIntegration();
}

// Test diagnostics publishing
void test_diagnostics_publishing() {
    setupMQTTIntegration();
    
    mqttClient->connect("test.broker", 1883);
    mqttController->initialize();
    
    // Publish diagnostics
    mqttController->publishDiagnostics();
    
    // Verify messages
    auto messages = mqttClient->getAllPublishedMessages();
    TEST_ASSERT(messages.size() >= 2);
    
    bool foundHealth = false;
    bool foundUptime = false;
    
    for (const auto& msg : messages) {
        if (msg.topic == "esplan/boiler/diagnostics/health") {
            foundHealth = true;
            TEST_ASSERT_EQUAL_STRING("ok", msg.payload.c_str());
        }
        if (msg.topic == "esplan/boiler/diagnostics/uptime") {
            foundUptime = true;
        }
    }
    
    TEST_ASSERT(foundHealth);
    TEST_ASSERT(foundUptime);
    
    tearDownMQTTIntegration();
}

// Test connection loss handling
void test_connection_loss() {
    setupMQTTIntegration();
    
    mqttClient->connect("test.broker", 1883);
    mqttController->initialize();
    
    // Publish should work when connected
    mqttController->publishStatus();
    TEST_ASSERT(mqttClient->getPublishedMessageCount() > 0);
    
    // Clear messages
    mqttClient->getAllPublishedMessages();
    
    // Disconnect
    mqttClient->disconnect();
    
    // Publish should not work when disconnected
    mqttController->publishStatus();
    TEST_ASSERT_EQUAL(0, mqttClient->getPublishedMessageCount());
    
    tearDownMQTTIntegration();
}

// Test QoS and retained messages
void test_qos_and_retention() {
    setupMQTTIntegration();
    
    mqttClient->connect("test.broker", 1883);
    mqttController->initialize();
    
    settings->heatingEnable = true;
    mqttController->publishStatus();
    
    auto messages = mqttClient->getAllPublishedMessages();
    
    // Find system state message
    for (const auto& msg : messages) {
        if (msg.topic == "esplan/boiler/state/system") {
            // System state should be retained
            TEST_ASSERT_TRUE(msg.retained);
            TEST_ASSERT_EQUAL(0, msg.qos);  // Using QoS 0 for now
        }
    }
    
    tearDownMQTTIntegration();
}

// main() is in test_main.cpp