// Minimale Header-Version nur für IntelliSense
// Die tatsächliche Implementierung wird aus der Bibliothek geladen

#ifndef ESP_ASYNC_MQTT_BROKER_H
#define ESP_ASYNC_MQTT_BROKER_H

// Minimale Definitionen für uint-Typen ohne stdint.h
#ifndef UINT8_T_DEFINED
#define UINT8_T_DEFINED
typedef unsigned char uint8_t;
#endif
#ifndef UINT16_T_DEFINED
#define UINT16_T_DEFINED
typedef unsigned short uint16_t;
#endif
#ifndef UINT32_T_DEFINED
#define UINT32_T_DEFINED
typedef unsigned int uint32_t;
#endif
#ifndef SIZE_T_DEFINED
#define SIZE_T_DEFINED
typedef unsigned int size_t;
#endif

// Minimale String-Klasse für IntelliSense
class String {
public:
    String() {}
    String(const char* s) {}
    String(const String& s) {}
    bool isEmpty() const { return true; }
    const char* c_str() const { return ""; }
    int indexOf(char c, int from = 0) const { return -1; }
    String substring(int start, int end = -1) const { return String(); }
    int length() const { return 0; }
    bool operator==(const String& s) const { return true; }
    bool operator!=(const String& s) const { return false; }
};

// Debug Level
enum DebugLevel {
    DEBUG_NONE = 0,
    DEBUG_ERROR = 1,
    DEBUG_INFO = 2,
    DEBUG_DEBUG = 3
};

// Konfigurationsstruktur für den MQTT-Broker
struct ESPAsyncMQTTBrokerConfig {
    String username;
    String password;
};

// Typedefs für Callbacks
typedef void (*ClientCallback)(String, String);
typedef void (*MessageCallback)(String, String, String);
typedef void (*ClientDisconnectCallback)(String);
typedef void (*ErrorCallback)(String, int, const String&);
typedef void (*SubscribeCallback)(String, const String&);
typedef void (*UnsubscribeCallback)(String, const String&);

// Die Hauptklasse - nur mit minimalen Stub-Implementierungen für IntelliSense
class ESPAsyncMQTTBroker {
public:
    ESPAsyncMQTTBroker(uint16_t port = 1883) {}
    ~ESPAsyncMQTTBroker() {}
    
    void begin() {}
    void stop() {}
    
    void setConfig(const ESPAsyncMQTTBrokerConfig& config) {}
    void setDebugLevel(DebugLevel level) {}
    
    void onClientConnect(ClientCallback callback) {}
    void onMessage(MessageCallback callback) {}
    void onClientDisconnect(ClientDisconnectCallback callback) {}
    void onError(ErrorCallback callback) {}
    void onSubscribe(SubscribeCallback callback) {}
    void onUnsubscribe(UnsubscribeCallback callback) {}
};

#endif // ESP_ASYNC_MQTT_BROKER_H
