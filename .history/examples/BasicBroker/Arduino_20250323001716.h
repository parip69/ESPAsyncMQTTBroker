// Lokale Stub-Version von Arduino.h - nur für IntelliSense
// Diese Datei wird zur Kompilierung nicht verwendet - die echte Arduino.h wird vom Compiler gefunden

#ifndef ARDUINO_H
#define ARDUINO_H

// Standard-C-Header, die von Arduino-Sketchen verwendet werden
#ifdef __has_include
  #if __has_include(<stdint.h>)
    #include <stdint.h>
  #endif
  #if __has_include(<stddef.h>)
    #include <stddef.h>
  #endif
  #if __has_include(<stdlib.h>)
    #include <stdlib.h>
  #endif
#endif

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

// Minimale Implementierung der Arduino-Basisfunktionen
void delay(unsigned long ms);
unsigned long millis();
void pinMode(int pin, int mode);
void digitalWrite(int pin, int val);
int digitalRead(int pin);
int analogRead(int pin);
void analogWrite(int pin, int val);

// Pin-Zustände
#define HIGH 0x1
#define LOW  0x0

// Pin-Modi
#define INPUT 0x0
#define OUTPUT 0x1
#define INPUT_PULLUP 0x2

// Serielle Kommunikation
class SerialClass {
public:
    SerialClass() {}
    void begin(unsigned long speed) {}
    void println() {}
    void println(const char* str) {}
    void println(int val) {}
    void println(const String& str) {}
    void print(const char* str) {}
    void print(int val) {}
    void print(const String& str) {}
    void printf(const char* format, ...) {}
};

extern SerialClass Serial;

// Minimale String-Klasse (für die Fälle, in denen die String-Definition in ESPAsyncMQTTBroker.h nicht verfügbar ist)
#ifndef String
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
    String operator+(const String& s) const { return String(); }
};
#endif

#endif // ARDUINO_H
