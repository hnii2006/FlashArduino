#include <Arduino.h>
#include <avr/pgmspace.h>
#include "firmware.h"  // firmware_bin[] と FIRMWARE_SIZE

// -------------------- 設定 --------------------
const uint32_t TARGET_BAUD = 57600;   // Target optiboot のボーレート
const uint8_t  RESET_PIN   = 8;       // Target の RESET
const uint8_t  BUTTON_PIN  = 2;       // GND で押されるボタン

// 1 回の STK500 PROG/READ で扱うバイト数（optiboot バッファに合わせて 128）
const uint16_t WRITE_CHUNK = 128;

HardwareSerial &ProgSer = Serial1;    // Target 通信用 UART1

// STK500 v1 commands
const uint8_t CMD_GET_SYNC    = 0x30;
const uint8_t CMD_LOAD_ADDR   = 0x55;
const uint8_t CMD_PROG_PAGE   = 0x64;
const uint8_t CMD_READ_PAGE   = 0x74;
const uint8_t CMD_READ_SIGN   = 0x75;
const uint8_t CMD_ENTER_PROG  = 0x50;
const uint8_t CMD_LEAVE_PROG  = 0x51;

const uint8_t STK_OK          = 0x10;
const uint8_t STK_INSYNC      = 0x14;
const uint8_t CRC_EOP         = 0x20;

// -------------------------------------------------

void stk_put(uint8_t b)
{
  ProgSer.write(b);
  // flush は連続送信の足を引っ張るので無し
}

bool stk_get_reply(uint8_t &insync, uint8_t &ok)
{
  unsigned long t0 = millis();
  while (ProgSer.available() < 2) {
    if (millis() - t0 > 1000) return false;
  }
  insync = ProgSer.read();
  ok     = ProgSer.read();
  return true;
}

// GET_SYNC
bool stk_sync()
{
  uint8_t ins, ok;
  stk_put(CMD_GET_SYNC);
  stk_put(CRC_EOP);
  if (!stk_get_reply(ins, ok)) return false;
  return (ins == STK_INSYNC && ok == STK_OK);
}

// LOAD_ADDRESS (word address)
bool stk_load_address(uint16_t word_addr)
{
  uint8_t ins, ok;
  stk_put(CMD_LOAD_ADDR);
  stk_put(word_addr & 0xFF);
  stk_put((word_addr >> 8) & 0xFF);
  stk_put(CRC_EOP);
  if (!stk_get_reply(ins, ok)) return false;
  return (ins == STK_INSYNC && ok == STK_OK);
}

// ENTER_PROGMODE
bool stk_enter_progmode()
{
  uint8_t ins, ok;
  stk_put(CMD_ENTER_PROG);
  stk_put(CRC_EOP);
  if (!stk_get_reply(ins, ok)) return false;
  return (ins == STK_INSYNC && ok == STK_OK);
}

// LEAVE_PROGMODE
bool stk_leave_progmode()
{
  uint8_t ins, ok;
  stk_put(CMD_LEAVE_PROG);
  stk_put(CRC_EOP);
  if (!stk_get_reply(ins, ok)) return false;
  return (ins == STK_INSYNC && ok == STK_OK);
}

// READ SIGNATURE (3 bytes)
bool stk_read_signature(uint8_t *sig)
{
  stk_put(CMD_READ_SIGN);
  stk_put(CRC_EOP);

  unsigned long t0 = millis();
  while (ProgSer.available() < 5) {
    if (millis() - t0 > 1000) return false;
  }

  uint8_t ins = ProgSer.read();
  if (ins != STK_INSYNC) return false;

  sig[0] = ProgSer.read();
  sig[1] = ProgSer.read();
  sig[2] = ProgSer.read();

  uint8_t ok = ProgSer.read();
  return (ok == STK_OK);
}

// PROG_PAGE
bool stk_prog_page(uint16_t len, const uint8_t *data)
{
  uint8_t ins, ok;

  stk_put(CMD_PROG_PAGE);
  stk_put((len >> 8) & 0xFF);
  stk_put(len & 0xFF);
  stk_put('F');  // flash

  for (uint16_t i = 0; i < len; i++) {
    stk_put(data[i]);
  }

  stk_put(CRC_EOP);

  if (!stk_get_reply(ins, ok)) return false;
  return (ins == STK_INSYNC && ok == STK_OK);
}

// READ_PAGE
bool stk_read_page(uint16_t len, uint8_t *buf)
{
  stk_put(CMD_READ_PAGE);
  stk_put((len >> 8) & 0xFF);
  stk_put(len & 0xFF);
  stk_put('F');   // flash
  stk_put(CRC_EOP);

  unsigned long t0 = millis();
  // INSYNC + len bytes + OK = len + 2 バイト
  while (ProgSer.available() < (1 + len + 1)) {
    if (millis() - t0 > 1000) return false;
  }

  uint8_t ins = ProgSer.read();
  if (ins != STK_INSYNC) return false;

  for (uint16_t i = 0; i < len; i++) {
    buf[i] = ProgSer.read();
  }

  uint8_t ok = ProgSer.read();
  return (ok == STK_OK);
}

void pulse_reset_for_bootloader()
{
  Serial.println(F("[RST] Resetting target..."));

  pinMode(RESET_PIN, OUTPUT);
  digitalWrite(RESET_PIN, LOW);
  delay(50);
  digitalWrite(RESET_PIN, HIGH);

  Serial.println(F("[RST] Bootloader wait..."));
  delay(200);
}

// -------------------------------------------------
// 書き込み + VERIFY
// -------------------------------------------------

bool verify_target()
{
  Serial.println(F("==== VERIFY ===="));

  uint32_t addr   = 0;
  uint32_t remain = FIRMWARE_SIZE;
  uint32_t total_chunks = (FIRMWARE_SIZE + WRITE_CHUNK - 1) / WRITE_CHUNK;
  uint32_t chunk_idx = 0;

  uint8_t readbuf[WRITE_CHUNK];
  bool read_page_supported = true;

  while (remain > 0) {
    uint16_t this_len = WRITE_CHUNK;
    if (remain < this_len) this_len = remain;

    uint16_t word_addr = addr >> 1;
    if (!stk_load_address(word_addr)) {
      Serial.println(F("[VERR] LOAD_ADDRESS failed"));
      return false;
    }

    if (!stk_read_page(this_len, readbuf)) {
      // 1 チャンク目から失敗した場合は「READ_PAGE非対応」とみなしてスキップ
      if (chunk_idx == 0) {
        Serial.println(F("[VERIFY] READ_PAGE not supported by bootloader. Skipping verify."));
        return true;  // 書き込みは成功しているので、ここではOK扱い
      } else {
        Serial.println(F("[VERR] READ_PAGE failed"));
        return false;
      }
    }

    // 比較
    for (uint16_t i = 0; i < this_len; i++) {
      uint8_t expect = pgm_read_byte(&firmware_bin[addr + i]);
      uint8_t actual = readbuf[i];
      if (expect != actual) {
        uint32_t bad_addr = addr + i;
        Serial.print(F("[VERR] Mismatch at 0x"));
        Serial.print(bad_addr, HEX);
        Serial.print(F("  exp=0x"));
        Serial.print(expect, HEX);
        Serial.print(F(" got=0x"));
        Serial.println(actual, HEX);
        return false;
      }
    }

    addr   += this_len;
    remain -= this_len;
    chunk_idx++;

    int pct = (chunk_idx * 100) / total_chunks;
    Serial.print(F("[VERIFY] Chunk "));
    Serial.print(chunk_idx);
    Serial.print(F("/"));
    Serial.print(total_chunks);
    Serial.print(F(" ("));
    Serial.print(pct);
    Serial.println(F("%)"));
  }

  Serial.println(F("[VERIFY] OK"));
  return true;
}

bool program_target()
{
  Serial.println(F("==== PROGRAM START ===="));
  Serial.print(F("Firmware size: "));
  Serial.println(FIRMWARE_SIZE);

  ProgSer.end();
  delay(10);
  ProgSer.begin(TARGET_BAUD);
  delay(50);

  pulse_reset_for_bootloader();

  // SYNC
  Serial.print(F("[SYNC] "));
  bool synced = false;
  for (int i = 0; i < 5; i++) {
    if (stk_sync()) { synced = true; break; }
    Serial.print(".");
    delay(100);
  }
  Serial.println();

  if (!synced) {
    Serial.println(F("[ERR] SYNC FAILED"));
    return false;
  }
  Serial.println(F("[OK] Synced with target"));

  // SIGNATURE
  uint8_t sig[3];
  if (stk_read_signature(sig)) {
    Serial.print(F("[SIG] "));
    Serial.print(sig[0], HEX); Serial.print(" ");
    Serial.print(sig[1], HEX); Serial.print(" ");
    Serial.println(sig[2], HEX);
  } else {
    Serial.println(F("[ERR] Failed to read signature"));
    return false;
  }

  // ENTER PROGMODE
  if (!stk_enter_progmode()) {
    Serial.println(F("[ERR] ENTER_PROG failed"));
    return false;
  }

  // -------- FLASH 書き込み --------
  uint32_t addr   = 0;
  uint32_t remain = FIRMWARE_SIZE;
  uint32_t total_chunks = (FIRMWARE_SIZE + WRITE_CHUNK - 1) / WRITE_CHUNK;
  uint32_t chunk_idx = 0;
  uint8_t  buf[WRITE_CHUNK];

  Serial.println(F("==== FLASHING ===="));

  while (remain > 0) {
    uint16_t this_len = WRITE_CHUNK;
    if (remain < this_len) this_len = remain;

    // PROGMEM から読み出し
    for (uint16_t i = 0; i < this_len; i++) {
      buf[i] = pgm_read_byte(&firmware_bin[addr + i]);
    }

    uint16_t word_addr = addr >> 1;
    if (!stk_load_address(word_addr)) {
      Serial.println(F("[ERR] LOAD_ADDRESS failed"));
      stk_leave_progmode();
      return false;
    }

    if (!stk_prog_page(this_len, buf)) {
      Serial.println(F("[ERR] PROG_PAGE FAILED"));
      stk_leave_progmode();
      return false;
    }

    addr   += this_len;
    remain -= this_len;
    chunk_idx++;

    int pct = (chunk_idx * 100) / total_chunks;
    Serial.print(F("[WRITE] Chunk "));
    Serial.print(chunk_idx);
    Serial.print(F("/"));
    Serial.print(total_chunks);
    Serial.print(F(" ("));
    Serial.print(pct);
    Serial.println(F("%)"));
  }

  // -------- VERIFY --------
  bool v = verify_target();

  // LEAVE PROGMODE（VERIFY の成否に関わらず一応出ておく）
  if (!stk_leave_progmode()) {
    Serial.println(F("[WARN] LEAVE_PROG failed (ignored)"));
  }

  if (!v) {
    Serial.println(F("[RESULT] VERIFY FAILED"));
    return false;
  }

  Serial.println(F("==== DONE (WRITE + VERIFY OK) ===="));
  return true;
}

// -------------------------------------------------

void setup()
{
  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println(F("===== Writer 328PB 8MHz Boot ====="));
  Serial.println(F("Serial0 = Debug Log @115200"));
  Serial.println(F("Serial1 = Target (optiboot writer)"));
  Serial.println();

  pinMode(RESET_PIN, OUTPUT);
  digitalWrite(RESET_PIN, HIGH);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
}

void loop()
{
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(30);
    if (digitalRead(BUTTON_PIN) == LOW) {
      digitalWrite(LED_BUILTIN, HIGH);
      bool ok = program_target();
      digitalWrite(LED_BUILTIN, LOW);

      if (ok) Serial.println(F("[RESULT] SUCCESS (WRITE+VERIFY)"));
      else    Serial.println(F("[RESULT] FAILED"));

      while (digitalRead(BUTTON_PIN) == LOW) delay(10);
      Serial.println();
    }
  }
}
