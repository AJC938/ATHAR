/*
 * =====================================================================
 *  ATHAR - (تجربة 5 ثواني) - Wokwi أو الجهاز الحقيقي
 *  ESP32-C3 + MPU6050 + FSR 402 + اتصال بالموقع (MQTT)
 *
 *  الضغط (FSR):
 *    0  - 40%  = أخضر  (خفيف)
 *    40 - 75%  = أصفر  (متوسط)
 *    75 - 100% = أحمر  (عالي)
 *
 *  التنبيه: بدون حركة 5 ثواني + الضغط ثابت 5 ثواني
 *
 *  التوصيل:
 *    MPU6050: VCC->3V3 , GND->GND , SDA->D8 , SCL->D9
 *    FSR:     OUT->D2 (في الجهاز الحقيقي: 3.3V -> FSR -> D2 -> مقاومة 10k -> GND)
 *    Buzzer:  + -> D3 , - -> GND
 *
 *  الاتصال بالموقع:
 *    الجهاز يرسل القراءات كل ثانية إلى broker.hivemq.com (MQTT)
 *    والموقع يستقبلها من نفس المكان باستخدام DEVICE_ID.
 *    ما يحتاج أي مكتبة إضافية: اتصال MQTT مكتوب داخل الكود (MiniMQTT)
 * =====================================================================
 */
#include <Wire.h>
#include <math.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// ---------------- الاتصال بالموقع ----------------
// ⚠️ غيّر DEVICE_ID لرمز خاص فيك (مثلاً pc-ahmad-77) - الـ broker عام،
//    وأي أحد يستخدم نفس الرمز يشوف نفس البيانات. حط نفس الرمز في الموقع.
#define DEVICE_ID     "pc-ajc938"
#define PATIENT_NAME  "Patient 01"
#define PATIENT_ROOM  "101"
#define FSR_ZONE      "sacrum"        // منطقة الحساس في خريطة الجسم

// ---------------- الواي فاي ----------------
// 1 = محاكاة Wokwi   |   0 = جهاز حقيقي (اكتب اسم شبكتك وكلمة المرور تحت)
#define USE_WOKWI     1

#if USE_WOKWI
  #define WIFI_SSID     "Wokwi-GUEST"
  #define WIFI_PASS     ""
  #define WIFI_CHANNEL  6
#else
  #define WIFI_SSID     "اسم_شبكتك"        // ← غيّره
  #define WIFI_PASS     "كلمة_المرور"       // ← غيّره
  #define WIFI_CHANNEL  0
#endif

#define MQTT_HOST     "broker.hivemq.com"
#define MQTT_PORT     1883
#define TOPIC_BASE    "positioncare/v1/" DEVICE_ID

// (اختياري) تنبيه على الجوال عن طريق تطبيق ntfy - حط اسم topic واشترك فيه بالتطبيق
// اتركه "" عشان يكون مقفل
#define NTFY_TOPIC    ""

// ---------------------------------------------------------------------
//  MiniMQTT: اتصال MQTT 3.1.1 مكتوب داخل الكود (بدل مكتبة PubSubClient)
//  يدعم: اتصال + رسالة الانقطاع (LWT) + نشر + اشتراك + استقبال + ping
// ---------------------------------------------------------------------
class MiniMQTT {
public:
  typedef void (*Callback)(char *topic, byte *payload, unsigned int length);
  explicit MiniMQTT(Client &c) : cl(c) {}
  void setServer(const char *h, uint16_t p) { host = h; port = p; }
  void setCallback(Callback f) { cb = f; }
  void setBufferSize(uint16_t) {}                       // حجم ثابت (1024)
  void setSocketTimeout(uint16_t s) { sockTimeout = s; }
  void setKeepAlive(uint16_t s) { keepAlive = s; }
  int  state() { return st; }

  bool connected() {
    if (st == 0 && !cl.connected()) { st = -3; cl.stop(); }
    return st == 0;
  }

  void disconnect() {
    if (cl.connected()) { uint8_t d[2] = {0xE0, 0x00}; cl.write(d, 2); }
    cl.stop();
    st = -1;
  }

  bool connect(const char *id, const char *willTopic, uint8_t willQos, bool willRetain, const char *willMsg) {
    cl.stop();
    if (!cl.connect(host, port)) { st = -2; return false; }
    size_t n = 0;
    putStr(n, "MQTT");
    body[n++] = 4;                                                // MQTT 3.1.1
    body[n++] = 0x02 | 0x04 | ((willQos & 3) << 3) | (willRetain ? 0x20 : 0);  // clean + will
    body[n++] = keepAlive >> 8; body[n++] = keepAlive & 0xFF;
    putStr(n, id); putStr(n, willTopic); putStr(n, willMsg);
    if (!send(0x10, n)) { st = -2; cl.stop(); return false; }

    uint8_t hdr, rc[2];                                           // CONNACK
    if (!readByte(hdr) || hdr != 0x20 || !readByte(rc[0]) || rc[0] != 2 ||
        !readByte(rc[0]) || !readByte(rc[1])) { st = -4; cl.stop(); return false; }
    if (rc[1] != 0) { st = rc[1]; cl.stop(); return false; }
    st = 0;
    lastIn = lastOut = millis();
    return true;
  }

  bool publish(const char *topic, const char *payload, bool retained = false) {
    if (!connected()) return false;
    size_t n = 0, pl = strlen(payload);
    if (!putStr(n, topic) || n + pl > sizeof(body)) return false;
    memcpy(body + n, payload, pl); n += pl;
    return send(0x30 | (retained ? 1 : 0), n);
  }

  bool subscribe(const char *topic) {
    if (!connected()) return false;
    size_t n = 0;
    if (++packetId == 0) packetId = 1;
    body[n++] = packetId >> 8; body[n++] = packetId & 0xFF;
    if (!putStr(n, topic)) return false;
    body[n++] = 0;                                                // QoS 0
    return send(0x82, n);
  }

  bool loop() {
    if (!connected()) return false;
    uint32_t now = millis();
    if (now - lastIn > keepAlive * 1500UL) { disconnect(); st = -3; return false; }   // الخادم ما يرد
    if (now - lastOut >= keepAlive * 1000UL) {                     // ping
      uint8_t d[2] = {0xC0, 0x00};
      cl.write(d, 2); lastOut = now;
    }
    while (cl.available() > 0) {
      uint8_t hdr; uint32_t len = 0, mul = 1; uint8_t b;
      if (!readByte(hdr)) break;
      do { if (!readByte(b)) { disconnect(); return false; } len += (b & 0x7F) * mul; mul <<= 7; } while ((b & 0x80) && mul < (1UL << 28));
      if (len >= sizeof(body)) {                                  // رسالة كبيرة: نتجاهلها
        for (uint32_t i = 0; i < len; i++) if (!readByte(b)) break;
        continue;
      }
      for (uint32_t i = 0; i < len; i++) if (!readByte(body[i])) { disconnect(); return false; }
      lastIn = millis();
      if ((hdr & 0xF0) == 0x30 && len >= 2) {                     // PUBLISH وارد
        uint16_t tl = (body[0] << 8) | body[1];
        size_t off = 2 + tl + (((hdr >> 1) & 3) ? 2 : 0);
        if (off > len) continue;
        char topic[128];
        size_t tcopy = tl < sizeof(topic) - 1 ? tl : sizeof(topic) - 1;
        memcpy(topic, body + 2, tcopy); topic[tcopy] = 0;
        if (cb) cb(topic, body + off, len - off);
      }
    }
    return true;
  }

private:
  Client     &cl;
  const char *host = "";
  uint16_t   port = 1883, keepAlive = 15, sockTimeout = 3;
  int        st = -1;
  uint16_t   packetId = 0;
  uint32_t   lastIn = 0, lastOut = 0;
  Callback   cb = nullptr;
  uint8_t    body[1024];
  uint8_t    pkt[1029];

  bool putStr(size_t &n, const char *s) {
    size_t L = strlen(s);
    if (n + 2 + L > sizeof(body)) return false;
    body[n++] = L >> 8; body[n++] = L & 0xFF;
    memcpy(body + n, s, L); n += L;
    return true;
  }

  bool send(uint8_t header, size_t len) {
    size_t i = 0;
    pkt[i++] = header;
    size_t x = len;
    do { uint8_t d = x % 128; x /= 128; if (x) d |= 0x80; pkt[i++] = d; } while (x);
    memcpy(pkt + i, body, len);
    size_t total = i + len;
    bool ok = cl.write(pkt, total) == total;
    if (ok) lastOut = millis();
    return ok;
  }

  bool readByte(uint8_t &b) {
    uint32_t t0 = millis();
    while (!cl.available()) {
      if (!cl.connected() || millis() - t0 > sockTimeout * 1000UL) return false;
      delay(1);
    }
    int v = cl.read();
    if (v < 0) return false;
    b = (uint8_t)v;
    return true;
  }
};

// ---------------- المنافذ (تتغير تلقائياً حسب اللوحة) ----------------
#if defined(CONFIG_IDF_TARGET_ESP32)
  // ESP32 DevKit العادية
  #define SDA_PIN     21
  #define SCL_PIN     22
  #define POT_PIN     34
  #define LED_GREEN   25
  #define LED_YELLOW  26
  #define LED_RED     27
  #define BUZZER_PIN  14
#else
  // XIAO ESP32-C3  (توصيلكم الحالي)
  #define SDA_PIN     8     // D8 (GPIO8) -> MPU6050 SDA
  #define SCL_PIN     9     // D9 (GPIO9) -> MPU6050 SCL
  #define POT_PIN     4     // D2 (GPIO4) -> FSR / السلايدر
  #define BUZZER_PIN  5     // D3 (GPIO5) -> Buzzer +
  #define LED_GREEN   -1    // ليدات اختيارية: حط رقم المنفذ لو ركبتها، و -1 = ما فيه
  #define LED_YELLOW  -1
  #define LED_RED     -1
#endif
#define MPU_ADDR    0x68

// لو زيادة الضغط تنقّص النسبة بدل ما يزيدها، خلها 1
#define INVERT_SLIDER 0

// ---------------- الأوقات ----------------
// للتجربة: 5 ثواني  |  للاستخدام الحقيقي: 120 (دقيقتين)
#define STILL_SEC   5     // مدة بدون حركة قبل التنبيه
#define STABLE_SEC  5     // مدة ثبات الضغط قبل التنبيه

// ---------------- حدود الألوان ----------------
#define MIN_PCT     3.0f    // أقل من هذا = ما فيه ضغط
#define MED_PCT     40.0f   // من هنا أصفر
#define HIGH_PCT    75.0f   // من هنا أحمر
#define DELTA_PCT   8.0f    // تغير أكبر من هذا = الضغط تغيّر
#define ACC_THR_G   0.06f   // تغير التسارع اللي يعتبر حركة
#define GYRO_THR    12.0f   // دوران (درجة/ثانية) يعتبر حركة

// ---------------- المتغيرات ----------------
bool     mpuOk = false;
float    baseAx, baseAy, baseAz, gbx = 0, gby = 0, gbz = 0;
bool     baseInit = false;
uint8_t  motionHits = 0;
bool     moving = false;
uint32_t lastMotionMs = 0;

float    pct = 0, ref = 0;
bool     loaded = false, potInit = false;
uint8_t  level = 0;
uint32_t pressureChangedMs = 0;

bool     alertActive = false;
bool     muted = false;          // إيقاف الصوت من الموقع
uint32_t alertCount = 0;
uint32_t lastSample = 0, lastPrint = 0;

// سجل الأحداث: 1 تنبيه ، 2 انتهى بالحركة/تغير الضغط ، 3 تم تغيير الوضعية (زر)
#define EVENT_LOG_SIZE 10
struct Event { uint32_t id; uint32_t t; uint8_t type; };
Event    events[EVENT_LOG_SIZE];
uint32_t eventSeq = 0;

// الشبكة
WiFiClient   net;
MiniMQTT     mqtt(net);
bool     wifiWasUp = false;
uint32_t lastWifiTry = 0;
uint32_t lastWifiLog = 0;
uint32_t wifiStartMs = 0;
uint32_t lastMqttTry = 0;
uint32_t mqttBackoff = 2000;
uint32_t lastPublish = 0;
bool     publishNow = false;
bool     ntfyPending = false;

// ---------------- الطباعة ----------------
//  Wokwi = 1 (يقرأ USB)   |   TIF Sandbox = 0 (يقرأ UART0)
#define USE_USB_SERIAL 1

#if USE_USB_SERIAL || !ARDUINO_USB_CDC_ON_BOOT
  #define Out Serial       // Wokwi / الجهاز الحقيقي
#else
  #define Out Serial0      // UART0 (لمحاكي TIF Sandbox)
#endif

const char* LEVEL_NAME[] = {"NO PRESSURE", "GREEN  (low)", "YELLOW (medium)", "RED    (high)"};

void addEvent(uint8_t type) {
  eventSeq++;
  Event &e = events[eventSeq % EVENT_LOG_SIZE];
  e.id = eventSeq;
  e.t = millis() / 1000;
  e.type = type;
}

// ---------------------------------------------------------------------
//  MPU6050
// ---------------------------------------------------------------------
bool mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool mpuRead(float &ax, float &ay, float &az, float &gx, float &gy, float &gz) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)MPU_ADDR, 14) != 14) return false;
  int16_t v[7];
  for (int i = 0; i < 7; i++) {
    uint8_t hi = Wire.read(), lo = Wire.read();
    v[i] = (int16_t)((hi << 8) | lo);
  }
  ax = v[0] / 16384.0f; ay = v[1] / 16384.0f; az = v[2] / 16384.0f;
  gx = v[4] / 131.0f;   gy = v[5] / 131.0f;   gz = v[6] / 131.0f;
  return true;
}

bool mpuInit() {
  if (!mpuWrite(0x6B, 0x00)) return false;   // تشغيل
  delay(50);
  mpuWrite(0x1A, 0x03);
  mpuWrite(0x1B, 0x00);   // ±250°/s
  mpuWrite(0x1C, 0x00);   // ±2g
  float sx = 0, sy = 0, sz = 0, ax, ay, az, gx, gy, gz;
  int n = 0;
  for (int i = 0; i < 50; i++) {
    if (mpuRead(ax, ay, az, gx, gy, gz)) { sx += gx; sy += gy; sz += gz; n++; }
    delay(5);
  }
  if (n < 25) return false;
  gbx = sx / n; gby = sy / n; gbz = sz / n;
  return true;
}

void updateMotion(uint32_t now) {
  if (!mpuOk) { moving = false; return; }
  float ax, ay, az, gx, gy, gz;
  if (!mpuRead(ax, ay, az, gx, gy, gz)) return;
  if (!baseInit) { baseAx = ax; baseAy = ay; baseAz = az; baseInit = true; }

  float dA = sqrtf(sq(ax - baseAx) + sq(ay - baseAy) + sq(az - baseAz));
  float gM = sqrtf(sq(gx - gbx) + sq(gy - gby) + sq(gz - gbz));
  baseAx += 0.05f * (ax - baseAx);
  baseAy += 0.05f * (ay - baseAy);
  baseAz += 0.05f * (az - baseAz);

  bool hit = dA > ACC_THR_G || gM > GYRO_THR;
  motionHits = hit ? (motionHits < 255 ? motionHits + 1 : 255) : 0;
  moving = motionHits >= 2;
  if (moving) lastMotionMs = now;
}

// ---------------------------------------------------------------------
//  الضغط (FSR)
// ---------------------------------------------------------------------
void updatePressure(uint32_t now) {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogRead(POT_PIN);
  float p = (sum / 8) * 100.0f / 4095.0f;
#if INVERT_SLIDER
  p = 100.0f - p;
#endif

  if (!potInit) { pct = p; ref = p; potInit = true; }
  pct += 0.3f * (p - pct);   // تنعيم

  bool nowLoaded = loaded ? (pct >= MIN_PCT - 1.0f) : (pct >= MIN_PCT);
  if (nowLoaded != loaded || (nowLoaded && fabsf(pct - ref) > DELTA_PCT)) {
    ref = pct;                 // الضغط تغيّر -> صفّر عداد الثبات
    pressureChangedMs = now;
  } else {
    ref += 0.01f * (pct - ref);
  }
  loaded = nowLoaded;

  if (!loaded)              level = 0;
  else if (pct < MED_PCT)   level = 1;   // أخضر
  else if (pct < HIGH_PCT)  level = 2;   // أصفر
  else                      level = 3;   // أحمر
}

// ---------------------------------------------------------------------
//  التنبيه + الليدات
// ---------------------------------------------------------------------
void setPin(int pin, bool on) { if (pin >= 0) digitalWrite(pin, on ? HIGH : LOW); }

uint32_t stillSec(uint32_t now)  { return (now - lastMotionMs) / 1000; }
uint32_t stableSec(uint32_t now) { return loaded ? (now - pressureChangedMs) / 1000 : 0; }

void updateOutputs(uint32_t now) {
  bool blink = (now / 250) % 2;   // وميض وقت التنبيه
  bool show  = !alertActive || blink;
  setPin(LED_GREEN,  show && level == 1);
  setPin(LED_YELLOW, show && level == 2);
  setPin(LED_RED,    show && level == 3);
  setPin(BUZZER_PIN, alertActive && !muted && (now % 1000) < 300);
}

void updateAlert(uint32_t now) {
  bool stillLong  = !mpuOk || stillSec(now) >= STILL_SEC;   // لو الحساس مو شغال نعتمد على الضغط
  bool stableLong = loaded && stableSec(now) >= STABLE_SEC;
  bool cond = stillLong && stableLong;

  if (cond && !alertActive) {
    alertActive = true;
    muted = false;
    alertCount++;
    addEvent(1);
    publishNow = true;
    ntfyPending = strlen(NTFY_TOPIC) > 0;
    Out.println();
    Out.println("##################################################");
    Out.println("   ALERT !  Patient still + constant pressure");
    Out.println("   >>> Reposition the patient now <<<");
    Out.printf ("   Pressure: %.0f%%  ->  %s   | Alerts: %lu\n", pct, LEVEL_NAME[level], (unsigned long)alertCount);
    Out.println("##################################################");
  } else if (!cond && alertActive) {
    alertActive = false;
    muted = false;
    addEvent(2);
    publishNow = true;
    Out.println(">>> Alert cleared: movement / pressure change detected\n");
  }
}

// تم تغيير وضعية المريض (من الموقع)
void doReset() {
  uint32_t now = millis();
  lastMotionMs = now;
  pressureChangedMs = now;
  ref = pct;
  if (alertActive) {          // نقفل التنبيه هنا عشان ما ينحسب "انتهى بالحركة"
    alertActive = false;
    Out.println(">>> Alert cleared: patient repositioned (from website)\n");
  }
  muted = false;
  addEvent(3);
  publishNow = true;
}

// ---------------------------------------------------------------------
//  الاتصال بالموقع (MQTT)
// ---------------------------------------------------------------------
// يبني JSON حسب docs/CONTRACT.md
size_t buildStatusJson(char *buf, size_t size) {
  uint32_t now = millis();
  int n = snprintf(buf, size,
    "{\"v\":1,\"id\":\"%s\",\"name\":\"%s\",\"room\":\"%s\","
    "\"uptime\":%lu,\"still\":%lu,\"stable\":%lu,\"stillReq\":%d,\"stableReq\":%d,"
    "\"moving\":%s,\"motionOk\":%s,\"loaded\":%s,\"alert\":%s,\"muted\":%s,\"alerts\":%lu,"
    "\"zones\":[{\"id\":\"%s\",\"pct\":%.1f,\"lvl\":%u}],\"events\":[",
    DEVICE_ID, PATIENT_NAME, PATIENT_ROOM,
    (unsigned long)(now / 1000), (unsigned long)stillSec(now), (unsigned long)stableSec(now),
    STILL_SEC, STABLE_SEC,
    moving ? "true" : "false", mpuOk ? "true" : "false", loaded ? "true" : "false",
    alertActive ? "true" : "false", muted ? "true" : "false", (unsigned long)alertCount,
    FSR_ZONE, pct, (unsigned)level);
  if (n < 0 || (size_t)n >= size) return 0;

  for (uint32_t k = 0; k < 5 && k < eventSeq && k < EVENT_LOG_SIZE; k++) {   // آخر 5 أحداث، الأحدث أولاً
    Event &e = events[(eventSeq - k) % EVENT_LOG_SIZE];
    int m = snprintf(buf + n, size - n, "%s{\"id\":%lu,\"t\":%lu,\"type\":%u}",
                     k ? "," : "", (unsigned long)e.id, (unsigned long)e.t, (unsigned)e.type);
    if (m < 0 || (size_t)(n + m) >= size) return 0;
    n += m;
  }
  int m = snprintf(buf + n, size - n, "],\"th\":{\"min\":%.0f,\"med\":%.0f,\"high\":%.0f}}",
                   MIN_PCT, MED_PCT, HIGH_PCT);
  if (m < 0 || (size_t)(n + m) >= size) return 0;
  return n + m;
}

void onMqttMessage(char *topic, byte *payload, unsigned int length) {
  char msg[96];
  unsigned int len = length < sizeof(msg) - 1 ? length : sizeof(msg) - 1;
  memcpy(msg, payload, len);
  msg[len] = 0;
  if (strstr(msg, "reset")) {
    Out.println("[MQTT] cmd: reset (repositioned)");
    doReset();
  } else if (strstr(msg, "ack")) {
    Out.println("[MQTT] cmd: ack (mute)");
    muted = true;
    publishNow = true;
  }
}

void publishStatus() {
  static char buf[900];
  size_t len = buildStatusJson(buf, sizeof(buf));
  if (len) mqtt.publish(TOPIC_BASE "/status", buf, true);   // retained
}

void sendNtfy() {
  WiFiClientSecure sc;
  sc.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(3000);
  if (!http.begin(sc, "https://ntfy.sh/" NTFY_TOPIC)) return;
  http.addHeader("Title", "ATHAR Alert");
  http.addHeader("Priority", "urgent");
  http.addHeader("Tags", "rotating_light");
  char body[160];
  snprintf(body, sizeof(body), "%s (Room %s): لم يتحرك والضغط ثابت - غيّر وضعيته الآن",
           PATIENT_NAME, PATIENT_ROOM);
  int code = http.POST((uint8_t *)body, strlen(body));
  http.end();
  Out.printf("[ntfy] sent (%d)\n", code);
}

void setupNetwork() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL);
  lastWifiTry = wifiStartMs = millis();
  Out.printf("[WiFi] connecting to %s ...\n", WIFI_SSID);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(1024);
  mqtt.setSocketTimeout(3);
  mqtt.setKeepAlive(15);
}

// ما يوقف الحساسات: الواي فاي يتصل بالخلفية، و MQTT يحاول كل فترة (2 -> 30 ثانية)
void handleNetwork(uint32_t now) {
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiWasUp) { wifiWasUp = false; mqtt.disconnect(); Out.println("[WiFi] disconnected"); }
    if (now - lastWifiLog >= 5000) {                 // يطمنك إنه يحاول
      lastWifiLog = now;
      Out.printf("[WiFi] connecting to %s ... (%lus)\n", WIFI_SSID, (unsigned long)((now - wifiStartMs) / 1000));
    }
    // نعطيه وقت كافي (60 ثانية) قبل ما نعيد المحاولة، عشان واي فاي Wokwi أحياناً بطيء
    if (now - lastWifiTry > 60000) {
      lastWifiTry = now;
      Out.println("[WiFi] retrying...");
      WiFi.disconnect();
      delay(100);
      WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL);
    }
    return;
  }
  if (!wifiWasUp) {
    wifiWasUp = true;
    Out.printf("[WiFi] connected  IP %s\n", WiFi.localIP().toString().c_str());
    lastMqttTry = now - mqttBackoff;   // جرّب MQTT على طول
  }
  if (ntfyPending) { ntfyPending = false; if (alertActive) sendNtfy(); }   // تنبيه الجوال ما يحتاج MQTT

  if (!mqtt.connected()) {
    if (now - lastMqttTry < mqttBackoff) return;
    lastMqttTry = now;
    char cid[40];
    snprintf(cid, sizeof(cid), "%s-%04lx", DEVICE_ID, (unsigned long)random(0x10000));
    if (mqtt.connect(cid, TOPIC_BASE "/online", 0, true, "0")) {   // LWT: online = "0"
      mqttBackoff = 2000;
      mqtt.publish(TOPIC_BASE "/online", "1", true);
      mqtt.subscribe(TOPIC_BASE "/cmd");
      publishNow = true;
      Out.printf("[MQTT] connected -> %s\n", TOPIC_BASE);
    } else {
      Out.printf("[MQTT] failed (rc=%d), retry in %lus\n", mqtt.state(), (unsigned long)(mqttBackoff / 1000));
      mqttBackoff = mqttBackoff * 2 > 30000 ? 30000 : mqttBackoff * 2;
    }
    return;
  }

  mqtt.loop();
  if (publishNow || now - lastPublish >= 1000) {
    publishNow = false;
    lastPublish = now;
    publishStatus();
  }
}

// شريط يوضح مستوى الضغط  [#####.....]
void printBar(float v) {
  int n = (int)(v / 10.0f + 0.5f);
  Out.print('[');
  for (int i = 0; i < 10; i++) Out.print(i < n ? '#' : '.');
  Out.print(']');
}

// ---------------------------------------------------------------------
void setup() {
  Out.begin(115200);
  delay(500);
  Out.println("\n=== ATHAR - Simulation ===");

  if (LED_GREEN  >= 0) pinMode(LED_GREEN, OUTPUT);
  if (LED_YELLOW >= 0) pinMode(LED_YELLOW, OUTPUT);
  if (LED_RED    >= 0) pinMode(LED_RED, OUTPUT);
  if (BUZZER_PIN >= 0) pinMode(BUZZER_PIN, OUTPUT);
  analogReadResolution(12);

  // يجرب التوصيل العادي، ولو ما لقى الحساس يجرب SDA/SCL معكوسين
  Wire.begin(SDA_PIN, SCL_PIN);
  mpuOk = mpuInit();
  if (!mpuOk) {
    Wire.end();
    Wire.begin(SCL_PIN, SDA_PIN);
    mpuOk = mpuInit();
    if (mpuOk) Out.println("[MPU6050] found with SDA/SCL swapped (SDA=D9, SCL=D8)");
  }
  Out.println(mpuOk ? "[MPU6050] OK" : "[MPU6050] NOT FOUND -> check VCC/GND/SDA/SCL (using pressure only)");
  Out.println("FSR: 0-40% GREEN | 40-75% YELLOW | 75-100% RED");
  Out.printf("Alert after: %d s still + %d s stable pressure\n\n", STILL_SEC, STABLE_SEC);

  Out.println("------------------------------------------------");
  Out.println(" Website: open ATHAR -> Connection -> Cloud");
  Out.printf (" Device ID: %s\n", DEVICE_ID);
  Out.printf (" MQTT: %s  topic %s/status\n", MQTT_HOST, TOPIC_BASE);
  Out.println("------------------------------------------------\n");

  setupNetwork();
  lastMotionMs = pressureChangedMs = millis();
}

void loop() {
  uint32_t now = millis();
  if (now - lastSample >= 100) {          // 10 قراءات بالثانية
    lastSample = now;
    updateMotion(now);
    updatePressure(now);
    updateAlert(now);
    updateOutputs(now);
  }

  handleNetwork(millis());

  now = millis();
  if (now - lastPrint >= 1000) {          // طباعة كل ثانية
    lastPrint = now;
    printBar(pct);
    Out.printf(" %3.0f%% %-15s | Still %2lu/%d s | Stable %2lu/%d s | %s%s\n",
                  pct, LEVEL_NAME[level],
                  (unsigned long)stillSec(now), STILL_SEC,
                  (unsigned long)stableSec(now), STABLE_SEC,
                  !mpuOk ? "MPU ERROR" : (moving ? "MOVING" : "still"),
                  alertActive ? (muted ? "  <== ALERT (muted)" : "  <== ALERT") : "");
  }
}
