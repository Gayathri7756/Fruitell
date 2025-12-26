/*
  FRUITELL Pen — Nano-fit Minimal + Train Flag + CSV Accumulate Mode (EEPROM)

  What’s new vs last version:
    • CSV accumulate mode persisted in EEPROM:
        - CSVACCUM:ON   -> each CSVTEST adds to running totals (across sessions & power cycles)
        - CSVACCUM:OFF  -> each CSVTEST replaces (session-only; previous totals cleared on new session)
        - CSVACCUM:CLEAR-> clears running totals (keeps anchors), TRAINED stays as-is
    • R command prints totals so you can see how many rows you’ve trained on in total.
    • CSVTEST:BEGIN/END still sets TRAINED=1 and (optionally) updates anchors from last seen values.

  Kept tiny to fit ATmega328P:
    - Integer prints only (no float formatting, no <math.h>)
    - Median + MAD only
    - Linear Fresh% from anchors; Conf% from MAD
    - Same UX: quiet-until-trained, RUN/STOP via touch, TRAIN:ON CSV, SNAP, F/S anchors, MODEL:RESET

  Serial commands:
    R                     -> print anchors, TRAINED, ACCUM_MODE, totals
    F / S                 -> anchor fresh / spoil from live median
    TRAIN:ON / TRAIN:OFF  -> live CSV stream
    SNAP                  -> one CSV line (+human if trained)
    MODEL:RESET           -> TRAINED=0, totals cleared
    CSVTEST:BEGIN / END   -> paste echo_us,label,fresh_anchor,spoil_anchor; END sets TRAINED=1
    CSVACCUM:ON/OFF       -> toggle accumulate mode (persisted)
    CSVACCUM:CLEAR        -> clear running totals (persisted), keep anchors
    TFLAG? / TFLAG:0/1    -> get/set train flag (persisted)
*/

#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ====== PINS ======
const uint8_t PIN_TRIG = 9;
const uint8_t PIN_ECHO = 8;
const uint8_t PIN_BTN  = 7;
const uint8_t PIN_BAT  = A0;
const uint8_t PIN_CHRG = 4;  // LOW = charging
const uint8_t PIN_DONE = 5;  // LOW = full

// ====== OLED ======
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ====== RUNTIME CONFIG ======
#define UPDATE_HZ        8
#define N_SAMPLES        15
#define MAX_TIMEOUT_US   40000
#define MAD_MAX_OK_US    120
#define EMA_ALPHA_NUM    35   // 0.35
#define EMA_ALPHA_DEN    100

// ====== EEPROM MODEL STATE ======
struct Calib {
  uint32_t marker;            // 0x90FEE1AA
  uint32_t fresh_us;          // anchors
  uint32_t spoil_us;
  uint8_t  trained;           // TRAIN FLAG: 0=no model, 1=trained
  uint8_t  accum_mode;        // 0 = REPLACE, 1 = ACCUMULATE
  // Persistent totals across CSVTEST sessions (for ACUM mode or just reporting)
  uint32_t tot_sum_fresh;     // sum of echo_us for label=1
  uint32_t tot_sum_spoil;     // sum of echo_us for label=0
  uint32_t tot_cnt_fresh;     // count rows label=1
  uint32_t tot_cnt_spoil;     // count rows label=0
};
const uint32_t CAL_MARKER = 0x90FEE1AA;
const int EEPROM_ADDR = 0;

Calib cal;

// ====== UI/STATE ======
bool running = false;
bool lastBtn = false;
unsigned long lastBtnMs = 0;
const unsigned long DEBOUNCE_MS = 600;

bool streamEnabled = false;
bool snapshotRequest = false;

int emaEcho = -1;   // echo EMA (us)
int vbat_mV = -1;   // battery EMA (mV)

// ====== Per-session CSV trainer accum (RAM, reset each BEGIN) ======
bool csvIngest = false;
uint32_t ses_seen = 0;
uint32_t ses_sum_fresh = 0, ses_sum_spoil = 0;
uint16_t ses_cnt_fresh = 0, ses_cnt_spoil = 0;
uint32_t ses_fresh_anchor = 0, ses_spoil_anchor = 0;
bool ses_anchor_seen = false;

// ====== HELPERS ======

struct EchoStats {
  uint8_t k;
  int med;  // us
  int mad;  // us
  bool ok;
};

static void splash(const __FlashStringHelper* m, uint16_t ms=900){
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0,0);
  display.println(F("FRUITELL Minimal"));
  display.println(m);
  display.display();
  delay(ms);
}
static void saveCal(){ EEPROM.put(EEPROM_ADDR, cal); }
static void loadCal(){
  EEPROM.get(EEPROM_ADDR, cal);
  if (cal.marker != CAL_MARKER) {
    cal.marker = CAL_MARKER;
    cal.fresh_us = 1400;
    cal.spoil_us = 2600;
    cal.trained = 0;
    cal.accum_mode = 0; // default REPLACE
    cal.tot_sum_fresh = cal.tot_sum_spoil = 0;
    cal.tot_cnt_fresh = cal.tot_cnt_spoil = 0;
    EEPROM.put(EEPROM_ADDR, cal);
  }
}
static bool readButton(){
  bool raw = (digitalRead(PIN_BTN)==HIGH); // capacitive touch
  unsigned long now = millis();
  static bool debounced=false;
  if(raw != debounced && (now - lastBtnMs) > DEBOUNCE_MS){
    lastBtnMs = now; debounced = raw;
  }
  bool pressed = debounced && !lastBtn;
  lastBtn = debounced;
  return pressed;
}
static unsigned long measureEcho() {
  digitalWrite(PIN_TRIG, LOW); delayMicroseconds(3);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  return pulseIn(PIN_ECHO, HIGH, MAX_TIMEOUT_US);
}
static void isortUL(unsigned long* a, uint8_t n){
  for(uint8_t i=1;i<n;i++){ unsigned long k=a[i]; uint8_t j=i;
    while(j>0 && a[j-1]>k){ a[j]=a[j-1]; j--; } a[j]=k;
  }
}


static EchoStats acquireWindow(){
  unsigned long buf[N_SAMPLES]; uint8_t k=0;
  for (uint8_t i=0;i<N_SAMPLES;i++){
    unsigned long v = measureEcho();
    if (v>0) buf[k++]=v;
    delay(5);
  }
  EchoStats st; st.k=k; st.med=0; st.mad=0; st.ok=false;
  if (k < 2) return st;

  isortUL(buf,k);
  int med = (k&1)? (int)buf[k/2] : (int)((buf[k/2-1]+buf[k/2])>>1);

  int dev[N_SAMPLES];
  for(uint8_t i=0;i<k;i++){
    long d = (long)buf[i] - (long)med;
    if (d<0) d = -d;
    dev[i] = (int)d;
  }
  // insertion sort for dev median
  for(uint8_t i=1;i<k;i++){ int key=dev[i]; uint8_t j=i;
    while(j>0 && dev[j-1]>key){ dev[j]=dev[j-1]; j--; } dev[j]=key;
  }
  int mad = (k&1)? dev[k/2] : (int)((dev[k/2-1]+dev[k/2])>>1);

  st.med = med; st.mad = mad; st.ok = (mad <= MAD_MAX_OK_US);
  return st;
}

// Linear probability from anchors (0..100)
static int freshPctFromEcho(int echo_us){
  long a = (long)cal.fresh_us;
  long b = (long)cal.spoil_us;
  if (a==b){ b=a+1; }
  long minv = (a<b)?a:b;
  long maxv = (a<b)?b:a;
  long span = maxv - minv;
  long x = echo_us;
  if (x < (int)minv) x = minv;
  if (x > (int)maxv) x = maxv;
  long num = (x - minv) * 100L / (span ? span : 1L);
  if (a < b) num = 100 - num; // smaller echo closer to fresh anchor
  if (num<0) num=0; if (num>100) num=100;
  return (int)num;
}

// Confidence from MAD (0..100)
static int confPctFromMAD(int mad_us){
  long c = 100L - ((long)mad_us * 100L) / (MAD_MAX_OK_US*2L);
  if (c<0) c=0; if (c>100) c=100;
  return (int)c;
}

// Battery (integer only)
static int readBattery_mV(){
  long acc=0; const uint8_t N=10;
  for(uint8_t i=0;i<N;i++){ acc += analogRead(PIN_BAT); delay(2); }
  long adc = acc / N; // 0..1023
  long mV = (adc * 5000L) / 1023L; // Vref 5.0V
  mV = (mV * 32L) / 10L; // *3.2 divider
  if (vbat_mV<0) vbat_mV = (int)mV;
  vbat_mV = ( (EMA_ALPHA_NUM * (int)mV) + ((EMA_ALPHA_DEN-EMA_ALPHA_NUM) * vbat_mV) ) / EMA_ALPHA_DEN;
  return vbat_mV;
}
static int batteryPercent(int mV){
  if(mV <= 3000) return 0;
  if(mV >= 4200) return 100;
  if(mV < 3600)  return (int)((mV-3000L)*15L/600L);
  if(mV < 3900)  return 15 + (int)((mV-3600L)*40L/300L);
  if(mV < 4100)  return 55 + (int)((mV-3900L)*30L/200L);
  return 85 + (int)((mV-4100L)*15L/100L);
}
static const __FlashStringHelper* chargeState(){
  pinMode(PIN_CHRG, INPUT_PULLUP);
  pinMode(PIN_DONE, INPUT_PULLUP);
  bool chrg = (digitalRead(PIN_CHRG)==LOW);
  bool done = (digitalRead(PIN_DONE)==LOW);
  if (chrg) return F("Charging...");
  if (done) return F("Full / Done");
  return F("Idle");
}

// UI
static void drawRunPage(int echo_us,int mad_us,bool stable){
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0,0);
  display.print(F("FRUITELL "));
  display.print(stable?F("[OK] "):F("[HOLD] "));
  if(!cal.trained) display.print(F("(No Model)"));

  // badges
  if (cal.trained){ display.setCursor(SCREEN_WIDTH-14,0); display.print(F("T")); }
  if (cal.accum_mode){ display.setCursor(SCREEN_WIDTH-6,0); display.print(F("A")); }

  display.setCursor(0,12);
  display.print(F("Echo:")); display.print(echo_us);
  display.print(F("  MAD:")); display.print(mad_us);

  if (cal.trained && stable && echo_us>0){
    int fresh = freshPctFromEcho(echo_us);
    int conf  = confPctFromMAD(mad_us);
    display.setCursor(0,24);
    display.print(F("Fresh: ")); display.print(fresh); display.print(F("%  "));
    display.print(F("Conf: "));  display.print(conf);  display.print(F("%"));
    int w = map(fresh,0,100,0,SCREEN_WIDTH-2);
    display.drawRect(0,40,SCREEN_WIDTH-2,12,SSD1306_WHITE);
    display.fillRect(1,41,w,10,SSD1306_WHITE);
  } else {
    display.setCursor(0,24); display.print(F("Fresh: --   Conf: --"));
    display.drawRect(0,40,SCREEN_WIDTH-2,12,SSD1306_WHITE);
  }

  display.setCursor(0,56);
  display.print(F("F:")); display.print((int)cal.fresh_us);
  display.print(F(" S:")); display.print((int)cal.spoil_us);
  display.display();
}



// Printing
static void printCSVLine(unsigned long ts_ms, int echo_us, int mad_us){
  int fresh=0, conf=0;
  if (cal.trained){ fresh = freshPctFromEcho(echo_us); conf = confPctFromMAD(mad_us); }
  Serial.print(ts_ms); Serial.print(',');
  Serial.print(echo_us); Serial.print(',');
  Serial.print(mad_us);  Serial.print(',');
  Serial.print(fresh);   Serial.print(',');
  Serial.print(conf);    Serial.print(',');
  Serial.print((int)cal.fresh_us); Serial.print(',');
  Serial.println((int)cal.spoil_us);
}
static void printHumanLine(int echo_us, int mad_us){
  if (!cal.trained) return;
  int fresh = freshPctFromEcho(echo_us);
  int conf  = confPctFromMAD(mad_us);
  Serial.print(F("Fresh=")); Serial.print(fresh); Serial.print(F("%, "));
  Serial.print(F("Conf="));  Serial.print(conf);  Serial.print(F("%, "));
  Serial.print(F("Echo="));  Serial.print(echo_us); Serial.print(F("us, "));
  Serial.print(F("MAD="));   Serial.print(mad_us);  Serial.print(F("us, "));
  Serial.print(F("F="));     Serial.print((int)cal.fresh_us); Serial.print(F(", "));
  Serial.print(F("S="));     Serial.println((int)cal.spoil_us);
}

// Streaming while TRAIN:ON
static void streamOnceIfEnabled(){
  if(!streamEnabled) return;
  EchoStats st = acquireWindow();
  if (st.k >= 2 && st.med > 0){
    int echo = st.med, mad = st.mad;
    printCSVLine(millis(), echo, mad);
    if (snapshotRequest) {
  if (cal.trained && mad <= MAD_MAX_OK_US) printHumanLine(echo, mad);
  snapshotRequest=false;
}
  }
}

// ====== CSV trainer ======
static void csvResetSession(){
  csvIngest=false; ses_seen=0;
  ses_sum_fresh=0; ses_sum_spoil=0; ses_cnt_fresh=0; ses_cnt_spoil=0;
  ses_fresh_anchor=0; ses_spoil_anchor=0; ses_anchor_seen=false;
}
static void csvBegin(){
  csvResetSession();
  csvIngest = true;
  streamEnabled = false;   // pause telemetry
  running = false;    
  Serial.println(F("CSVTEST:READY"));
}
static void csvFeedLine(const String& sLine){
  String line = sLine; line.trim(); if (!line.length()) return;
  if (line.startsWith("echo") || line.startsWith("ts,")) return;

  int p1=line.indexOf(','); if(p1<0) return;
  int p2=line.indexOf(',',p1+1); if(p2<0) return;
  int p3=line.indexOf(',',p2+1); if(p3<0) return;

  long echo = line.substring(0,p1).toInt();
  int  lab  = line.substring(p1+1,p2).toInt();
  long fa   = line.substring(p2+1,p3).toInt();
  long sa   = line.substring(p3+1).toInt();

  if (fa>0 && sa>0){ ses_fresh_anchor=fa; ses_spoil_anchor=sa; ses_anchor_seen=true; }
  if (echo>0){
    if (lab==1){ ses_sum_fresh += (uint32_t)echo; ses_cnt_fresh++; }
    else       { ses_sum_spoil += (uint32_t)echo; ses_cnt_spoil++; }
    ses_seen++;
    Serial.print(F("CSVROW:")); Serial.print(echo); Serial.print(','); Serial.println(lab);
  }
}
static void csvApplySessionToTotals(){
  // If REPLACE mode: clear totals first
  if (cal.accum_mode == 0){
    cal.tot_sum_fresh = cal.tot_sum_spoil = 0;
    cal.tot_cnt_fresh = cal.tot_cnt_spoil = 0;
  }
  // Add session to totals
  cal.tot_sum_fresh += ses_sum_fresh;
  cal.tot_sum_spoil += ses_sum_spoil;
  cal.tot_cnt_fresh += ses_cnt_fresh;
  cal.tot_cnt_spoil += ses_cnt_spoil;

  // Update anchors from session if provided
  if (ses_anchor_seen){
    cal.fresh_us = ses_fresh_anchor;
    cal.spoil_us = ses_spoil_anchor;
  }

  // Mark trained
  cal.trained = 1;
  saveCal();
}

// Battery/charging page (shown when STOPPED)
static void drawBatteryPage(){
  int mv  = readBattery_mV();
  int pct = batteryPercent(mv);

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0,0);
  display.println(F("FRUITELL (Stopped)"));

  display.setCursor(0,14);
  display.print(F("Battery: "));
  display.print(pct);
  display.print(F("%  ("));
  display.print(mv/1000);
  display.print(F("."));
  display.print((mv%1000)/10);
  display.println(F("V)"));

  int w = map(pct, 0, 100, 0, SCREEN_WIDTH-2);
  display.drawRect(0, 28, SCREEN_WIDTH-2, 12, SSD1306_WHITE);
  display.fillRect(1, 29, w, 10, SSD1306_WHITE);

  display.setCursor(0,46);
  display.println(chargeState());

  display.display();
}

static void csvEndAndTrain(){
  if (!csvIngest){ Serial.println(F("CSVTEST:ERR")); return; }
  csvIngest=false;

  if ((ses_cnt_fresh==0 && cal.tot_cnt_fresh==0) ||
      (ses_cnt_spoil==0 && cal.tot_cnt_spoil==0)){
    // Require both classes in either the session or already accumulated totals
    Serial.println(F("CSVTEST:ERR need both classes"));
    return;
  }

  csvApplySessionToTotals();

  // quick report
  // Prefer totals if available, else session
  uint32_t cf = cal.tot_cnt_fresh ? cal.tot_cnt_fresh : ses_cnt_fresh;
  uint32_t cs = cal.tot_cnt_spoil ? cal.tot_cnt_spoil : ses_cnt_spoil;
  uint32_t sf = cal.tot_cnt_fresh ? cal.tot_sum_fresh : ses_sum_fresh;
  uint32_t ss = cal.tot_cnt_spoil ? cal.tot_sum_spoil : ses_sum_spoil;

  uint32_t mu_f = cf ? (sf / cf) : 0;
  uint32_t mu_s = cs ? (ss / cs) : 0;

  Serial.println(F("CSVTEST:DONE"));
  Serial.print(F(" session_rows=")); Serial.println(ses_seen);
  Serial.print(F(" totals_fresh_cnt=")); Serial.println(cal.tot_cnt_fresh);
  Serial.print(F(" totals_spoil_cnt=")); Serial.println(cal.tot_cnt_spoil);
  Serial.print(F(" mu_fresh~=")); Serial.println(mu_f);
  Serial.print(F(" mu_spoil~=")); Serial.println(mu_s);
  Serial.print(F(" anchors F,S=")); Serial.print((int)cal.fresh_us); Serial.print(','); Serial.println((int)cal.spoil_us);
}

// ====== SETUP / LOOP ======
void setup(){
  pinMode(PIN_TRIG,OUTPUT);
  pinMode(PIN_ECHO,INPUT);
  pinMode(PIN_BTN, INPUT);       // capacitive/touch input (HIGH when touched)
  pinMode(PIN_BAT, INPUT);
  pinMode(PIN_CHRG, INPUT_PULLUP);
  pinMode(PIN_DONE, INPUT_PULLUP);

  Wire.begin();
  display.begin(SSD1306_SWITCHCAPVCC,0x3C);
  display.clearDisplay(); display.display();

  Serial.begin(115200);
  Serial.setTimeout(4000);
  loadCal();
  splash(F("Use CSVTEST or F/S"), 1200);
}

void loop(){
  // Serial
  if (Serial.available()){
    String line = Serial.readStringUntil('\n'); line.trim();

    if (csvIngest && !line.equalsIgnoreCase("CSVTEST:END")){
      csvFeedLine(line);
    } else if (line.equalsIgnoreCase("R")){
      Serial.print(F("Anchors: ")); Serial.print((int)cal.fresh_us); Serial.print(','); Serial.println((int)cal.spoil_us);
      Serial.print(F("TRAINED=")); Serial.println((int)cal.trained);
      Serial.print(F("ACCUM_MODE=")); Serial.println((int)cal.accum_mode);
      Serial.print(F("TOTAL fresh cnt/sum=")); Serial.print(cal.tot_cnt_fresh); Serial.print(F("/")); Serial.println(cal.tot_sum_fresh);
      Serial.print(F("TOTAL spoil cnt/sum=")); Serial.print(cal.tot_cnt_spoil); Serial.print(F("/")); Serial.println(cal.tot_sum_spoil);
    } else if (line.equalsIgnoreCase("F")){
      EchoStats st=acquireWindow();
      if (st.k>=7 && st.med>0){ cal.fresh_us=(uint32_t)st.med; saveCal(); splash(F("Fresh anchor saved")); }
      else splash(F("Hold steady & retry"));
    } else if (line.equalsIgnoreCase("S")){
      EchoStats st=acquireWindow();
      if (st.k>=7 && st.med>0){ cal.spoil_us=(uint32_t)st.med; saveCal(); splash(F("Spoil anchor saved")); }
      else splash(F("Hold steady & retry"));
    } else if (line.equalsIgnoreCase("MODEL:RESET")){
      cal.trained = 0;
      cal.tot_sum_fresh = cal.tot_sum_spoil = 0;
      cal.tot_cnt_fresh = cal.tot_cnt_spoil = 0;
      saveCal();
      Serial.println(F("WOK: model reset (TRAINED=0, totals cleared)"));
      splash(F("Model cleared"), 700);
    } else if (line.equalsIgnoreCase("TRAIN:ON")){
      streamEnabled = true; splash(F("Training stream ON"), 600);
    } else if (line.equalsIgnoreCase("TRAIN:OFF")){
      streamEnabled = false; splash(F("Training stream OFF"), 600);
    } else if (line.equalsIgnoreCase("SNAP")){
      snapshotRequest = true;
    } else if (line.equalsIgnoreCase("CSVTEST:BEGIN")){
      csvBegin();
    } else if (line.equalsIgnoreCase("CSVTEST:END")){
      csvEndAndTrain();
    } else if (line.equalsIgnoreCase("TFLAG?")){
      Serial.print(F("TRAINED=")); Serial.println((int)cal.trained);
    } else if (line.startsWith("TFLAG:")){
      int v = line.substring(6).toInt();
      cal.trained = (v?1:0); saveCal();
      Serial.print(F("WOK: TRAINED=")); Serial.println((int)cal.trained);
    } else if (line.equalsIgnoreCase("CSVACCUM:ON")){
      cal.accum_mode = 1; saveCal();
      Serial.println(F("WOK: ACCUMULATE mode ON"));
    } else if (line.equalsIgnoreCase("CSVACCUM:OFF")){
      cal.accum_mode = 0; saveCal();
      Serial.println(F("WOK: ACCUMULATE mode OFF (REPLACE)"));
    } else if (line.equalsIgnoreCase("CSVACCUM:CLEAR")){
      cal.tot_sum_fresh = cal.tot_sum_spoil = 0;
      cal.tot_cnt_fresh = cal.tot_cnt_spoil = 0;
      saveCal();
      Serial.println(F("WOK: totals cleared"));
    }
  }
  if (csvIngest) { delay(5); return; }  // drain serial aggressively

  // Toggle run/stop
  if (readButton()){
    running = !running;
    splash(running?F("Device STARTED"):F("Device STOPPED"), 700);
  }

  if (running){
    EchoStats st = acquireWindow();
    Serial.print(F("DBG k=")); Serial.print(st.k);
Serial.print(F(" med=")); Serial.print(st.med);
Serial.print(F(" mad=")); Serial.println(st.mad);
    if (st.med>0){
      if (emaEcho<0) emaEcho = st.med;
      emaEcho = (EMA_ALPHA_NUM*st.med + (EMA_ALPHA_DEN-EMA_ALPHA_NUM)*emaEcho)/EMA_ALPHA_DEN;
    }
    drawRunPage( (emaEcho<0)?0:emaEcho, st.mad, st.ok );
    streamOnceIfEnabled();

    if (snapshotRequest){
      int echo = (emaEcho<0)?0:emaEcho;
      printCSVLine(millis(), echo, st.mad);
      if (st.ok) printHumanLine(echo, st.mad);
            snapshotRequest=false;
          }
  } else {
    drawBatteryPage();
    streamOnceIfEnabled();

    if (snapshotRequest){
      EchoStats st2=acquireWindow();
      if (st2.med>0){
        printCSVLine(millis(), st2.med, st2.mad);
if (st2.ok) printHumanLine(st2.med, st2.mad);
      } else {
        Serial.println(F("SNAP: no valid echo"));
      }
      snapshotRequest=false;
    }
  }

  delay(1000/UPDATE_HZ);
}
