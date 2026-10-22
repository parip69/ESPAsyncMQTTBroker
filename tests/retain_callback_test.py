"""Originalen Retain-Callback mit erhaltener Text-/Binaerschnittstelle pruefen."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/ESPAsyncMQTTBroker.cpp').read_text()
a=s.index('void ESPAsyncMQTTBroker::notifyMessage(')
b=s.index('void ESPAsyncMQTTBroker::disconnectClient(',a)
method=s[a:b]
code=r'''
#include <cassert>
#include <cstdint>
#include <functional>
#include <string>
#include <cstdio>
using String=std::string;
unsigned conversions=0;
String mqttString(const uint8_t*p,size_t n){++conversions;return String(reinterpret_cast<const char*>(p),n);}
struct ESPAsyncMQTTBroker {
 std::function<void(const String&,const String&,const String&)> messageCallback;
 std::function<void(const String&,const String&,const uint8_t*,size_t)> binaryMessageCallback;
 std::function<void(const String&,const String&,const String&,bool)> retainedMessageCallback;
 void notifyMessage(const String&,const String&,const uint8_t*,size_t,bool);
};
PRODUKT
int main(){
 ESPAsyncMQTTBroker b;unsigned texts=0,binaries=0,properties=0;bool flag=false;
 const uint8_t payload[]={'o','n'};
 b.binaryMessageCallback=[&](const String&id,const String&topic,const uint8_t*p,size_t n){assert(id=="client"&&topic=="out"&&p==payload&&n==2);++binaries;};
 b.notifyMessage("client","out",payload,2,true);assert(binaries==1&&conversions==0);
 b.messageCallback=[&](const String&,const String&,const String&value){assert(value=="on");++texts;};
 b.retainedMessageCallback=[&](const String&,const String&,const String&value,bool retain){assert(value=="on");++properties;flag=retain;};
 b.notifyMessage("client","out",payload,2,true);assert(flag&&texts==1&&properties==1&&conversions==1);
 b.notifyMessage("client","out",payload,2,false);assert(!flag&&texts==2&&properties==2&&conversions==2);
 b.messageCallback=nullptr;b.notifyMessage("client","out",payload,2,true);assert(flag&&properties==3&&texts==2);
 puts("PASS Retain true/false, alter Textcallback, Binaercallback und nur eine Textkonvertierung");
}
'''.replace('PRODUKT',method)
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-no-pie',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
assert 'notifyMessage(source, topic, data + offset, payloadLength, retained)' in s
assert 'message.payload_len, message.retained)' in s
print('PASS QoS-0/1- und QoS-2-Aufrufstellen reichen Retain weiter')
