/*
 * =====================================================================
 *  ATHAR | أثر - ESP32-C3 SuperMini firmware (الجهاز الحقيقي)
 *  مبني على الكود اللي اشتغل معكم: نفس المكتبات ونفس المنافذ ونفس الشبكة
 *
 *  الإضافات:
 *   - عدادات حقيقية: مدة السكون (still) ومدة ثبات الضغط (stable)
 *   - تنبيه حقيقي: بدون حركة + ضغط ثابت للمدة المطلوبة  ->  alert = true
 *   - الحركة تنقرأ 20 مرة بالثانية (بدل مرة وحدة) عشان ما تفوت أي حركة
 *   - أوامر الموقع: "إيقاف الصوت" و "تم تغيير الوضعية" تشتغل فعلاً
 *   - سجل أحداث يطلع في الموقع
 *
 *  المكتبات: PubSubClient + ArduinoJson (نفس اللي عندكم)
 * =====================================================================
 */
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <math.h>

// -----------------------------------------------------------------------------
// إعدادات الـ Wi-Fi والـ MQTT (نفس إعداداتكم)
// -----------------------------------------------------------------------------
const char* WIFI_SSID     = "YOUR_WIFI_NAME";      // ← اسم شبكتك
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";  // ← كلمة المرور

const char* MQTT_SERVER   = "broker.hivemq.com";
const int   MQTT_PORT     = 1883;
const char* DEVICE_ID     = "pc-4821";        // نفس الرقم اللي تكتبه في الموقع

String topicStatus = "positioncare/v1/" + String(DEVICE_ID) + "/status";
String topicOnline = "positioncare/v1/" + String(DEVICE_ID) + "/online";
String topicCmd    = "positioncare/v1/" + String(DEVICE_ID) + "/cmd";

// -----------------------------------------------------------------------------
// المنافذ (نفس توصيلكم)
// -----------------------------------------------------------------------------
const int FSR_PIN = 2;   // حساس الضغط
#define SDA_PIN 9
#define SCL_PIN 8

// -----------------------------------------------------------------------------
// إعدادات التنبيه
// -----------------------------------------------------------------------------
// للتجربة: 10 ثواني  |  للاستخدام الحقيقي: 120 (دقيقتين)
const unsigned long STILL_SEC  = 10;   // مدة بدون حركة قبل التنبيه
const unsigned long STABLE_SEC = 10;   // مدة ثبات الضغط قبل التنبيه

// مستويات الضغط (نفس اللي عندكم)
const float LVL_LOW  = 5.0;    // من 5%  = خفيف (أخضر)   -> يعتبر فيه مريض
const float LVL_MED  = 50.0;   // من 50% = متوسط (أصفر)
const float LVL_HIGH = 85.0;   // من 85% = عالي (أحمر)
const float PRESSURE_CHANGE_PCT = 8.0;   // تغيّر أكبر من هذا = الضغط تغيّر (يصفّر العداد)

// الحركة
const float HEAVY_DELTA = 0.15;   // نفس شرطكم: |التسارع - 1g| > 0.15
const float MOVE_DELTA  = 0.06;   // تغيّر بين قراءتين متتاليتين (يلقط الحركة البطيئة / الميلان)

// -----------------------------------------------------------------------------
byte mpuAddress = 0x68;
bool mpuOk = false;

WiFiClient espClient;
PubSubClient client(espClient);

unsigned long lastPublishTime = 0, lastMotionSample = 0, lastFsrSample = 0;
unsigned long lastMotionMs = 0, pressureChangedMs = 0;
bool  movedSincePublish = false;     // حصلت حركة من آخر إرسال؟
bool  publishNow = false;
float prevGX = 0, prevGY = 0, prevGZ = 1;
bool  prevInit = false;
int16_t rawX = 0, rawY = 0, rawZ = 0;
float lastDelta = 0;

float pctSmooth = 0, pctRef = 0;
bool  loaded = false;
int   lvl = 0;

bool alertActive = false, muted = false;
unsigned long alertCount = 0;

// سجل الأحداث: 1 تنبيه ، 2 انتهى بالحركة/تغيّر الضغط ، 3 تم تغيير الوضعية (من الموقع)
struct Event { unsigned long id, t; int type; };
Event events[5];
int   eventCount = 0;
unsigned long eventSeq = 0;

void addEvent(int type) {
  for (int i = 4; i > 0; i--) events[i] = events[i - 1];
  events[0] = { ++eventSeq, millis() / 1000, type };
  if (eventCount < 5) eventCount++;
}

// -----------------------------------------------------------------------------
// أوامر الموقع
// -----------------------------------------------------------------------------
void callback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.print("📩 وصل أمر من الموقع: ");
  Serial.println(msg);

  if (msg.indexOf("reset") >= 0) {          // تم تغيير الوضعية
    unsigned long now = millis();
    lastMotionMs = now;
    pressureChangedMs = now;
    pctRef = pctSmooth;
    if (alertActive) { alertActive = false; Serial.println("✅ التنبيه انتهى (تم تغيير الوضعية من الموقع)"); }
    muted = false;
    addEvent(3);
    publishNow = true;
  } else if (msg.indexOf("ack") >= 0) {     // إيقاف الصوت
    muted = true;
    publishNow = true;
  }
}

// -----------------------------------------------------------------------------
// MPU6050
// -----------------------------------------------------------------------------
void initMPU() {
  Wire.beginTransmission(mpuAddress);
  Wire.write(0x6B);
  Wire.write(0x00);
  if (Wire.endTransmission() == 0) {
    mpuOk = true;
    Serial.print("✅ تم تفعيل MPU6050 على العنوان: 0x");
    Serial.println(mpuAddress, HEX);
  } else {
    mpuOk = false;
  }
}

// تقرأ الحساس وترجع true إذا فيه حركة (قوية أو ميلان/تحرك بطيء)
bool sampleMotion() {
  if (!mpuOk) return false;
  Wire.beginTransmission(mpuAddress);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) { initMPU(); return false; }
  if (Wire.requestFrom((uint8_t)mpuAddress, (uint8_t)6) != 6) return false;

  rawX = (Wire.read() << 8) | Wire.read();
  rawY = (Wire.read() << 8) | Wire.read();
  rawZ = (Wire.read() << 8) | Wire.read();
  if (rawX == 0 && rawY == 0 && rawZ == 0) return false;   // الحساس مو جاهز

  float gX = rawX / 16384.0, gY = rawY / 16384.0, gZ = rawZ / 16384.0;
  float total = sqrt(gX * gX + gY * gY + gZ * gZ);
  lastDelta = fabs(total - 1.0);
  bool heavy = lastDelta > HEAVY_DELTA;                    // شرطكم الأصلي

  float change = 0;
  if (prevInit) change = fabs(gX - prevGX) + fabs(gY - prevGY) + fabs(gZ - prevGZ);
  prevGX = gX; prevGY = gY; prevGZ = gZ; prevInit = true;

  return heavy || change > MOVE_DELTA;
}

// -----------------------------------------------------------------------------
// FSR (نفس العكس البرمجي حقكم)
// -----------------------------------------------------------------------------
void sampleFsr(unsigned long now) {
  long sum = 0;
  for (int i = 0; i < 8; i++) sum += analogRead(FSR_PIN);
  int fsrValue = 4095 - (int)(sum / 8);       // 0 بدون لمس، ويرتفع مع الضغط
  float pct = fsrValue * 100.0 / 4095.0;
  if (pct < 0) pct = 0;
  pctSmooth += 0.3 * (pct - pctSmooth);

  bool nowLoaded = pctSmooth >= LVL_LOW;
  if (nowLoaded != loaded || (nowLoaded && fabs(pctSmooth - pctRef) > PRESSURE_CHANGE_PCT)) {
    pctRef = pctSmooth;                       // الضغط تغيّر -> العداد يرجع صفر
    pressureChangedMs = now;
  } else {
    pctRef += 0.01 * (pctSmooth - pctRef);    // تذبذب بسيط: ما يصفّر العداد
  }
  loaded = nowLoaded;
  lvl = (pctSmooth >= LVL_HIGH) ? 3 : (pctSmooth >= LVL_MED) ? 2 : (pctSmooth >= LVL_LOW) ? 1 : 0;
}

// -----------------------------------------------------------------------------
// الشبكة (نفس دالتكم)
// -----------------------------------------------------------------------------
void connectToNetwork() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.print("جاري الاتصال بالـ Wi-Fi...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
    Serial.println("\n✅ تم الاتصال بالـ Wi-Fi بنجاح!");
  }

  if (!client.connected()) {
    Serial.print("جاري الاتصال بخادم MQTT...");
    client.setKeepAlive(60);
    String cid = String(DEVICE_ID) + "-" + String((uint32_t)random(0xFFFF), HEX);
    if (client.connect(cid.c_str(), topicOnline.c_str(), 0, true, "0")) {
      Serial.println("\n✅ تم الاتصال بخادم MQTT بنجاح!");
      client.publish(topicOnline.c_str(), "1", true);
      client.subscribe(topicCmd.c_str());
      publishNow = true;
    } else {
      Serial.print(" فشل الاتصال، الكود=");
      Serial.println(client.state());
      delay(2000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  pinMode(FSR_PIN, INPUT);
  analogReadResolution(12);

  Wire.begin(SDA_PIN, SCL_PIN, 100000);
  delay(100);

  mpuAddress = 0x68;
  initMPU();
  if (!mpuOk) { mpuAddress = 0x69; initMPU(); }
  if (!mpuOk) Serial.println("⚠️ لم يتم استجابة MPU6050، سيتم إرسال بيانات الضغط فقط.");

  WiFi.setSleep(false);
  client.setBufferSize(1024);
  client.setServer(MQTT_SERVER, MQTT_PORT);
  client.setCallback(callback);

  lastMotionMs = pressureChangedMs = millis();
  connectToNetwork();
}

// -----------------------------------------------------------------------------
void publishStatus(unsigned long now) {
  unsigned long still  = (now - lastMotionMs) / 1000;
  unsigned long stable = loaded ? (now - pressureChangedMs) / 1000 : 0;

  StaticJsonDocument<1024> doc;
  doc["v"]         = 1;
  doc["id"]        = DEVICE_ID;
  doc["name"]      = "Patient 01";
  doc["room"]      = "101";
  doc["uptime"]    = now / 1000;
  doc["still"]     = still;              // ثواني من آخر حركة (حقيقي)
  doc["stable"]    = stable;             // ثواني من آخر تغيّر في الضغط (حقيقي)
  doc["stillReq"]  = STILL_SEC;
  doc["stableReq"] = STABLE_SEC;
  doc["moving"]    = movedSincePublish;  // true إذا صار أي حركة من آخر إرسال
  doc["motionOk"]  = mpuOk;
  doc["loaded"]    = loaded;
  doc["alert"]     = alertActive;
  doc["muted"]     = muted;
  doc["alerts"]    = alertCount;

  JsonArray zones = doc.createNestedArray("zones");
  JsonObject z = zones.createNestedObject();
  z["id"]  = "sacrum";
  z["pct"] = round(pctSmooth * 10) / 10.0;
  z["lvl"] = lvl;

  JsonArray ev = doc.createNestedArray("events");
  for (int i = 0; i < eventCount; i++) {
    JsonObject e = ev.createNestedObject();
    e["id"] = events[i].id; e["t"] = events[i].t; e["type"] = events[i].type;
  }
  JsonObject th = doc.createNestedObject("th");
  th["min"] = LVL_LOW; th["med"] = LVL_MED; th["high"] = LVL_HIGH;

  String jsonOutput;
  serializeJson(doc, jsonOutput);
  client.publish(topicStatus.c_str(), jsonOutput.c_str(), true);   // retained

  Serial.print("📡 FSR: "); Serial.print(pctSmooth, 1); Serial.print("%");
  Serial.print(loaded ? " (فيه ضغط)" : " (ما فيه ضغط)");
  Serial.print(" | سكون: "); Serial.print(still); Serial.print("/"); Serial.print(STILL_SEC);
  Serial.print(" | ثبات: "); Serial.print(stable); Serial.print("/"); Serial.print(STABLE_SEC);
  Serial.print(" | MPU [X:"); Serial.print(rawX); Serial.print(" Y:"); Serial.print(rawY);
  Serial.print(" Z:"); Serial.print(rawZ); Serial.print("] Δ: "); Serial.print(lastDelta, 2);
  Serial.print(movedSincePublish ? " 🔥 [حركة!]" : " (ساكن)");
  Serial.println(alertActive ? (muted ? "  🚨 تنبيه (صامت)" : "  🚨 تنبيه!") : "");
  movedSincePublish = false;
}

void loop() {
  if (!client.connected()) connectToNetwork();
  client.loop();

  unsigned long now = millis();

  // الحركة: 20 مرة بالثانية
  if (now - lastMotionSample >= 50) {
    lastMotionSample = now;
    if (sampleMotion()) {
      lastMotionMs = now;
      if (!movedSincePublish) publishNow = true;   // أرسل الحركة للموقع على طول
      movedSincePublish = true;
    }
  }

  // الضغط: 10 مرات بالثانية
  if (now - lastFsrSample >= 100) {
    lastFsrSample = now;
    sampleFsr(now);
  }

  // التنبيه: بدون حركة + ضغط ثابت
  bool stillLong  = !mpuOk || (now - lastMotionMs) >= STILL_SEC * 1000UL;
  bool stableLong = loaded && (now - pressureChangedMs) >= STABLE_SEC * 1000UL;
  bool cond = stillLong && stableLong;
  if (cond && !alertActive) {
    alertActive = true; muted = false; alertCount++; addEvent(1); publishNow = true;
    Serial.println("🚨 تنبيه: المريض بدون حركة والضغط ثابت - غيّر وضعيته");
  } else if (!cond && alertActive) {
    alertActive = false; muted = false; addEvent(2); publishNow = true;
    Serial.println("✅ التنبيه انتهى (حركة أو تغيّر في الضغط)");
  }

  // الإرسال: كل ثانية، أو فوراً عند حركة/تنبيه
  if (publishNow || now - lastPublishTime >= 1000) {
    publishNow = false;
    lastPublishTime = now;
    publishStatus(now);
  }
}
