// Walkie-talkie using ESPNOW
// Half-duplex push-to-talk voice over ESP-NOW. Same sketch on BOTH boards.
// Hold the button to talk, release to listen.
//
// Hardware: MAX9814 OUT -> GPIO34, amp module L input <- GPIO25 (DAC),
//           push button between GPIO27 and GND. Onboard LED (GPIO2) = transmitting.
// Board:    ESP32 Dev Module (classic ESP32, needs the GPIO25 DAC)

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

const uint8_t PIN_MIC = 34;
const uint8_t PIN_DAC = 25;
const uint8_t PIN_BTN = 27;
const uint8_t PIN_LED = 2;

const int SAMPLE_RATE = 16000;
const unsigned long PERIOD_US = 1000000UL / SAMPLE_RATE;   // 62.5 us
const int SAMPLES_PER_PKT = 128;                           // 8 ms per packet at 16 kHz
const uint8_t T_START = 1, T_AUDIO = 2, T_END = 3;
const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Tuning
const float SOFT_GAIN  = 3.0f;    // mic volume
const float LP_ALPHA   = 0.8f;    // low-pass (lower = less hiss, more muffled)
const float GATE_OPEN  = 25.0f;   // noise gate thresholds (ADC counts)
const float GATE_CLOSE = 15.0f;
const float BEEP_VOL   = 60.0f;   // beep loudness
const uint16_t PREBUFFER = 512;   // samples buffered before playback (32 ms)

// Mic processing state
float dcLevel = 2048.0f, lp = 0.0f, env = 0.0f, gateGain = 0.0f;
bool gateOpen = false;

// Receive ring buffer (filled by the ESP-NOW callback, emptied by loop)
const uint16_t RB_SIZE = 2048, RB_MASK = RB_SIZE - 1;
uint8_t rb[RB_SIZE];
volatile uint16_t head = 0, tail = 0;
inline uint16_t rbCount() { return (uint16_t)(head - tail) & RB_MASK; }

volatile bool evStart = false, evEnd = false, txMode = false;
volatile unsigned long lastRxMs = 0;
volatile uint32_t rxPackets = 0;

bool rxActive = false, rxEnding = false, playing = false;
unsigned long nextPlay = 0;

// ---------------- Beeps ----------------
void playTone(float freq, int ms) {
  int n = SAMPLE_RATE * ms / 1000;
  unsigned long t = micros();
  for (int i = 0; i < n; i++) {
    int edge = min(i, n - i);
    float e = edge >= 40 ? 1.0f : edge / 40.0f;
    float s = sinf(2.0f * PI * freq * i / SAMPLE_RATE);
    dacWrite(PIN_DAC, (uint8_t)(128 + BEEP_VOL * e * s));
    t += PERIOD_US;
    while ((long)(micros() - t) < 0) {}
  }
  dacWrite(PIN_DAC, 128);
}
void beepIncoming() { playTone(900, 60);  playTone(1300, 60); }
void beepRoger()    { playTone(1300, 60); playTone(900, 60);  }

// ---------------- ESP-NOW receive ----------------
void handlePacket(const uint8_t *data, int len) {
  if (len < 1 || txMode) return;              // ignore while we are talking
  lastRxMs = millis();
  if (data[0] == T_START) {
    evStart = true;
  } else if (data[0] == T_END) {
    evEnd = true;
  } else if (data[0] == T_AUDIO) {
    rxPackets++;
    uint16_t h = head;
    for (int i = 1; i < len; i++) {
      if (((h + 1) & RB_MASK) != tail) {
        rb[h] = data[i];
        h = (h + 1) & RB_MASK;
      }
    }
    head = h;
  }
}

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  handlePacket(data, len);
}
#else
void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
  handlePacket(data, len);
}
#endif

void sendControl(uint8_t type) {
  for (int i = 0; i < 3; i++) {               // 3 times in case one is lost
    esp_now_send(BROADCAST, &type, 1);
    delay(3);
  }
}

// ---------------- Receive mode ----------------
void startRx(bool beep) {
  rxActive = true;
  rxEnding = false;
  playing = false;
  Serial.println("RX: start");
  if (beep) beepIncoming();
  nextPlay = micros() + PERIOD_US;
}

void doReceive() {
  if (evStart) { evStart = false; if (!rxActive) startRx(true); }
  if (evEnd)   { evEnd = false;   if (rxActive) rxEnding = true; }
  if (!rxActive && rbCount() > 0) startRx(false);       // start packet was missed

  unsigned long now = micros();
  if ((long)(now - nextPlay) >= 0) {
    nextPlay += PERIOD_US;
    if ((long)(now - nextPlay) > 1000) nextPlay = now + PERIOD_US;

    if (!playing && rxActive && (rbCount() >= PREBUFFER || (rxEnding && rbCount() > 0)))
      playing = true;

    if (playing && rbCount() > 0) {
      dacWrite(PIN_DAC, rb[tail]);
      tail = (tail + 1) & RB_MASK;
    } else {
      if (playing) playing = false;                     // ran dry, rebuffer
      dacWrite(PIN_DAC, 128);
    }
  }

  // No audio for a while: treat as ended
  if (rxActive && !rxEnding && millis() - lastRxMs > 500) rxEnding = true;

  // Finished: play the "over" beep
  if (rxActive && rxEnding && rbCount() == 0) {
    Serial.printf("RX: done, audio packets received: %lu\n", (unsigned long)rxPackets);
    rxPackets = 0;
    beepRoger();
    rxActive = rxEnding = playing = false;
    tail = head;
    nextPlay = micros() + PERIOD_US;
  }
}

// ---------------- Transmit mode ----------------
void doTransmit() {
  digitalWrite(PIN_LED, HIGH);
  txMode = true;
  rxActive = rxEnding = playing = false;
  tail = head;
  Serial.println("TX: start");

  sendControl(T_START);
  playTone(1200, 60);                         // local "you are live" beep
  delay(60);                                  // lets the other side finish its beep

  lp = 0; env = 0; gateGain = 0; gateOpen = false;
  uint8_t buf[SAMPLES_PER_PKT + 1];
  buf[0] = T_AUDIO;
  int n = 0;
  uint32_t sent = 0;
  unsigned long next = micros();
  unsigned long releaseStart = 0;

  while (true) {
    if (digitalRead(PIN_BTN) == HIGH) {         // released for 30 ms = stop
      if (releaseStart == 0) releaseStart = millis();
      else if (millis() - releaseStart > 30) break;
    } else {
      releaseStart = 0;
    }

    if ((long)(micros() - next) < 0) continue;
    next += PERIOD_US;

    float raw = (analogRead(PIN_MIC) + analogRead(PIN_MIC)) * 0.5f;
    dcLevel += (raw - dcLevel) * 0.0005f;
    float ac = raw - dcLevel;
    lp += LP_ALPHA * (ac - lp);

    float a = fabsf(lp);
    env += (a - env) * 0.05f;
    if (!gateOpen && env > GATE_OPEN)  gateOpen = true;
    if (gateOpen  && env < GATE_CLOSE) gateOpen = false;
    gateGain += ((gateOpen ? 1.0f : 0.0f) - gateGain) * (gateOpen ? 0.05f : 0.002f);

    float out = 128.0f + (lp * gateGain * SOFT_GAIN) / 16.0f;
    if (out < 0) out = 0;
    if (out > 255) out = 255;
    buf[1 + n] = (uint8_t)out;
    n++;

    if (n == SAMPLES_PER_PKT) {
      esp_now_send(BROADCAST, buf, SAMPLES_PER_PKT + 1);
      sent++;
      n = 0;
    }
  }

  sendControl(T_END);
  Serial.printf("TX: end, audio packets sent: %lu\n", (unsigned long)sent);
  txMode = false;
  tail = head;
  digitalWrite(PIN_LED, LOW);
  dacWrite(PIN_DAC, 128);
  nextPlay = micros() + PERIOD_US;
}

// ---------------- Arduino ----------------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_MIC, ADC_11db);
  dacWrite(PIN_DAC, 128);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);   // both boards on the same channel

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    while (true) { digitalWrite(PIN_LED, !digitalRead(PIN_LED)); delay(150); }
  }
  esp_now_register_recv_cb(onRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST, 6);
  peer.channel = 1;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  long sum = 0;
  for (int i = 0; i < 500; i++) { sum += analogRead(PIN_MIC); delay(1); }
  dcLevel = sum / 500.0f;

  nextPlay = micros();
  Serial.print("My MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.println("Ready. Hold the button to talk.");
}

void loop() {
  if (digitalRead(PIN_BTN) == LOW) {
    delay(20);                                       // debounce
    if (digitalRead(PIN_BTN) == LOW) {
      doTransmit();
      return;
    }
  }
  doReceive();
}
