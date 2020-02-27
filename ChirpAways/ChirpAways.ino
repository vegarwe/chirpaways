#include <Arduino.h>
#include <MQTT.h>
#include <WiFiClientSecure.h>
#include <Wire.h>

#include "config.h"


// WiFi and MQTT client
static WiFiClientSecure net;
static MQTTClient mqtt(384);


//Default I2C Address of the sensor
#define SOILMOISTURESENSOR_DEFAULT_ADDR     0x20

//Soil Moisture Sensor Register Addresses
#define SOILMOISTURESENSOR_GET_CAPACITANCE  0x00 // (r) 2 bytes
#define SOILMOISTURESENSOR_SET_ADDRESS      0x01 // (w) 1 byte
#define SOILMOISTURESENSOR_GET_ADDRESS      0x02 // (r) 1 byte
#define SOILMOISTURESENSOR_MEASURE_LIGHT    0x03 // (w) n/a
#define SOILMOISTURESENSOR_GET_LIGHT        0x04 // (r) 2 bytes
#define SOILMOISTURESENSOR_GET_TEMPERATURE  0x05 // (r) 2 bytes
#define SOILMOISTURESENSOR_RESET            0x06 // (w) n/a
#define SOILMOISTURESENSOR_GET_VERSION      0x07 // (r) 1 bytes
#define SOILMOISTURESENSOR_SLEEP            0x08 // (w) n/a
#define SOILMOISTURESENSOR_GET_BUSY         0x09 // (r) 1 bytes


static HardwareSerial* debugger = NULL;

static RTC_DATA_ATTR int bootCount = 0;

static RTC_DATA_ATTR struct {
    byte mac [ 6 ];
    byte mode;
    byte chl;
    uint32_t ip;
    uint32_t gw;
    uint32_t msk;
    uint32_t dns;
    uint32_t seq;
    uint32_t chk;
} cfgbuf;


static bool checkCfg()
{
    // See https://github.com/tve/low-power-wifi/blob/master/esp32-deep-sleep-mqtts/src/main.ino
    uint32_t x = 0;
    uint32_t *p = (uint32_t *)cfgbuf.mac;
    for (uint32_t i = 0; i < sizeof(cfgbuf)/4; i++) x += p[i];
    if (debugger) debugger->printf("RTC read: chk=%x x=%x ip=%08x mode=%d %s\n",
            cfgbuf.chk, x, cfgbuf.ip, cfgbuf.mode, x==0?"OK":"FAIL");
    if (x == 0 && cfgbuf.ip != 0) return true;
    if (debugger) debugger->println("NVRAM cfg init");
    // bad checksum, init data
    for (uint32_t i = 0; i < 6; i++) cfgbuf.mac[i] = 0xff;
    cfgbuf.mode = 0; // chk err, reconfig
    cfgbuf.chl = 0;
    cfgbuf.ip = IPAddress(0, 0, 0, 0);
    cfgbuf.gw = IPAddress(0, 0, 0, 0);
    cfgbuf.msk = IPAddress(255, 255, 255, 0);
    cfgbuf.dns = IPAddress(0, 0, 0, 0);
    cfgbuf.seq = 100;
    return false;
}


static void writecfg()
{
    // save new info
    uint8_t *bssid = WiFi.BSSID();
    for (int i=0; i<sizeof(cfgbuf.mac); i++) cfgbuf.mac[i] = bssid[i];
    cfgbuf.chl = WiFi.channel();
    cfgbuf.ip = WiFi.localIP();
    cfgbuf.gw = WiFi.gatewayIP();
    cfgbuf.msk = WiFi.subnetMask();
    cfgbuf.dns = WiFi.dnsIP();
    // recalculate checksum
    uint32_t x = 0;
    uint32_t *p = (uint32_t *)cfgbuf.mac;
    for (uint32_t i = 0; i < sizeof(cfgbuf)/4-1; i++) x += p[i];
    cfgbuf.chk = -x;
    if (debugger) debugger->printf("RTC write: chk=%x x=%x ip=%08x mode=%d\n", cfgbuf.chk, x, cfgbuf.ip, cfgbuf.mode);
}



//--------------------------------------------------------------------------------
static void mqtt_on_message(String &topic, String &payload)
{
    if (debugger) {
        debugger->print("mqtt_on_message: ");
        debugger->print("[");
        debugger->print(topic);
        debugger->print("] ");
        debugger->println(payload);
    }
}


unsigned int readI2CRegister16bit(int addr, int reg) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.endTransmission();
    delay(20); // TODO: Enough time? I2CSoilMoistureSensor.cpp seems to think so
    Wire.requestFrom(addr, 2);
    unsigned int t = Wire.read() << 8;
    t = t | Wire.read();
    return t;
}


void print_wakeup_reason() {
    if (! debugger) return;

    esp_sleep_wakeup_cause_t wakeup_reason;

    wakeup_reason = esp_sleep_get_wakeup_cause();

    switch(wakeup_reason)
    {
        case ESP_SLEEP_WAKEUP_EXT0:     debugger->println("Wakeup caused by external signal using RTC_IO"); break;
        case ESP_SLEEP_WAKEUP_EXT1:     debugger->println("Wakeup caused by external signal using RTC_CNTL"); break;
        case ESP_SLEEP_WAKEUP_TIMER:    debugger->println("Wakeup caused by timer"); break;
        case ESP_SLEEP_WAKEUP_TOUCHPAD: debugger->println("Wakeup caused by touchpad"); break;
        case ESP_SLEEP_WAKEUP_ULP:      debugger->println("Wakeup caused by ULP program"); break;
        default:                        debugger->printf("Wakeup was not caused by deep sleep: %d\n",wakeup_reason); break;
    }
}


//--------------------------------------------------------------------------------
void setup() {
    //debugger = &Serial;

    if (debugger)
    {
        debugger->begin(115200);
        delay(100);
        debugger->println("");
        debugger->println("Starting");
    }

#ifdef FEATHER_ESP32
    uint16_t analog_value = analogRead(35);
    if (analog_value < 2040)
    {
        // (2040 * 2 * 3.3 / 4095) + 0.366 ~= 3.65V
        // No wake-up just shutdown and protect the battery
        esp_deep_sleep_start();
    }

    if (debugger)
    {
        debugger->print("battery measurement: ");
        debugger->println(analog_value);
        // (2112 * 2 * 3.3 / 4095) + 0.366 ~= 3.77
        // (2444 * 2 * 3.3 / 4095) + 0.366 ~= 4.2 -- 4.3
    }
#else
    uint16_t analog_value = 0;
#endif

    ++bootCount;
    if (debugger) debugger->println("Boot number: " + String(bootCount));
    print_wakeup_reason();

    //pinMode(14, OUTPUT);
    //digitalWrite(14, HIGH);

    //Wire.begin();

    checkCfg();

    //if (WiFi.getMode() != WIFI_OFF)
    //{
    //    if (debugger) debugger->println("Wifi wasn't off!");
    //    WiFi.persistent(true);
    //    WiFi.mode(WIFI_OFF);
    //}

    //WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    bool ok;

    unsigned long fisken = millis();
    if (cfgbuf.mode == 2)
    {
        ok = WiFi.config(cfgbuf.ip, cfgbuf.gw, cfgbuf.msk, cfgbuf.dns);
        if (!ok && debugger) debugger->println("WiFi.config failed");
        ok = WiFi.begin(WIFI_SSID, WIFI_PASS);
        //ok = WiFi.begin(WIFI_SSID, WIFI_PASS, cfgbuf.chl, cfgbuf.mac);
        if (debugger) debugger->println("Using mode 2");
    }
    else
    {
        ok = WiFi.begin(WIFI_SSID, WIFI_PASS);
    }

    while (WiFi.status() != WL_CONNECTED) delay(1);
    if (debugger) debugger->printf("Connected  WiFi %lu\n", millis() - fisken);

    // Configure WiFiClientSecure to use the AWS certificates we generated
    net.setPreSharedKey(MQTT_IDNT, MQTT__PSK);

    // Setup MQTT
    fisken = millis();
    mqtt.begin(MQTT_HOST, MQTT_PORT, net);
    mqtt.onMessage(mqtt_on_message);
    if (! mqtt.connect(MQTT_NAME))
    {
        if (debugger) debugger->println("Unable to connect");
    }
    if (debugger) debugger->printf("Connected  MQTT %lu\n", millis() - fisken);

    cfgbuf.mode = 2;
    writecfg();

    unsigned long mqtt_connected = millis();

    //delay(1000); // give some time to boot up // TODO: Adaptive based on wifi_setup time

    //unsigned int fw_version         = readI2CRegister16bit(SOILMOISTURESENSOR_DEFAULT_ADDR, SOILMOISTURESENSOR_GET_VERSION);
    //unsigned int is_busy            = readI2CRegister16bit(SOILMOISTURESENSOR_DEFAULT_ADDR, SOILMOISTURESENSOR_GET_BUSY)
    //unsigned int soil_capacitance  = readI2CRegister16bit(SOILMOISTURESENSOR_DEFAULT_ADDR, SOILMOISTURESENSOR_GET_CAPACITANCE);
    //unsigned int temperature        = readI2CRegister16bit(SOILMOISTURESENSOR_DEFAULT_ADDR, SOILMOISTURESENSOR_GET_TEMPERATURE);
    unsigned int soil_capacitance  = millis();
    unsigned int temperature        = millis();

    if (debugger)
    {
        debugger->print("chirp!");
        //debugger->print(" sensor fw version: ");
        //debugger->print(fw_version, HEX);
        //debugger->print(", busy ");
        //debugger->print(is_busy, HEX);
        debugger->print(", soil moisture capacitance: ");
        debugger->print(soil_capacitance);
        debugger->print(", temperature: ");
        debugger->print(temperature / 10.0f);
        debugger->println();
    }

    //digitalWrite(14, LOW);

    mqtt.loop();
    static char payload[512];
    sprintf(payload,
            "{ \"temperature\": %d,"
            "  \"soil_capacitance\": %d,"
            "  \"battery_voltage\": %u,"
            "  \"millis\": %lu"
            "}",
            temperature,
            soil_capacitance,
            analog_value,
            mqtt_connected);
    mqtt.publish("chirp/sonsor", payload);
    mqtt.loop();

    //esp_sleep_enable_timer_wakeup(20 * 60 * 1000 * 1000);
    esp_sleep_enable_timer_wakeup(     10 * 1000 * 1000);
    if (debugger)
    {
        debugger->print("sleeping after");
        debugger->println(millis());
        debugger->flush();
    }
    esp_deep_sleep_start();
}

//--------------------------------------------------------------------------------
void loop()
{
}
