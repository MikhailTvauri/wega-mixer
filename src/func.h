#include <style.h>

#define SSE_MAX_CHANNELS 8
WiFiClient subscription[SSE_MAX_CHANNELS];
unsigned long lastSentTime = 0;

// server sent events
bool checkConnected() {
  bool result = false;
  for (uint8_t i = 0; i < SSE_MAX_CHANNELS; i++) {
    if (!subscription[i] || subscription[i].status() == CLOSED) {
      continue;
    } else if (subscription[i].connected()) {
      result = true;
    } else {
      subscription[i].flush();
      subscription[i].stop();
    }
  }
  return result;
}

void sendEvent(const __FlashStringHelper* name, int bufSize, void (*appendPayload)(String&)) {
  if (!checkConnected()) return;
  lastSentTime = millis();
  String message((char*)0);
  message.reserve(bufSize);
  message += F("event:");
  message += name;
  message += F("\ndata:{");
  appendPayload(message);
  message += F("}\n\n");
  
  for (byte i = 0; i < SSE_MAX_CHANNELS; i++) {
    if (subscription[i] && subscription[i].connected()) subscription[i].print(message); 
  }
}

// функции для генерации json
template<typename T>
void appendJson(String& src, const __FlashStringHelper* name, const T& value, const bool quote, const bool last) {
  src += '"'; src += name; src += F("\":");
  if (quote) src += '"';
  append(src, value);
  if (quote) src += '"';
  if (!last) src += ',';  
}

void sendState() {
  sendEvent(F("state"), 256, [] (String& message) {
    appendJson(message, F("state"), stateStr[state],  true, true);  
  }); 
}

void setState(State s){
  state = s;
  sendState();
}

template<typename T>
void appendJsonArr(String& src, const __FlashStringHelper* name, const T value[], const int len, const bool quote, const bool last) {
  src += '"'; src += name; src += F("\":");
  src += '[';
  for (int i = 0; i < len; i++) {
    if (quote) src += '"';
    append(src, value[i]);
    if (quote) src += '"';
    if (i < (len - 1)) src += ',';
  }
  src += ']';
  if (!last) src += ',';  
}

void sendReportUpdate() {
  sendEvent(F("report"), 512, [] (String& message) {
    unsigned long ms = sTime == 0 ? 0 : (eTime == 0 ? millis() - sTime : eTime - sTime);  
    appendJson(message,    F("timer"),  ms / 1000,         false, false);
    appendJson(message,    F("sumA"),   sumA,              false, false);
    appendJson(message,    F("sumB"),   sumB,              false, false);
    appendJson(message,    F("pumpWorking"), pumpWorking,  false, false); 
    appendJsonArr(message, F("goal"),   goal,   PUMPS_NO,  false, false);
    appendJsonArr(message, F("result"), curvol, PUMPS_NO,  false, true);
  });  
}

void handleSubscribe() {
  for (int i = 0; i < SSE_MAX_CHANNELS; i++) {
    if (!subscription[i] || subscription[i].status() == CLOSED) {
      server.client().setNoDelay(true);
      server.client().setSync(true);
      subscription[i] = server.client();
      
      server.setContentLength(CONTENT_LENGTH_UNKNOWN);
      server.sendContent_P(PSTR("HTTP/1.1 200 OK\n"
        "Content-Type: text/event-stream;\n" 
        "Connection: keep-alive\n" 
        "Cache-Control: no-cache\n" 
        "Access-Control-Allow-Origin: *\n\n"));

      sendReportUpdate();
      sendState();
      return;
    }
  }
  return busyPage();
}

float rawToUnits(float raw) {
  return (raw - scale.get_offset()) / scale.get_scale();
}

void sendScalesValue() {
  sendEvent(F("scales"), 256, [] (String& message) {
    appendJson(message, F("value"),    rawToUnits(displayFilter.getEstimation()), false, false);  
    appendJson(message, F("rawValue"), displayFilter.getEstimation(),             false, false);
    appendJson(message, F("rawZero"),  scale.get_offset(),                        false, true);
  }); 
}


void handleMeta() {
  String message((char*)0);
  message.reserve(255);
  message += '{';
  appendJson(message,    F("version"),     FPSTR(FW_version),         true,  false);
  appendJson(message,    F("scalePointA"), scale_calibration_A,       false, false);
  appendJson(message,    F("scalePointB"), scale_calibration_B,       false, false);  
  appendJsonArr(message, F("names"),       names, PUMPS_NO,           true,  true);  
  message += '}';
  server.send(200, "application/json", message);
}

// Функции для работы с весами
float readScales(int times) {
  float sum = 0;
  for (int i = 0; i < times; i++) {
    float value = scale.read();
    sum += value;
    displayFilter.updateEstimation(value);
    filter.updateEstimation(value);
  }
  return sum / times;
}

void ping() {
  if (lastSentTime + 10000 > millis()) return;
  lastSentTime = millis();
  if (!checkConnected()) return;
  
  for (byte i = 0; i < SSE_MAX_CHANNELS; i++) {
    if (subscription[i] && subscription[i].connected()) { 
      subscription[i].print(F(": keepAlive\n\n"));
    }   
  }
}

float readScalesWithCheck(int times) {
  float value1 = readScales(times / 2);
  while (true) {
    server.handleClient();
    ping();
    delay(20);
    float value2 = readScales(times / 2);
    if (fabs(value1 - value2) < fabs(0.01 * scale_calibration_A)) {
      return (value1 + value2) / 2;
    }
    value1 = value2;
  }
}

void handleMeasure() {
  if (state != STATE_READY) return busyPage(); 
  
  setState(STATE_BUSY);    
  float rawValue = readScalesWithCheck(128);
  String message((char*)0);
  message.reserve(255);
  message += '{';
  appendJson(message, F("value"),    rawToUnits(rawValue),  false, false);
  appendJson(message, F("rawValue"), rawValue,              false, false);
  appendJson(message, F("rawZero"),  scale.get_offset(),    false, true);
  message += '}';
  server.send(200, "application/json", message);
  setState(STATE_READY);
} 

// |          Ready |
void printStatus(const char* status) {
  lcd.setCursor(0, 0);
  lcd.printf("%15s ", status); 
}

// | Skip           |
void printProgress(const __FlashStringHelper* progress) {
  lcd.setCursor(0, 1);
  lcd.print(progress);
}

void tareScalesWithCheck(int times) {
  scale.set_offset(readScalesWithCheck(times));
}

void handleTare(){
  if (state != STATE_READY) return busyPage(); 

  setState(STATE_BUSY);
  printStatus(stateStr[state]);
  printProgress(F("Taring"));
  tareScalesWithCheck(255);
  setState(STATE_READY);
  okPage();
}

// Функции вывода на экран
// |1:Ca 34.45 Drops|
void printStage(byte n, const __FlashStringHelper* stage) {
  lcd.clear(); 
  lcd.printf_P(PSTR("%4s %.2f %S     "), names[n], goal[n], stage); 
}

// Функции помп
void pumpStart(int n) {
  #if (RUSTY_MOTOR_KICK)
  mcp.digitalWrite(pinForward[n], LOW); 
  mcp.digitalWrite(pinReverse[n], HIGH);
  delay(4);
  #endif
  mcp.digitalWrite(pinForward[n], HIGH); 
  mcp.digitalWrite(pinReverse[n], LOW);
}

void pumpStop(int n) {
  mcp.digitalWrite(pinForward[n], LOW); 
  mcp
  .digitalWrite(pinReverse[n], LOW);
}
void pumpReverse(int n) {
  mcp.digitalWrite(pinForward[n], LOW); 
  mcp.digitalWrite(pinReverse[n], HIGH);
}

void handleTest(){
  if (state != STATE_READY) return busyPage(); 

  setState(STATE_BUSY);
  okPage();
  for (byte i = 0; i < PUMPS_NO; i++) {
    printStage(i, F("Start")); pumpStart(i); delay(3000);
    printStage(i, F("Revers")); pumpReverse(i); delay(3000);
    printStage(i, F("Stop"));  pumpStop(i); delay(1000);
  }
  setState(STATE_READY);
}


void wait(unsigned long ms, int step) {
  unsigned long endPreloadTime = millis() + ms; 
  while (millis() < endPreloadTime) {
    server.handleClient();
    ping();
    delay(step);
  }
}

// |Preload=12000ms |
void printPreload(int ms) {
  lcd.setCursor(0, 1);
  lcd.printf_P(PSTR("Preload=%dms    "), ms);
}

long pumpToValue(byte n, float capValue, float capMillis, float allowedOscillation, void (*printFunc)(float)) {
  float value = rawToUnits(filter.getEstimation());
  if (value >= capValue) return 0;
  
  unsigned long startMillis = millis();
  unsigned long stopMillis = startMillis + capMillis;
  float maxValue = value;
  pumpStart(n);
  char exitCode;
  for (long i = 0; true; i++) { 
    if (value >= capValue) {
      exitCode = 'V'; // вес достиг заданный
      break;
    } else if (millis() >= stopMillis) {
      exitCode = 'T'; // истекло время
      break;
    } else if (value < maxValue - allowedOscillation) {
      exitCode = 'E'; // аномальные показания весов
      break;
    }
    readScales(1);
    value = rawToUnits(filter.getEstimation());
    maxValue = max(value, maxValue);
    if (i % 16 == 0) {
      printFunc(value);
      yield();
      server.handleClient();
      ping();
    }
  }
  pumpStop(n);
  printFunc(value);
  lcd.setCursor(15, 1);lcd.print(exitCode);
  return millis() - startMillis;
}

float truncNegativeZero (float x, float treshold) {
  return x < 0 && x > treshold ? 0 : x;
}

// | 3.14           |
void printProgressValueOnly(float value) {
  lcd.setCursor(0, 1);
  lcd.printf_P(PSTR("%5.2f           "), truncNegativeZero(value, -0.01)); 
}

// | 3.14 80%       |
void printProgress(float value) {
  lcd.setCursor(0, 1);
  lcd.printf_P(PSTR("%5.2f %.0f%%      "), value, value / goal[pumpWorking] * 100); 
}

// | 3.14 80% 80ms  |
void printProgress(float value, int ms) {
  lcd.setCursor(0, 1);
  lcd.printf_P(PSTR("%5.2f %.0f%% %2dms      "), value, value / goal[pumpWorking] * 100, ms); 
}

// | 3.14 100.02%   |
void printResult(float value) {
  lcd.setCursor(0, 1);
  lcd.printf_P(PSTR("%5.2f %.2f%%      "), value, value / goal[pumpWorking] * 100);
}

// Функция налива
float pumping(int n) {
  pumpWorking = n;
  server.handleClient();
  sendReportUpdate();

  if (goal[n] <= 0) {
    printStage(n, F("Skip"));
    printProgress(F("SKIP"));
    wait(1000, 10);
    return 0;
  }

  printStage(n, F("Tare"));
  tareScalesWithCheck(255);
  server.handleClient();

  printStage(n, F("Load"));
  long preload = 0;
  if (goal[n] < 0.5) { // статический прелоад
    preload = staticPreload[n];

    printProgress(F("Reverse"));
    pumpReverse(n); wait(preload, 10); pumpStop(n);
    printPreload(preload);
    pumpStart(n); wait(preload, 10); pumpStop(n);

    curvol[n] = rawToUnits(readScalesWithCheck(128));
    server.handleClient();
    sendReportUpdate();
  } else { // прелоад до первой капли
    while (curvol[n] < 0.02) {
      preload += pumpToValue(n, 0.03, staticPreload[n] * 2, 0.1, printProgressValueOnly);
      curvol[n] = rawToUnits(readScalesWithCheck(128));
      server.handleClient();
      sendReportUpdate();
    }
    printPreload(preload);
    wait(2000, 10);
  }
  
  // быстрая фаза до конечного веса минус 0.2 - 0.5 грамм по половине от остатка 
  printStage(n, F("Fast"));
  float performance = 0.0007;                                 // производительность грамм/мс при 12v и трубке 2x4, в процессе уточняется
  float dropTreshold = goal[n] - curvol[n] > 1.0 ? 0.2 : 0.5; // определяет сколько оставить на капельный налив. Чем больше итераций тем точнее и можно лить почти до конца
  float valueToPump = goal[n] - curvol[n] - dropTreshold;
  float allowedOscillation = valueToPump < 1.0 ? 1.5 : 3.0;  // допустимое раскачивание, чтобы остановиться при помехах/раскачивании, важно на малых объемах  
  if (valueToPump > 0.3) { // если быстро качать не много то не начинать даже
    while ((valueToPump = goal[n] - curvol[n] - dropTreshold) > 0) {
      if (valueToPump > 0.2) valueToPump = valueToPump / 2;  // качать по половине от остатка
      long timeToPump = valueToPump / performance;           // ограничение по времени
      long workedTime = pumpToValue(n, curvol[n] + valueToPump, timeToPump, allowedOscillation, printProgress);
      float prevValue = curvol[n];
      curvol[n] = rawToUnits(readScalesWithCheck(128));
      if (workedTime > 200 && curvol[n] - prevValue > 0.15) performance = max(performance, (curvol[n] - prevValue) / workedTime);
      server.handleClient();
      sendReportUpdate();
    }
  }
  
  // капельный налив
  printStage(n, F("Drop"));
  int sk = 25;
  while (curvol[n] < goal[n] - 0.01) {
    printProgress(curvol[n], sk);
    
    pumpStart(n); delay(sk); pumpStop(n);
      
    float prevValue = curvol[n];
    curvol[n] = rawToUnits(readScalesWithCheck(128));
    if (curvol[n] - prevValue < 0.01) {sk = min(80, sk + 2);}
    if (curvol[n] - prevValue > 0.01) {sk = max(2, sk - 2);}
    if (curvol[n] - prevValue > 0.1 ) {sk = 0;}
    
    server.handleClient();
    sendReportUpdate();
  }

  // реверс, высушить трубки
  printStage(n, F("Dry"));
  printResult(curvol[n]);
  pumpReverse(n);
  wait(max(preload, staticPreload[n]) * 1.5, 10); 
  pumpStop(n);
  return 0;
}

template<typename T>
void append(String& ret, const T& value) {
  ret += value;  
}
template <>
void append<float>(String& ret, const float& value) {
  char buff[20];
  dtostrf(value, 5, 3, buff);
  ret += buff;
}

void reportToWega(int systemId) {
  String httpstr((char*)0);
  httpstr.reserve(512);
  httpstr += F(WegaApiUrl);
  httpstr += F("?s=");
  httpstr += systemId; 
  for(byte i = 0; i < PUMPS_NO; i++) {
    httpstr += F("&p");
    httpstr += (i + 1);
    httpstr += '=';
    append(httpstr, curvol[i]);
    httpstr += F("&v");
    httpstr += (i + 1);
    httpstr += '=';
    append(httpstr, goal[i]);
  }
  WiFiClient client;
  HTTPClient http;
  http.begin(client, httpstr);
  http.GET();
  http.end();
}

void handleStart() {
  if (state != STATE_READY) return busyPage(); 
  
  setState(STATE_WORKING);  
  sTime = millis();
  eTime = 0;
  sumA = 0;
  sumB = 0;
  int systemId = server.arg("s").toInt();

  for (byte i = 0; i < PUMPS_NO; i ++) {
    goal[i] = server.arg(String(F("p")) + (i + 1)).toFloat();
    curvol[i] = 0;
  }
 
  okPage();

  float offsetBeforePump = scale.get_offset();
  scale.set_scale(scale_calibration_A);
  float raw1 = readScalesWithCheck(255);
  pumping(0);
  pumping(1);
  pumping(2);
  float raw2 = readScalesWithCheck(255);   
  sumA = (raw2 - raw1) / scale_calibration_A;
  
  scale.set_scale(scale_calibration_B); 
  pumping(3);
  pumping(4);
  pumping(5);
  pumping(6);
  pumping(7); 
  pumpWorking = -1;
  
  float raw3 = readScalesWithCheck(255); 
  sumB = (raw3 - raw2) / scale_calibration_B;

  reportToWega(systemId);

  scale.set_scale(scale_calibration_A);
  scale.set_offset(offsetBeforePump);

  eTime = millis();
  sendReportUpdate();
  setState(STATE_READY);
  lcd.clear();
}

