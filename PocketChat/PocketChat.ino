#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <U8g2lib.h>

WebServer server(80);
WebSocketsServer ws(81);
DNSServer dnsServer;

const int MAX_CLIENTS = WEBSOCKETS_SERVER_CLIENT_MAX;
String names[MAX_CLIENTS];

// ---------- تنظیمات شبکه ----------
// رمزها توی فایل secrets.h هستن که وارد git نمی‌شه (نمونه: secrets.h.example).
// اگه secrets.h نبود، با مقدارهای پیش‌فرض (وای‌فای باز، بدون اتصال به روتر) کامپایل می‌شه.
#if __has_include("secrets.h")
#include "secrets.h"
#else
#warning "secrets.h not found: using defaults (open AP, no home WiFi). Copy secrets.h.example to secrets.h"
#define SECRET_STA_SSID ""
#define SECRET_STA_PASS ""
#define SECRET_AP_SSID  "PocketChat"
#define SECRET_AP_PASS  ""
#endif

// وای‌فای خونه (STA): برد به روتر وصل می‌شه
const char* STA_SSID = SECRET_STA_SSID;      // اسم وای‌فای خونه (خالی = غیرفعال)
const char* STA_PASS = SECRET_STA_PASS;      // رمز وای‌فای خونه
const unsigned long STA_TIMEOUT = 10000;     // حداکثر مدت انتظار برای اتصال (میلی‌ثانیه)

// شبکه‌ی خود برد (AP)
const char* AP_SSID = SECRET_AP_SSID;
const char* AP_PASS = SECRET_AP_PASS;        // خالی = شبکه‌ی باز. رمز بذاری: حداقل 8 کاراکتر

// ---------- نمایشگر ----------
const int SDA_PIN = 8;
const int SCL_PIN = 9;
const int BTN_PIN = 0;                       // دکمه‌ی BOOT روی برد
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);

const int PAGE_COUNT = 3;                    // وضعیت، آنلاین‌ها، سیستم
const unsigned long PAGE_TIME = 5000;        // هر صفحه 5 ثانیه
int currentPage = 0;
unsigned long pageSince = 0;

int totalMsgs = 0;
int peakUsers = 0;                           // بیشترین تعداد کاربر همزمان
bool displayDirty = true;
unsigned long lastDisplay = 0;

// ---------- اعلان پیام جدید (پاپ‌آپ) ----------
const unsigned long NOTIF_TIME = 3000;       // مدت نمایش اعلان (میلی‌ثانیه)
const int NOTIF_MAX_CHARS = 35;              // حداکثر تعداد حروف پیام در اعلان
String notifFrom = "", notifText = "";
unsigned long notifStart = 0;
unsigned long notifUntil = 0;
unsigned long lastNotifDraw = 0;

// ---------- صفحه‌ی شروع (Splash) و پیشرفت بوت ----------

// لوگوی متحرک: سه کمان وای‌فای یکی‌یکی ظاهر می‌شن
void showSplash() {
  const uint8_t ARC = U8G2_DRAW_UPPER_RIGHT | U8G2_DRAW_UPPER_LEFT;
  for (int step = 0; step <= 3; step++) {
    oled.clearBuffer();
    oled.drawDisc(64, 16, 2);
    if (step >= 1) oled.drawCircle(64, 16, 6, ARC);
    if (step >= 2) oled.drawCircle(64, 16, 10, ARC);
    if (step >= 3) oled.drawCircle(64, 16, 14, ARC);

    oled.setFont(u8g2_font_helvB14_tr);
    const char* t = "PocketChat";
    oled.drawStr((128 - oled.getStrWidth(t)) / 2, 40, t);

    oled.setFont(u8g2_font_6x10_tr);
    const char* s = "Offline group chat";
    oled.drawStr((128 - oled.getStrWidth(s)) / 2, 56, s);

    oled.sendBuffer();
    delay(350);
  }
  delay(600);
}

// صفحه‌ی بوت: اسم برنامه + پیام مرحله + نوار پیشرفت (0 تا 100)
void bootScreen(const char* msg, int percent) {
  oled.clearBuffer();
  oled.setFont(u8g2_font_helvB14_tr);
  const char* t = "PocketChat";
  oled.drawStr((128 - oled.getStrWidth(t)) / 2, 24, t);

  oled.setFont(u8g2_font_6x10_tr);
  oled.drawStr((128 - oled.getStrWidth(msg)) / 2, 42, msg);

  oled.drawFrame(10, 50, 108, 10);
  oled.drawBox(12, 52, (104 * percent) / 100, 6);
  oled.sendBuffer();
}

// ---------- نمایشگر: ابزارهای کمکی ----------

// نمایشگر فقط حروف انگلیسی داره. حروف غیرانگلیسی (مثل فارسی) رو با ? جایگزین می‌کنیم
String asciiOnly(const String& s) {
  String r;
  for (size_t i = 0; i < s.length(); i++) {
    unsigned char c = s[i];
    if (c >= 32 && c < 127) r += (char)c;
    else if (c >= 0xC0) r += '?';            // شروع یک حرف چندبایتی
  }
  return r;
}

// متن رو در چند خط 21 کاراکتری می‌نویسه
void drawWrapped(const String& s, int y, int lines) {
  for (int i = 0; i < lines; i++) {
    String part = s.substring(i * 21, (i + 1) * 21);
    if (part.length() == 0) break;
    oled.setCursor(0, y + i * 12);
    oled.print(part);
  }
}

// ---------- اعلان پیام جدید ----------

bool notifOn() {
  return (long)(notifUntil - millis()) > 0;
}

// وقتی پیام جدید میاد صدا زده می‌شه: اعلان رو آماده و فعال می‌کنه
void noteMessage(const String& from, const String& text) {
  notifFrom = asciiOnly(from).substring(0, 18);
  String t = asciiOnly(text);
  if ((int)t.length() > NOTIF_MAX_CHARS)     // بلندتر از 35 حرف؟ ببر و ... بذار
    t = t.substring(0, NOTIF_MAX_CHARS) + "...";
  notifText = t;
  notifStart = millis();
  notifUntil = notifStart + NOTIF_TIME;
  lastNotifDraw = 0;                         // اولین فریم فوراً کشیده بشه
}

void drawNotification() {
  unsigned long elapsed = millis() - notifStart;
  oled.clearBuffer();

  // نوار سفید بالا: پاکت نامه + عنوان (متن سیاه روی سفید)
  oled.setDrawColor(1);
  oled.drawBox(0, 0, 128, 14);
  oled.setDrawColor(0);
  int bounce = (elapsed / 250) % 2;          // پاکت هر 250 میلی‌ثانیه یک پیکسل بالا/پایین می‌ره
  oled.drawFrame(4, 2 + bounce, 12, 9);
  oled.drawLine(4, 2 + bounce, 10, 7 + bounce);
  oled.drawLine(15, 2 + bounce, 10, 7 + bounce);
  oled.setFont(u8g2_font_6x10_tr);
  oled.drawStr(22, 11, "NEW MESSAGE");
  oled.setDrawColor(1);

  // اسم فرستنده (فونت بولد)
  oled.setFont(u8g2_font_7x13B_tr);
  oled.drawStr(0, 29, notifFrom.c_str());

  // متن پیام (حداکثر 35 حرف + ...)
  oled.setFont(u8g2_font_6x10_tr);
  drawWrapped(notifText, 43, 2);

  // نوار پایین: زمان باقی‌مانده‌ی اعلان
  int w = 128 - (int)((128UL * elapsed) / NOTIF_TIME);
  if (w < 0) w = 0;
  oled.drawBox(0, 61, w, 3);

  oled.sendBuffer();
}

// ---------- نمایشگر: صفحه‌های عادی ----------

void updateDisplay() {
  int users = 0;
  for (int i = 0; i < MAX_CLIENTS; i++)
    if (names[i].length() > 0) users++;
  if (users > peakUsers) peakUsers = users;

  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tr);

  // سرتیتر
  const char* titles[PAGE_COUNT] = {"PocketChat", "Online", "System"};
  oled.setCursor(0, 10);   oled.print(titles[currentPage]);
  oled.setCursor(104, 10); oled.print(currentPage + 1); oled.print("/"); oled.print(PAGE_COUNT);
  oled.drawHLine(0, 13, 128);

  if (currentPage == 0) {                    // وضعیت
    oled.setCursor(0, 26); oled.print("WiFi: "); oled.print(AP_SSID);
    oled.setCursor(0, 38); oled.print("IP: ");   oled.print(WiFi.softAPIP());
    oled.setCursor(0, 50); oled.print("Users: "); oled.print(users);
    oled.setCursor(0, 62); oled.print("Msgs: ");  oled.print(totalMsgs);

  } else if (currentPage == 1) {             // آنلاین‌ها
    if (users == 0) {
      oled.setCursor(0, 26); oled.print("nobody online");
    } else {
      int shown = 0;
      for (int i = 0; i < MAX_CLIENTS; i++) {
        if (names[i].length() == 0) continue;
        if (shown < 4) {
          oled.setCursor(0, 26 + shown * 12);
          oled.print(asciiOnly(names[i]).substring(0, 21));
        }
        shown++;
      }
      if (shown > 4) {                       // جای چهارمی رو به «+N more» بده
        oled.setDrawColor(0);
        oled.drawBox(0, 52, 128, 12);
        oled.setDrawColor(1);
        oled.setCursor(0, 62);
        oled.print("+"); oled.print(shown - 3); oled.print(" more");
      }
    }

  } else {                                   // سیستم
    unsigned long s = millis() / 1000;
    oled.setCursor(0, 26); oled.printf("Up: %02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
    oled.setCursor(0, 38); oled.printf("Free RAM: %u KB", ESP.getFreeHeap() / 1024);
    oled.setCursor(0, 50); oled.printf("Peak users: %d", peakUsers);
    oled.setCursor(0, 62); oled.printf("WiFi devices: %d", WiFi.softAPgetStationNum());
  }

  oled.sendBuffer();
}

// ---------- تاریخچه (ring buffer) ----------
const int HIST_MAX = 30;
String hist[HIST_MAX];     // هر خونه یک پیام به‌صورت JSON
int histHead = 0;          // خونه‌ای که پیام بعدی توش نوشته می‌شه
int histCount = 0;         // چند پیام ذخیره شده

void addHistory(const String& msgJson) {
  hist[histHead] = msgJson;
  histHead = (histHead + 1) % HIST_MAX;
  if (histCount < HIST_MAX) histCount++;
}

void sendHistory(uint8_t num) {
  int start = (histHead - histCount + HIST_MAX) % HIST_MAX;   // قدیمی‌ترین
  for (int i = 0; i < histCount; i++) {
    ws.sendTXT(num, hist[(start + i) % HIST_MAX]);
  }
}

const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width, initial-scale=1'>
<title>PocketChat</title>
<style>
  body{font-family:sans-serif;margin:0;padding:10px}
  #log{height:60vh;overflow-y:auto;border:1px solid #ccc;padding:8px;margin:8px 0}
  #log div{padding:3px 0;border-bottom:1px solid #eee}
  .sys{color:#888;font-style:italic}
  .me b{color:#0a7}
  input{width:65%;padding:8px;font-size:16px}
  button{padding:8px 14px;font-size:16px}
  #users{color:#555;font-size:14px}
  #err{color:#c00}
</style></head><body>

<div id='login'>
  <h3>PocketChat</h3>
  <input id='nm' placeholder='esm-e shoma' maxlength='20' autocomplete='off'>
  <button onclick='join()'>Join</button>
  <p id='err'></p>
</div>

<div id='chat' style='display:none'>
  <h3>PocketChat <small id='st'></small></h3>
  <div id='users'></div>
  <div id='log'></div>
  <input id='inp' placeholder='payam...' autocomplete='off'>
  <button onclick='send()'>Send</button>
</div>

<script>
const $ = id => document.getElementById(id);
let me = '';          // خالی = هنوز وارد نشده؛ پر = وارد شده (برای ورود مجدد)
let ws;

function sendJoin(name) {
  if (ws.readyState === 1) ws.send(JSON.stringify({type:'join', name:name}));
}
function join() {
  const name = $('nm').value.trim();
  if (name) sendJoin(name);
}
function send() {
  const t = $('inp').value;
  if (t && ws.readyState === 1) {
    ws.send(JSON.stringify({type:'msg', text:t}));
    $('inp').value = '';
  }
}
function addLine(text, cls, from) {
  const d = document.createElement('div');
  d.dir = 'auto';
  if (cls) d.className = cls;
  if (from) {
    const b = document.createElement('b');
    b.textContent = from + ': ';
    d.appendChild(b);
  }
  d.appendChild(document.createTextNode(text));
  $('log').appendChild(d);
  $('log').scrollTop = $('log').scrollHeight;
}

function connect() {
  ws = new WebSocket('ws://' + location.hostname + ':81/');

  ws.onopen = () => {
    $('st').textContent = '(connected)';
    if (me) sendJoin(me);              // اتصال مجدد: با اسم قبلی وارد شو
  };
  ws.onclose = () => {
    $('st').textContent = '(reconnecting...)';
    setTimeout(connect, 2000);         // ۲ ثانیه بعد دوباره تلاش کن
  };
  ws.onmessage = (e) => {
    const m = JSON.parse(e.data);
    if (m.type === 'joined') {
      me = m.name;
      $('log').innerHTML = '';         // تاریخچه دوباره می‌رسه، تکراری نشه
      $('login').style.display = 'none';
      $('chat').style.display = 'block';
      $('st').textContent = '(connected)';
    } else if (m.type === 'error') {
      if (me) setTimeout(() => sendJoin(me), 3000);   // حین اتصال مجدد: دوباره امتحان کن
      else $('err').textContent = m.text;
    } else if (m.type === 'msg') {
      addLine(m.text, m.from === me ? 'me' : '', m.from);
    } else if (m.type === 'sys') {
      addLine(m.text, 'sys');
    } else if (m.type === 'users') {
      $('users').textContent = 'Online (' + m.list.length + '): ' + m.list.join(', ');
    }
  };
}
connect();

$('nm').addEventListener('keydown', e => { if (e.key === 'Enter') join(); });
$('inp').addEventListener('keydown', e => { if (e.key === 'Enter') send(); });
</script></body></html>
)rawliteral";

void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", PAGE);
}

// هر آدرسی که نمی‌شناسیم → ببر به صفحه‌ی چت
void handleNotFound() {
  if (server.client().localIP() != WiFi.softAPIP()) {   // درخواست از شبکه‌ی خونه یا اینترنت: redirect نکن
    server.send(404, "text/plain", "Not found");
    return;
  }
  server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
  server.sendHeader("Cache-Control", "no-cache");
  server.send(302, "text/plain", "");
}

// ---------- توابع کمکی ----------

bool nameTaken(const String& n) {
  for (int i = 0; i < MAX_CLIENTS; i++)
    if (names[i].length() > 0 && names[i].equalsIgnoreCase(n)) return true;
  return false;
}

void sendSys(const String& text) {
  JsonDocument doc;
  doc["type"] = "sys";
  doc["text"] = text;
  String out;
  serializeJson(doc, out);
  ws.broadcastTXT(out);
}

void sendUsers() {
  JsonDocument doc;
  doc["type"] = "users";
  JsonArray arr = doc["list"].to<JsonArray>();
  for (int i = 0; i < MAX_CLIENTS; i++)
    if (names[i].length() > 0) arr.add(names[i]);
  String out;
  serializeJson(doc, out);
  ws.broadcastTXT(out);
}

void sendError(uint8_t num, const char* text) {
  JsonDocument doc;
  doc["type"] = "error";
  doc["text"] = text;
  String out;
  serializeJson(doc, out);
  ws.sendTXT(num, out);
}

// ---------- پردازش پیام‌ها ----------

void handleMessage(uint8_t num, uint8_t* payload, size_t length) {
  JsonDocument doc;
  if (deserializeJson(doc, (const char*)payload, length)) return;

  const char* type = doc["type"] | "";

  if (strcmp(type, "join") == 0) {
    String name = doc["name"] | "";
    name.trim();
    if (names[num].length() > 0) return;
    if (name.length() == 0 || name.length() > 24) { sendError(num, "Esm bayad 1 ta 24 harf bashe"); return; }
    if (nameTaken(name)) { sendError(num, "In esm gerefte shode"); return; }

    names[num] = name;
    JsonDocument ok;
    ok["type"] = "joined";
    ok["name"] = name;
    String out;
    serializeJson(ok, out);
    ws.sendTXT(num, out);

    sendHistory(num);              // پیام‌های قبلی رو فقط برای همین نفر بفرست
    sendSys(name + " joined");
    sendUsers();
    displayDirty = true;

  } else if (strcmp(type, "msg") == 0) {
    if (names[num].length() == 0) return;
    String text = doc["text"] | "";
    if (text.length() == 0 || text.length() > 300) return;

    JsonDocument m;
    m["type"] = "msg";
    m["from"] = names[num];
    m["text"] = text;
    String out;
    serializeJson(m, out);
    addHistory(out);               // در تاریخچه ذخیره کن
    ws.broadcastTXT(out);
    totalMsgs++;
    noteMessage(names[num], text); // اعلان پیام جدید روی نمایشگر
    displayDirty = true;
  }
}

void onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.printf("Client %u connected\n", num);
      break;
    case WStype_DISCONNECTED:
      Serial.printf("Client %u disconnected\n", num);
      if (names[num].length() > 0) {
        String old = names[num];
        names[num] = "";
        sendSys(old + " left");
        sendUsers();
        displayDirty = true;
      }
      break;
    case WStype_TEXT:
      handleMessage(num, payload, length);
      break;
    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);

  // نمایشگر و دکمه
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);        // I2C سریع‌تر (400kHz)
  oled.begin();
  pinMode(BTN_PIN, INPUT_PULLUP);

  showSplash();                 // لوگوی شروع (حدود 2 ثانیه؛ زمان کافی برای باز شدن Serial Monitor)

  // مرحله 1: وای‌فای
  bootScreen("Starting WiFi...", 10);
  int apChannel = 1;
  WiFi.mode(WIFI_AP_STA);                    // هم Access Point، هم متصل به روتر
  if (strlen(STA_SSID) > 0) {
    bootScreen("Joining home WiFi...", 15);
    WiFi.begin(STA_SSID, STA_PASS);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < STA_TIMEOUT) delay(200);
    if (WiFi.status() == WL_CONNECTED) {
      apChannel = WiFi.channel();            // AP باید روی همون کانال روتر باشه (یک آنتن بیشتر نداریم)
      Serial.print("Home WiFi IP: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println("Home WiFi: not connected (continuing with AP only)");
    }
  }
  bootScreen("Starting WiFi...", 25);
  WiFi.softAP(AP_SSID, strlen(AP_PASS) ? AP_PASS : NULL, apChannel, 0, 8);
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());
  delay(300);

  // مرحله 2: DNS (Captive Portal)
  bootScreen("Starting DNS...", 35);
  dnsServer.start(53, "*", WiFi.softAPIP());         // هر اسمی → آدرس خود برد
  delay(300);

  // مرحله 3: mDNS
  bootScreen("Starting mDNS...", 55);
  if (MDNS.begin("pocket")) {                        // آدرس pocket.local
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS: http://pocket.local");
  }
  delay(300);

  // مرحله 4: وب‌سرور
  bootScreen("Starting server...", 75);
  server.on("/", handleRoot);
  server.onNotFound(handleNotFound);                 // آدرس‌های ناشناخته → redirect
  server.begin();
  delay(300);

  // مرحله 5: چت (WebSocket)
  bootScreen("Starting chat...", 90);
  ws.begin();
  ws.onEvent(onWsEvent);
  ws.enableHeartbeat(10000, 3000, 2);   // هر 10 ثانیه ping؛ 3 ثانیه مهلت pong؛ بعد از 2 بار نیومدن، قطع
  delay(300);

  bootScreen("Ready!", 100);
  Serial.printf("Started. Max clients: %d\n", MAX_CLIENTS);
  delay(800);

  pageSince = millis();
  updateDisplay();              // صفحه‌ی اول
}

unsigned long lastPrint = 0;

void loop() {
  dnsServer.processNextRequest();    // به سؤال‌های DNS جواب بده
  server.handleClient();
  ws.loop();

  // --- دکمه‌ی BOOT: اگه اعلان بازه می‌بندتش، وگرنه صفحه‌ی بعدی ---
  static bool lastBtn = HIGH;
  static unsigned long lastPress = 0;
  bool btn = digitalRead(BTN_PIN);
  if (lastBtn == HIGH && btn == LOW && millis() - lastPress > 200) {   // لحظه‌ی فشرده شدن
    lastPress = millis();
    if (notifOn()) notifUntil = 0;                         // بستن اعلان
    else currentPage = (currentPage + 1) % PAGE_COUNT;     // صفحه‌ی بعدی
    pageSince = millis();
    updateDisplay();                                       // فوراً نشون بده
    lastDisplay = millis();
    displayDirty = false;
  }
  lastBtn = btn;

  static bool wasNotif = false;

  if (notifOn()) {
    // --- اعلان فعاله: هر 100 میلی‌ثانیه دوباره بکش (برای انیمیشن و نوار زمان) ---
    wasNotif = true;
    pageSince = millis();                                  // چرخش صفحه‌ها تا پایان اعلان صبر می‌کنه
    if (millis() - lastNotifDraw > 100) {
      lastNotifDraw = millis();
      drawNotification();
    }

  } else {
    // اعلان تموم شد: صفحه‌ی عادی رو برگردون
    if (wasNotif) {
      wasNotif = false;
      displayDirty = true;
      lastDisplay = 0;
    }

    // --- چرخش خودکار صفحه‌ها ---
    if (millis() - pageSince > PAGE_TIME) {
      currentPage = (currentPage + 1) % PAGE_COUNT;
      pageSince = millis();
      displayDirty = true;
    }

    // --- تازه‌کردن نمایشگر: حداکثر ثانیه‌ای یک بار ---
    // (صفحه‌ی سیستم، یعنی شماره 2، همیشه تازه می‌شه چون زمان کار داره می‌گذره)
    if ((displayDirty || currentPage == 2) && millis() - lastDisplay > 1000) {
      lastDisplay = millis();
      displayDirty = false;
      updateDisplay();
    }
  }

  if (millis() - lastPrint > 10000) {   // هر 10 ثانیه وضعیت رو چاپ کن
    lastPrint = millis();
    Serial.printf("WiFi clients: %d | free heap: %u | history: %d\n",
                  WiFi.softAPgetStationNum(), ESP.getFreeHeap(), histCount);
  }
}
