#include <Preferences.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <SPI.h>
#include "Utils.h"
#include "ConfigSettings.h"
#include "Somfy.h"
#include "Sockets.h"
#include "MQTT.h"
#include "ConfigFile.h"

extern Preferences pref;
extern SomfyShadeController somfy;
extern SocketEmitter sockEmit;
extern MQTTClass mqtt;


uint8_t rxmode = 0;  // Indicates whether the radio is in receive mode.  Just to ensure there isn't more than one interrupt hooked.
#define SYMBOL 640
#if defined(ESP8266)
    #define RECEIVE_ATTR ICACHE_RAM_ATTR
#elif defined(ESP32)
    #define RECEIVE_ATTR IRAM_ATTR
#else
    #define RECEIVE_ATTR
#endif

#define SETMY_REPEATS 15
#define TILT_REPEATS 15

int sort_asc(const void *cmp1, const void *cmp2) {
  int a = *((uint8_t *)cmp1);
  int b = *((uint8_t *)cmp2);
  if(a == b) return 0;
  else if(a < b) return -1;
  return 1;
}

static int interruptPin = 0;
static uint8_t bit_length = 56;
somfy_commands translateSomfyCommand(const String& string) {
    if (string.equalsIgnoreCase("My")) return somfy_commands::My;
    else if (string.equalsIgnoreCase("Up")) return somfy_commands::Up;
    else if (string.equalsIgnoreCase("MyUp")) return somfy_commands::MyUp;
    else if (string.equalsIgnoreCase("Down")) return somfy_commands::Down;
    else if (string.equalsIgnoreCase("MyDown")) return somfy_commands::MyDown;
    else if (string.equalsIgnoreCase("UpDown")) return somfy_commands::UpDown;
    else if (string.equalsIgnoreCase("MyUpDown")) return somfy_commands::MyUpDown;
    else if (string.equalsIgnoreCase("Prog")) return somfy_commands::Prog;
    else if (string.equalsIgnoreCase("SunFlag")) return somfy_commands::SunFlag;
    else if (string.equalsIgnoreCase("StepUp")) return somfy_commands::StepUp;
    else if (string.equalsIgnoreCase("StepDown")) return somfy_commands::StepDown;
    else if (string.equalsIgnoreCase("Flag")) return somfy_commands::Flag;
    else if (string.startsWith("mud") || string.startsWith("MUD")) return somfy_commands::MyUpDown;
    else if (string.startsWith("md") || string.startsWith("MD")) return somfy_commands::MyDown;
    else if (string.startsWith("ud") || string.startsWith("UD")) return somfy_commands::UpDown;
    else if (string.startsWith("mu") || string.startsWith("MU")) return somfy_commands::MyUp;
    else if (string.startsWith("su") || string.startsWith("SU")) return somfy_commands::StepUp;
    else if (string.startsWith("sd") || string.startsWith("SD")) return somfy_commands::StepDown;
    else if (string.startsWith("p") || string.startsWith("P")) return somfy_commands::Prog;
    else if (string.startsWith("u") || string.startsWith("U")) return somfy_commands::Up;
    else if (string.startsWith("d") || string.startsWith("D")) return somfy_commands::Down;
    else if (string.startsWith("m") || string.startsWith("M")) return somfy_commands::My;
    else if (string.startsWith("f") || string.startsWith("F")) return somfy_commands::Flag;
    else if (string.startsWith("s") || string.startsWith("S")) return somfy_commands::SunFlag;
    else if (string.length() == 1) return static_cast<somfy_commands>(strtol(string.c_str(), nullptr, 16));
    else return somfy_commands::My;
}
String translateSomfyCommand(const somfy_commands cmd) {
    switch (cmd) {
    case somfy_commands::Up:
        return "Up";
    case somfy_commands::Down:
        return "Down";
    case somfy_commands::My:
        return "My";
    case somfy_commands::MyUp:
        return "My+Up";
    case somfy_commands::UpDown:
        return "Up+Down";
    case somfy_commands::MyDown:
        return "My+Down";
    case somfy_commands::MyUpDown:
        return "My+Up+Down";
    case somfy_commands::Prog:
        return "Prog";
    case somfy_commands::SunFlag:
        return "Sun Flag";
    case somfy_commands::Flag:
        return "Flag";
    case somfy_commands::StepUp:
        return "Step Up";
    case somfy_commands::StepDown:
        return "Step Down";
    default:
        return "Unknown(" + String((uint8_t)cmd) + ")";
    }
}
void somfy_frame_t::decodeFrame(byte* frame) {
    byte decoded[10];
    decoded[0] = frame[0];
    // The last 3 bytes are not encoded even on 80-bits. Go figure.
    decoded[7] = frame[7];
    decoded[8] = frame[8];
    decoded[9] = frame[9];
    for (byte i = 1; i < 7; i++) {
        decoded[i] = frame[i] ^ frame[i - 1];
    }
    byte checksum = 0;
    // We only want the upper nibble for the command byte.
    for (byte i = 0; i < 7; i++) {
        if (i == 1) checksum = checksum ^ (decoded[i] >> 4);
        else checksum = checksum ^ decoded[i] ^ (decoded[i] >> 4);
    }
    checksum &= 0b1111;  // We keep the last 4 bits only

    this->checksum = decoded[1] & 0b1111;
    this->encKey = decoded[0];
    // Pull in the 80-bit commands.  The upper nibble will be 0 even on 80 bit packets.
    this->cmd = (somfy_commands)((decoded[1] >> 4));
    // Pull in the data for an 80-bit step command.
    if(this->cmd == somfy_commands::StepDown)
      this->cmd = (somfy_commands)((decoded[1] >> 4) | ((decoded[8] & 0x08) << 4));
    this->rollingCode = decoded[3] + (decoded[2] << 8);
    this->remoteAddress = (decoded[6] + (decoded[5] << 8) + (decoded[4] << 16));
    this->valid = this->checksum == checksum && this->remoteAddress < 16777215 && this->rollingCode > 0;
    if (this->valid) {
        // Check for valid command.
        switch (this->cmd) {
        case somfy_commands::Unknown0:
        case somfy_commands::My:
        case somfy_commands::Up:
        case somfy_commands::MyUp:
        case somfy_commands::Down:
        case somfy_commands::MyDown:
        case somfy_commands::UpDown:
        case somfy_commands::MyUpDown:
        case somfy_commands::Prog:
        case somfy_commands::SunFlag:
        case somfy_commands::Flag:
        case somfy_commands::UnknownC:
        case somfy_commands::UnknownD:
        case somfy_commands::UnknownE:
        case somfy_commands::UnknownF:
            break;
        case somfy_commands::StepUp:
        case somfy_commands::StepDown:
            // These must be 80 bit commands
            break;
        default:
            this->valid = false;
            break;
        }
    }

    if (this->valid) {
        Serial.print("KEY:");
        Serial.print(this->encKey);
        Serial.print(" ADDR:");
        Serial.print(this->remoteAddress);
        Serial.print(" CMD:");
        Serial.print(translateSomfyCommand(this->cmd));
        Serial.print(" RCODE:");
        Serial.print(this->rollingCode);
        Serial.print(" BITS:");
        Serial.print(this->bitLength);
        Serial.print(" HWSYNC:");
        Serial.println(this->hwsync);
    }
    else {
        Serial.print("INVALID FRAME ");
        Serial.print("KEY:");
        Serial.print(this->encKey);
        Serial.print(" ADDR:");
        Serial.print(this->remoteAddress);
        Serial.print(" CMD:");
        Serial.print(translateSomfyCommand(this->cmd));
        Serial.print(" RCODE:");
        Serial.println(this->rollingCode);
        Serial.println("    KEY  1   2   3   4   5   6  ");
        Serial.println("--------------------------------");
        Serial.print("ENC ");
        for (byte i = 0; i < 10; i++) {
            if (frame[i] < 10)
                Serial.print("  ");
            else if (frame[i] < 100)
                Serial.print(" ");
            Serial.print(frame[i]);
            Serial.print(" ");
        }
        Serial.println();
        Serial.print("DEC ");
        for (byte i = 0; i < 10; i++) {
            if (decoded[i] < 10)
                Serial.print("  ");
            else if (decoded[i] < 100)
                Serial.print(" ");
            Serial.print(decoded[i]);
            Serial.print(" ");
        }
        Serial.println();
    }
}
void somfy_frame_t::decodeFrame(somfy_rx_t *rx) {
  this->hwsync = rx->cpt_synchro_hw;
  this->pulseCount = rx->pulseCount;
  this->bitLength = rx->bit_length;
  this->rssi = ELECHOUSE_cc1101.getRssi();
  this->decodeFrame(rx->payload);
}
void somfy_frame_t::encodeFrame(byte *frame) { 
  const byte btn = static_cast<byte>(cmd);
  frame[0] = this->encKey;              // Encryption key. Doesn't matter much
  frame[1] = (btn & 0x0F) << 4;         // Which button did you press? The 4 LSB will be the checksum
  frame[2] = this->rollingCode >> 8;    // Rolling code (big endian)
  frame[3] = this->rollingCode;         // Rolling code
  frame[4] = this->remoteAddress >> 16; // Remote address
  frame[5] = this->remoteAddress >> 8;  // Remote address
  frame[6] = this->remoteAddress;       // Remote address
  frame[7] = 132;
  frame[8] = 0;
  frame[9] = 29;
  switch(this->cmd) {
    case somfy_commands::StepUp:
      frame[7] = 132;
      frame[8] = 56;
      frame[9] = 22;
      break;
    case somfy_commands::StepDown:
      frame[7] = 132;
      frame[8] = 48;
      frame[9] = 30;
      break;
    case somfy_commands::Prog:
      frame[7] = 196;
      frame[8] = 0;
      frame[9] = 25;
      break;
  }
  byte checksum = 0;
 
  for (byte i = 0; i < 7; i++) {
      checksum = checksum ^ frame[i] ^ (frame[i] >> 4);
  }
  checksum &= 0b1111;  // We keep the last 4 bits only
  // Checksum integration
  frame[1] |= checksum;
  // Obfuscation: a XOR of all the bytes
  for (byte i = 1; i < 7; i++) {
      frame[i] ^= frame[i - 1];
  }
}
void somfy_frame_t::print() {
    Serial.println("----------- Receiving -------------");
    Serial.print("RSSI:");
    Serial.print(this->rssi);
    Serial.print(" LQI:");
    Serial.println(this->lqi);
    Serial.print("CMD:");
    Serial.print(translateSomfyCommand(this->cmd));
    Serial.print(" ADDR:");
    Serial.print(this->remoteAddress);
    Serial.print(" RCODE:");
    Serial.println(this->rollingCode);
    Serial.print("KEY:");
    Serial.print(this->encKey, HEX);
    Serial.print(" CS:");
    Serial.println(this->checksum);
}
bool somfy_frame_t::isRepeat(somfy_frame_t &frame) { return this->remoteAddress == frame.remoteAddress && this->cmd == frame.cmd && this->rollingCode == frame.rollingCode; }
void somfy_frame_t::copy(somfy_frame_t &frame) {
  if(this->isRepeat(frame)) {
    this->repeats++;
    this->rssi = frame.rssi;
    this->lqi = frame.lqi;
  }
  else {
    this->valid = frame.valid;
    this->processed = frame.processed;
    this->rssi = frame.rssi;
    this->lqi = frame.lqi;
    this->cmd = frame.cmd;
    this->remoteAddress = frame.remoteAddress;
    this->rollingCode = frame.rollingCode;
    this->encKey = frame.encKey;
    this->checksum = frame.checksum;
    this->hwsync = frame.hwsync;
    this->repeats = frame.repeats;
  }
}
void SomfyShadeController::end() { this->transceiver.disableReceive(); }
SomfyShadeController::SomfyShadeController() {
  memset(this->m_shadeIds, 255, sizeof(this->m_shadeIds));
  uint64_t mac = ESP.getEfuseMac();
  this->startingAddress = mac & 0x0FFFFF;
}
SomfyShade *SomfyShadeController::findShadeByRemoteAddress(uint32_t address) {
  for(uint8_t i = 0; i < SOMFY_MAX_SHADES; i++) {
    SomfyShade &shade = this->shades[i];
    if(shade.getRemoteAddress() == address) return &shade;
    else {
      for(uint8_t j = 0; j < SOMFY_MAX_LINKED_REMOTES; j++) {
        if(shade.linkedRemotes[j].getRemoteAddress() == address) return &shade;
      }
    }
  }
  return nullptr;
}
bool SomfyShadeController::loadLegacy() {
  Serial.println("Loading Legacy shades using NVS");
  pref.begin("Shades", true);
  pref.getBytes("shadeIds", this->m_shadeIds, sizeof(this->m_shadeIds));
  pref.end();
  for(uint8_t i = 0; i < sizeof(this->m_shadeIds); i++) {
    if(i != 0) DEBUG_SOMFY.print(",");
    DEBUG_SOMFY.print(this->m_shadeIds[i]);
  }
  DEBUG_SOMFY.println();
  sortArray<uint8_t>(this->m_shadeIds, sizeof(this->m_shadeIds));
  #ifdef DEBUG_SOMFY
  for(uint8_t i = 0; i < sizeof(this->m_shadeIds); i++) {
    if(i != 0) DEBUG_SOMFY.print(",");
    DEBUG_SOMFY.print(this->m_shadeIds[i]);
  }
  DEBUG_SOMFY.println();
  #endif

  uint8_t id = 0;
  for(uint8_t i = 0; i < sizeof(this->m_shadeIds); i++) {
    if(this->m_shadeIds[i] == id) this->m_shadeIds[i] = 255;
    id = this->m_shadeIds[i];
    SomfyShade *shade = &this->shades[i];
    shade->setShadeId(id);
    if(id == 255) {
      continue;
    }
    shade->load();
  }
  #ifdef DEBUG_SOMFY
  for(uint8_t i = 0; i < SOMFY_MAX_SHADES; i++) {
    DEBUG_SOMFY.print(this->shades[i].getShadeId());
    DEBUG_SOMFY.print(":");
    DEBUG_SOMFY.print(this->m_shadeIds[i]);
    if(i < SOMFY_MAX_SHADES - 1) DEBUG_SOMFY.print(",");
  }
  Serial.println();
  #endif
  if(!this->useNVS()) {
    pref.begin("Shades");
    pref.putBytes("shadeIds", this->m_shadeIds, sizeof(this->m_shadeIds));
    pref.end();
  }
  this->commit();
  return true;
}
bool SomfyShadeController::begin() {
  // Load up all the configuration data.
  ShadeConfigFile::getAppVersion(this->appVersion);
  Serial.printf("App Version:%u.%u.%u\n", this->appVersion.major, this->appVersion.minor, this->appVersion.build);
  if(!this->useNVS()) {  // At 1.4 we started using the configuration file.  If the file doesn't exist then booh.
    // We need to remove all the extraeneous data from NVS for the shades.  From here on out we
    // will rely on the shade configuration.
    Serial.println("No longer using NVS");
    if(ShadeConfigFile::exists()) {
      ShadeConfigFile::load(this);
    }
    else {
      this->loadLegacy();
    }
    pref.begin("Shades");
    if(pref.isKey("shadeIds")) {
      pref.getBytes("shadeIds", this->m_shadeIds, sizeof(this->m_shadeIds));
      pref.clear(); // Delete all the keys.
    }
    pref.end();
    for(uint8_t i = 0; i < sizeof(this->m_shadeIds); i++) {
      // Start deleting the keys for the shades.
      if(this->m_shadeIds[i] == 255) continue;
      char shadeKey[15];
      sprintf(shadeKey, "SomfyShade%u", this->m_shadeIds[i]);
      pref.begin(shadeKey);
      pref.clear();
      pref.end();
    }
  }
  else if(ShadeConfigFile::exists()) {
    Serial.println("shades.cfg exists so we are using that");
    ShadeConfigFile::load(this);
  }
  else {
    Serial.println("Starting clean");
    this->loadLegacy();
  }
  this->transceiver.begin();

  // Set the radio type for shades that have yet to be specified.
  bool saveFlag = false;
  for(uint8_t i = 0; i < SOMFY_MAX_SHADES; i++) {
    SomfyShade *shade = &this->shades[i];
    if(shade->getShadeId() != 255 && shade->bitLength == 0) {
      Serial.printf("Setting bit length to %d\n", this->transceiver.config.type);
      shade->bitLength = this->transceiver.config.type;
      saveFlag = true;
    }
  }
  if(saveFlag) somfy.commit();
  return true;
}
void SomfyShadeController::commit() {
  ShadeConfigFile file;
  file.begin();
  file.save(this);
  file.end();
  this->isDirty = false;
  this->lastCommit = millis();
}
SomfyShade * SomfyShadeController::getShadeById(uint8_t shadeId) {
  for(uint8_t i = 0; i < SOMFY_MAX_SHADES; i++) {
    if(this->shades[i].getShadeId() == shadeId) return &this->shades[i];
  }
  return nullptr;
}
bool SomfyShade::linkRemote(uint32_t address, uint16_t rollingCode) {
  // Check to see if the remote is already linked. If it is
  // just return true after setting the rolling code
  for(uint8_t i = 0; i < SOMFY_MAX_LINKED_REMOTES; i++) {
    if(this->linkedRemotes[i].getRemoteAddress() == address) {
      this->linkedRemotes[i].setRollingCode(rollingCode);
      return true;
    }
  }
  for(uint8_t i = 0; i < SOMFY_MAX_LINKED_REMOTES; i++) {
    if(this->linkedRemotes[i].getRemoteAddress() == 0) {
      this->linkedRemotes[i].setRemoteAddress(address);
      this->linkedRemotes[i].setRollingCode(rollingCode);
      if(somfy.useNVS()) {
        uint32_t linkedAddresses[SOMFY_MAX_LINKED_REMOTES];
        memset(linkedAddresses, 0x00, sizeof(linkedAddresses));
        uint8_t j = 0;
        for(uint8_t i = 0; i < SOMFY_MAX_LINKED_REMOTES; i++) {
          SomfyLinkedRemote lremote = this->linkedRemotes[i];
          if(lremote.getRemoteAddress() != 0) linkedAddresses[j++] = lremote.getRemoteAddress();
        }
        char shadeKey[15];
        snprintf(shadeKey, sizeof(shadeKey), "SomfyShade%u", this->getShadeId());
        pref.begin(shadeKey);
        pref.putBytes("linkedAddr", linkedAddresses, sizeof(uint32_t) * SOMFY_MAX_LINKED_REMOTES);
        pref.end();
      }
      this->commit();
      return true;
    }
  }
  return false;
}
void SomfyShade::commit() { somfy.commit(); }
void SomfyShade::commitShadePosition() {
  somfy.isDirty = true;
  char shadeKey[15];
  if(somfy.useNVS()) {
    snprintf(shadeKey, sizeof(shadeKey), "SomfyShade%u", this->shadeId);
    Serial.print("Writing current shade position: ");
    Serial.println(this->currentPos, 4);
    pref.begin(shadeKey);
    pref.putFloat("currentPos", this->currentPos);
    pref.end();
  }
}
void SomfyShade::commitMyPosition() {
  somfy.isDirty = true;
  if(somfy.useNVS()) {
    char shadeKey[15];
    snprintf(shadeKey, sizeof(shadeKey), "SomfyShade%u", this->shadeId);
    Serial.print("Writing my shade position:");
    Serial.print(this->myPos);
    Serial.println("%");
    pref.begin(shadeKey);
    pref.putUShort("myPos", this->myPos);
    pref.end();
  }
}
void SomfyShade::commitTiltPosition() {
  somfy.isDirty = true;
  if(somfy.useNVS()) {
    char shadeKey[15];
    snprintf(shadeKey, sizeof(shadeKey), "SomfyShade%u", this->shadeId);
    Serial.print("Writing current shade tilt position: ");
    Serial.println(this->currentTiltPos, 4);
    pref.begin(shadeKey);
    pref.putFloat("currentTiltPos", this->currentTiltPos);
    pref.end();
  }
}
bool SomfyShade::unlinkRemote(uint32_t address) {
  for(uint8_t i = 0; i < SOMFY_MAX_LINKED_REMOTES; i++) {
    if(this->linkedRemotes[i].getRemoteAddress() == address) {
      this->linkedRemotes[i].setRemoteAddress(0);
      if(somfy.useNVS()) {
        char shadeKey[15];
        snprintf(shadeKey, sizeof(shadeKey), "SomfyShade%u", this->getShadeId());
        uint32_t linkedAddresses[SOMFY_MAX_LINKED_REMOTES];
        memset(linkedAddresses, 0x00, sizeof(linkedAddresses));
        uint8_t j = 0;
        for(uint8_t i = 0; i < SOMFY_MAX_LINKED_REMOTES; i++) {
          SomfyLinkedRemote lremote = this->linkedRemotes[i];
          if(lremote.getRemoteAddress() != 0) linkedAddresses[j++] = lremote.getRemoteAddress();
        }
        pref.begin(shadeKey);
        pref.putBytes("linkedAddr", linkedAddresses, sizeof(uint32_t) * SOMFY_MAX_LINKED_REMOTES);
        pref.end();
      }
      this->commit();
      return true;
    }
  }
  return false;
}
void SomfyShade::checkMovement() {
  int8_t currDir = this->direction;
  uint8_t currPos = this->position;
  int8_t currTiltDir = this->tiltDirection;
  uint8_t currTiltPos = this->tiltPosition;
  
  if(this->direction > 0) {
    if(this->downTime == 0) {
      this->direction = 0;
      this->currentPos = 1;
    }
    else {
      // The shade is moving down so we need to calculate its position through the down position.
      // 10000ms from 0 to 100
      // The starting posion is a float value from 0-1 that indicates how much the shade is open. So
      // if we take the starting position * the total down time then this will tell us how many ms it
      // has moved in the down position.
      int32_t msFrom0 = (int32_t)floor(this->startPos * this->downTime);
      
      // So if the start position is .1 it is 10% closed so we have a 1000ms (1sec) of time to account for
      // before we add any more time.
      msFrom0 += (millis() - this->moveStart);
      // Now we should have the total number of ms that the shade moved from the top.  But just so we
      // don't have any rounding errors make sure that it is not greater than the max down time.
      msFrom0 = min((int32_t)this->downTime, msFrom0);
      if(msFrom0 >= this->downTime) {
        this->currentPos = 1.0;
        this->direction = 0;        
      }
      else {
        // So now we know how much time has elapsed from the 0 position to down.  The current position should be
        // a ratio of how much time has travelled over the total time to go 100%.
  
        // We should now have the number of ms it will take to reach the shade fully close.
        this->currentPos = min(max((float)0.0, (float)msFrom0 / (float)this->downTime), (float)1.0);
        // If the current position is >= 1 then we are at the bottom of the shade.
        if(this->currentPos >= 1) {
          this->direction = 0;
          this->currentPos = 1.0;
        }
      }
      this->position = floor(this->currentPos * 100);
      if(this->seekingPos && this->position >= this->target) {
        Serial.print("Stopping Shade:");
        Serial.print(this->name);
        Serial.print(" at ");
        Serial.print(this->position);
        Serial.print("% target ");
        Serial.print(this->target);
        Serial.println("%");
        if(!this->seekingFixedPos) this->sendCommand(somfy_commands::My);
        else this->direction = 0;
        this->seekingPos = false;
        this->seekingFixedPos = false;
      }
    }
  }
  else if(this->direction < 0) {
    if(this->upTime == 0) {
      this->direction = 0;
      this->currentPos = 0;
    }
    else {
      // The shade is moving up so we need to calculate its position through the up position. Shades
      // often move slower in the up position so since we are using a relative position the up time
      // can be calculated.
      // 10000ms from 100 to 0;
      int32_t msFrom100 = (int32_t)this->upTime - (int32_t)floor(this->startPos * this->upTime);
      msFrom100 += (millis() - this->moveStart);
      msFrom100 = min((int32_t)this->upTime, msFrom100);
      if(msFrom100 >= this->upTime) {
        this->currentPos = 0.0;
        this->direction = 0;
      }
      // We should now have the number of ms it will take to reach the shade fully open.
      this->currentPos = (float)1.0 - min(max((float)0.0, (float)msFrom100 / (float)this->upTime), (float)1.0);
      // If we are at the top of the shade then set the movement to 0.
      if(this->currentPos <= 0.0) {
        this->direction = 0;
        this->currentPos = 0;
      }
    }
    this->position = floor(this->currentPos * 100);
    if(this->seekingPos && this->position <= this->target) {
      Serial.print("Stopping Shade:");
      Serial.print(this->name);
      Serial.print(" at ");
      Serial.print(this->position);
      Serial.print("% target ");
      Serial.print(this->target);
      Serial.println("%");
      if(!this->seekingFixedPos) this->sendCommand(somfy_commands::My);
      else this->direction = 0;
      this->seekingFixedPos = false;
      this->seekingPos = false;
    }
  }
  if(this->tiltDirection > 0) {
    int32_t msFrom0 = (int32_t)floor(this->startTiltPos * this->tiltTime);
    msFrom0 += (millis() - this->tiltStart);
    msFrom0 = min((int32_t)this->tiltTime, msFrom0);
    if(msFrom0 >= this->tiltTime) {
      this->currentTiltPos = 1.0;
      this->tiltDirection = 0;        
    }
    else {
      this->currentTiltPos = min(max((float)0.0, (float)msFrom0 / (float)this->tiltTime), (float)1.0);
      if(this->currentTiltPos >= 1) {
        this->tiltDirection = 0;
        this->currentTiltPos = 1.0;
      }
    }
    this->tiltPosition = floor(this->currentTiltPos * 100);
    if(this->seekingTiltPos && this->tiltPosition >= this->tiltTarget) {
      Serial.print("Stopping Shade Tilt:");
      Serial.print(this->name);
      Serial.print(" at ");
      Serial.print(this->tiltPosition);
      Serial.print("% target ");
      Serial.print(this->tiltTarget);
      Serial.println("%");
      this->sendCommand(somfy_commands::My);
      this->tiltDirection = 0;
      this->seekingTiltPos = false;
    }
  }
  else if(this->tiltDirection < 0) {
    if(this->tiltTime == 0) {
      this->tiltDirection = 0;
      this->currentTiltPos = 0;
    }
    else {
      int32_t msFrom100 = (int32_t)this->tiltTime - (int32_t)floor(this->startTiltPos * this->tiltTime);
      msFrom100 += (millis() - this->tiltStart);
      msFrom100 = min((int32_t)this->tiltTime, msFrom100);
      if(msFrom100 >= this->tiltTime) {
        this->currentTiltPos = 0.0;
        this->tiltDirection = 0;
      }
      this->currentTiltPos = (float)1.0 - min(max((float)0.0, (float)msFrom100 / (float)this->tiltTime), (float)1.0);
      // If we are at the top of the shade then set the movement to 0.
      if(this->currentTiltPos <= 0.0) {
        this->tiltDirection = 0;
        this->currentTiltPos = 0;
      }
    }
    this->tiltPosition = floor(this->currentTiltPos * 100);
    if(this->seekingTiltPos && this->tiltPosition <= this->tiltTarget) {
      Serial.print("Stopping Shade Tilt:");
      Serial.print(this->name);
      Serial.print(" at ");
      Serial.print(this->tiltPosition);
      Serial.print("% target ");
      Serial.print(this->tiltTarget);
      Serial.println("%");
      this->sendCommand(somfy_commands::My);
      this->tiltDirection = 0;
      this->seekingTiltPos = false;
    }
  }
  if(currDir != this->direction && this->direction == 0) {
    this->commitShadePosition();
    if(this->settingMyPos) {
      delay(200);
      // Set this position before sending the command.  If you don't the processFrame function
      // will send the shade back to its original My position.
      if(this->myPos == this->position) this->myPos = 255;
      else this->myPos = this->position;
      SomfyRemote::sendCommand(somfy_commands::My, SETMY_REPEATS);
      this->settingMyPos = false;
      this->seekingFixedPos = false;
      this->commitMyPosition();
    }
  }
  if(currTiltDir != this->tiltDirection && this->tiltDirection == 0) {
    this->commitTiltPosition();
  }
  if(currDir != this->direction || currPos != this->position || currTiltDir != this->tiltDirection || currTiltPos != this->tiltPosition) {
    // We need to emit on the socket that our state has changed.
    this->position = floor(this->currentPos * 100.0);
    this->tiltPosition = floor(this->currentTiltPos * 100.0);
    this->emitState();
  }
  
}
void SomfyShade::load() {
    char shadeKey[15];
    uint32_t linkedAddresses[SOMFY_MAX_LINKED_REMOTES];
    memset(linkedAddresses, 0x00, sizeof(uint32_t) * SOMFY_MAX_LINKED_REMOTES);
    snprintf(shadeKey, sizeof(shadeKey), "SomfyShade%u", this->shadeId);
    // Now load up each of the shades into memory.
    //Serial.print("key:");
    //Serial.println(shadeKey);
    
    pref.begin(shadeKey, !somfy.useNVS());
    pref.getString("name", this->name, sizeof(this->name));
    this->paired = pref.getBool("paired", false);
    if(pref.isKey("upTime") && pref.getType("upTime") != PreferenceType::PT_U32) {
      // We need to convert these to 32 bits because earlier versions did not support this.
      this->upTime = static_cast<uint32_t>(pref.getUShort("upTime", 1000));
      this->downTime = static_cast<uint32_t>(pref.getUShort("downTime", 1000));
      this->tiltTime = static_cast<uint32_t>(pref.getUShort("tiltTime", 7000));
      if(somfy.useNVS()) {
        pref.remove("upTime");
        pref.putUInt("upTime", this->upTime);
        pref.remove("downTime");
        pref.putUInt("downTime", this->downTime);
        pref.remove("tiltTime");
        pref.putUInt("tiltTime", this->tiltTime);
      }
    }
    else {
      this->upTime = pref.getUInt("upTime", this->upTime);
      this->downTime = pref.getUInt("downTime", this->downTime);
      this->tiltTime = pref.getUInt("tiltTime", this->tiltTime);
    }
    this->setRemoteAddress(pref.getUInt("remoteAddress", 0));
    this->currentPos = pref.getFloat("currentPos", 0);
    this->position = (uint8_t)floor(this->currentPos * 100);
    this->target = this->position;
    this->myPos = pref.getUShort("myPos", this->myPos);
    this->hasTilt = pref.getBool("hasTilt", false);
    this->shadeType = static_cast<shade_types>(pref.getChar("shadeType", static_cast<uint8_t>(this->shadeType)));
    this->currentTiltPos = pref.getFloat("currentTiltPos", 0);
    this->tiltPosition = (uint8_t)floor(this->currentTiltPos * 100);
    this->tiltTarget = this->tiltPosition;
    pref.getBytes("linkedAddr", linkedAddresses, sizeof(linkedAddresses));
    pref.end();
    Serial.print("shadeId:");
    Serial.print(this->getShadeId());
    Serial.print(" name:");
    Serial.print(this->name);
    Serial.print(" address:");
    Serial.print(this->getRemoteAddress());
    Serial.print(" position:");
    Serial.print(this->position);
    Serial.print(" myPos:");
    Serial.println(this->myPos);
    pref.begin("ShadeCodes");
    this->lastRollingCode = pref.getUShort(this->m_remotePrefId, 0);
    for(uint8_t j = 0; j < SOMFY_MAX_LINKED_REMOTES; j++) {
      SomfyLinkedRemote &lremote = this->linkedRemotes[j];
      lremote.setRemoteAddress(linkedAddresses[j]);
      lremote.lastRollingCode = pref.getUShort(lremote.getRemotePrefId(), 0);
    }
    pref.end();
}
void SomfyShade::publish() {
  if(mqtt.connected()) {
    char topic[32];
    snprintf(topic, sizeof(topic), "shades/%u/shadeId", this->shadeId);
    mqtt.publish(topic, this->shadeId);
    snprintf(topic, sizeof(topic), "shades/%u/name", this->shadeId);
    mqtt.publish(topic, this->name);
    snprintf(topic, sizeof(topic), "shades/%u/remoteAddress", this->shadeId);
    mqtt.publish(topic, this->getRemoteAddress());
    snprintf(topic, sizeof(topic), "shades/%u/position", this->shadeId);
    mqtt.publish(topic, this->position);
    snprintf(topic, sizeof(topic), "shades/%u/direction", this->shadeId);
    mqtt.publish(topic, this->direction);
    snprintf(topic, sizeof(topic), "shades/%u/target", this->shadeId);
    mqtt.publish(topic, this->target);
    snprintf(topic, sizeof(topic), "shades/%u/lastRollingCode", this->shadeId);
    mqtt.publish(topic, this->lastRollingCode);
    snprintf(topic, sizeof(topic), "shades/%u/mypos", this->shadeId);
    mqtt.publish(topic, this->hasTilt ? "true" : "false");
    snprintf(topic, sizeof(topic), "shades/%u/shadeType", this->shadeId);
    mqtt.publish(topic, static_cast<uint8_t>(this->shadeType));
    if(this->hasTilt) {
      snprintf(topic, sizeof(topic), "shades/%u/tiltDirection", this->shadeId);
      mqtt.publish(topic, this->tiltDirection);
      snprintf(topic, sizeof(topic), "shades/%u/tiltPosition", this->shadeId);
      mqtt.publish(topic, this->tiltPosition);
      snprintf(topic, sizeof(topic), "shades/%u/tiltTarget", this->shadeId);
      mqtt.publish(topic, this->tiltTarget);
    }
  }
}
void SomfyShade::emitState(const char *evt) { this->emitState(255, evt); }
void SomfyShade::emitState(uint8_t num, const char *evt) {
  char buf[320];
  if(this->hasTilt)
    snprintf(buf, sizeof(buf), "{\"shadeId\":%d,\"type\":%u,\"remoteAddress\":%d,\"name\":\"%s\",\"direction\":%d,\"position\":%d,\"target\":%d,\"mypos\":%d,\"hasTilt\":%s,\"tiltDirection\":%d,\"tiltTarget\":%d,\"tiltPosition\":%d}", 
      this->shadeId, static_cast<uint8_t>(this->shadeType), this->getRemoteAddress(), this->name, this->direction, this->position, this->target, this->myPos, this->hasTilt ? "true" : "false", this->tiltDirection, this->tiltTarget, this->tiltPosition);
  else
    snprintf(buf, sizeof(buf), "{\"shadeId\":%d,\"type\":%u,\"remoteAddress\":%d,\"name\":\"%s\",\"direction\":%d,\"position\":%d,\"target\":%d,\"mypos\":%d,\"hasTilt\":%s}", 
      this->shadeId, static_cast<uint8_t>(this->shadeType), this->getRemoteAddress(), this->name, this->direction, this->position, this->target, this->myPos, this->hasTilt ? "true" : "false");
  if(num >= 255) sockEmit.sendToClients(evt, buf);
  else sockEmit.sendToClient(num, evt, buf);
  if(mqtt.connected()) {
    char topic[32];
    snprintf(topic, sizeof(topic), "shades/%u/position", this->shadeId);
    mqtt.publish(topic, this->position);
    snprintf(topic, sizeof(topic), "shades/%u/direction", this->shadeId);
    mqtt.publish(topic, this->direction);
    snprintf(topic, sizeof(topic), "shades/%u/target", this->shadeId);
    mqtt.publish(topic, this->target);
    snprintf(topic, sizeof(topic), "shades/%u/lastRollingCode", this->shadeId);
    mqtt.publish(topic, this->lastRollingCode);
    snprintf(topic, sizeof(topic), "shades/%u/mypos", this->shadeId);
    mqtt.publish(topic, this->myPos);
    snprintf(topic, sizeof(topic), "shades/%u/hasTilt", this->hasTilt);
    mqtt.publish(topic, this->hasTilt ? "true" : "false");
    if(this->hasTilt) {
      snprintf(topic, sizeof(topic), "shades/%u/tiltPosition", this->shadeId);
      mqtt.publish(topic, this->tiltPosition);
      snprintf(topic, sizeof(topic), "shades/%u/tiltTarget", this->shadeId);
      mqtt.publish(topic, this->tiltTarget);
    }
  }
}
bool SomfyShade::isIdle() { return this->direction == 0 && this->tiltDirection == 0; }
void SomfyShade::processWaitingFrame() {
  if(this->shadeId == 255) {
    this->lastFrame.await = 0; 
    return;
  }
  if(this->lastFrame.processed) return;
  if(this->lastFrame.await > 0 && (millis() > this->lastFrame.await)) {
    switch(this->lastFrame.cmd) {
      case somfy_commands::StepUp:
          this->lastFrame.processed = true;
          // Simply move the shade up by 1%.
          if(this->position > 0) {
            this->seekingFixedPos = true;
            this->seekingPos = true;
            this->target = this->position - 1;
            this->setMovement(-1);
          }
          break;
      case somfy_commands::StepDown:
          this->lastFrame.processed = true;
          // Simply move the shade down by 1%.
          if(this->position < 100) {
            this->seekingFixedPos = true;
            this->seekingPos = true;
            this->target = this->position + 1;
            this->setMovement(1);
          }
          break;
      case somfy_commands::Down:
      case somfy_commands::Up:
        if(this->hasTilt) { // Theoretically this should get here unless it does have a tilt.
          if(this->lastFrame.repeats >= TILT_REPEATS) {
            int8_t dir = this->lastFrame.cmd == somfy_commands::Up ? -1 : 1;
            this->seekingTiltPos = false;
            this->tiltTarget = dir > 0 ? 100 : 0;
            this->setTiltMovement(dir);
            this->lastFrame.processed = true;
            Serial.print(this->name);
            Serial.print(" Processing tilt ");
            Serial.print(translateSomfyCommand(this->lastFrame.cmd));
            Serial.print(" after ");
            Serial.print(this->lastFrame.repeats);
            Serial.println(" repeats");
          }
          else {
            int8_t dir = this->lastFrame.cmd == somfy_commands::Up ? -1 : 1;
            this->seekingPos = false;
            this->target = dir > 0 ? 100 : 0;
            this->setMovement(dir);
            this->lastFrame.processed = true;
          }
          if(this->lastFrame.repeats > TILT_REPEATS + 2) this->lastFrame.processed = true;
        }
        break;
      case somfy_commands::My:
        if(this->lastFrame.repeats >= SETMY_REPEATS && this->isIdle()) {
          if(this->myPos == this->position) // We are clearing it.
            this->myPos = 255;
          else
            this->myPos = this->position;
          this->commitMyPosition();
          this->lastFrame.processed = true;
          this->emitState();
        }
        else if(this->myPos >= 0 && this->myPos <= 100) {
          int8_t dir = 0;
          if(myPos < this->position) dir = -1;
          else if(myPos > this->position) dir = 1;
          if(dir != 0) this->seekingFixedPos = true;
          this->seekingPos = true;
          this->target = this->myPos;
          this->setMovement(dir);
          this->lastFrame.processed = true;
        }
        if(this->lastFrame.repeats > SETMY_REPEATS + 2) this->lastFrame.processed = true;
        if(this->lastFrame.processed) {
          Serial.print(this->name);
          Serial.print(" Processing MY after ");
          Serial.print(this->lastFrame.repeats);
          Serial.println(" repeats");
        }
        break;
    }
  }
}
void SomfyShade::processFrame(somfy_frame_t &frame, bool internal) {
  if(this->shadeId == 255) return; 
  bool hasRemote = this->getRemoteAddress() == frame.remoteAddress;
  if(!hasRemote) {
    for(uint8_t i = 0; i < SOMFY_MAX_LINKED_REMOTES; i++) {
      if(this->linkedRemotes[i].getRemoteAddress() == frame.remoteAddress) {
        this->linkedRemotes[i].setRollingCode(frame.rollingCode);
        hasRemote = true;
        break;      
      }
    }
  }
  if(!hasRemote) return;
  this->lastFrame.copy(frame);
  int8_t dir = 0;
  // If the frame came from the radio it cannot be seeking a position.  This means that the target will be set.
  if(!internal) this->seekingTiltPos = this->seekingPos = false;
 
  // At this point we are not processing the combo buttons
  // will need to see what the shade does when you press both.
  switch(frame.cmd) {
    case somfy_commands::Up:
      if(this->hasTilt) {
        // Wait another half seccond just in case we are potentially processing a tilt.
        if(!internal) this->lastFrame.await = millis() + 500;
        else if(this->lastFrame.repeats >= TILT_REPEATS) {
          // This is an internal tilt command.
          this->setTiltMovement(-1);
          this->lastFrame.processed = true;
          return;
        }
        else {
          dir = -1;
          if(!internal) this->target = 0;
          this->lastFrame.processed = true;
        }
      }
      else {
        dir = -1;
        if(!internal) this->target = 0;
        this->lastFrame.processed = true;
      }
      break;
    case somfy_commands::Down:
      if(this->hasTilt) {
        // Wait another half seccond just in case we are potentially processing a tilt.
        if(!internal) this->lastFrame.await = millis() + 500;
        else if(this->lastFrame.repeats >= TILT_REPEATS) {
          // This is an internal tilt command.
          this->setTiltMovement(1);
          this->lastFrame.processed = true;
          return;
        }
        else {
          dir = 1;
          if(!internal) this->target = 100;
          this->lastFrame.processed = true;
        }
      }
      else {
        dir = 1;
        if(!internal) this->target = 100;
        this->lastFrame.processed = true;
      }
      break;
    case somfy_commands::My:
      dir = 0;
      if(this->isIdle()) {
        if(!internal) {
          // This frame is coming from a remote. We are potentially setting
          // the my position.
          this->lastFrame.await = millis() + 500;
        }
        else if(myPos >= 0 && this->myPos <= 100) {
          this->lastFrame.processed = true;
          // In this instance we will be moving to the my position.  This
          // will be up or down or not.
          if(this->myPos < this->position) dir = -1;
          else if(this->myPos > this->position) dir = 1;
          if(dir != 0) {
            Serial.println("Start moving to My Position");
            this->seekingFixedPos = true;
          }
          this->seekingPos = true;
          this->target = this->myPos;
        }
      }
      else
        // This will occur if the shade needs to be stopped or there
        // is no my position set.
        this->lastFrame.processed = true;
      break;
    case somfy_commands::StepUp:
      if(!internal) {
        this->lastFrame.await = millis() + 200;
      }
      else {
        this->lastFrame.processed = true;
        // Simply move the shade up by 1%.
        if(this->position > 0) {
          this->seekingFixedPos = true;
          this->seekingPos = true;
          this->target = this->position - 1;
          dir = -1;
        }
      }
      break;
    case somfy_commands::StepDown:
      if(!internal) {
        this->lastFrame.await = millis() + 200;
      }
      else {
        this->lastFrame.processed = true;
        // Simply move the shade down by 1%.
        if(this->position < 100) {
          this->seekingFixedPos = true;
          this->seekingPos = true;
          this->target = this->position + 1;
          dir = 1;
        }
      }
      break;
    default:
      dir = 0;
      break;
  }
  if(dir == 0 && this->hasTilt && this->tiltDirection != 0) this->setTiltMovement(0);
  this->setMovement(dir);
}
void SomfyShade::setTiltMovement(int8_t dir) {
  int8_t currDir = this->tiltDirection;
  if(dir == 0) {
    // The shade tilt is stopped.
    this->startTiltPos = this->currentTiltPos;
    this->tiltStart = 0;
    this->tiltDirection = dir;
    if(currDir != dir) {
      this->commitTiltPosition();
    }
  }
  else if(this->direction != dir) {
    this->tiltStart = millis();
    this->startTiltPos = this->currentTiltPos;
    this->tiltDirection = dir;
  }
  if(this->tiltDirection != currDir) {
    this->tiltPosition = floor(this->currentTiltPos * 100.0);
    this->emitState();
  }
}
void SomfyShade::setMovement(int8_t dir) {
  int8_t currDir = this->direction;
  if(dir == 0) {
    // The shade is stopped.
    this->startPos = this->currentPos;
    this->moveStart = 0;
    this->direction = dir;
    if(currDir != dir) {
      this->commitShadePosition();
    }
  }
  else if(this->direction != dir) {
    this->moveStart = millis();
    this->startPos = this->currentPos;
    this->direction = dir;
  }
  if(this->direction != currDir) {
    this->position = floor(this->currentPos * 100.0);
    this->emitState();
  }
}
void SomfyShade::setMyPosition(uint8_t target) {
  if(this->direction != 0) return; // Don't do this if it is moving.
  Serial.println("setMyPosition called");
  if(target != this->position) {
    this->settingMyPos = true;
    Serial.println("Moving to set My Position");
    if(target == this->myPos)
      // Setting the My Position to the same position will toggle it off. So lets send a my
      // command instead of an up/down.  This will ensure we get the thing cleared.
      this->moveToMyPosition();
    else
      this->moveToTarget(target);
  }
  else {
    this->sendCommand(somfy_commands::My, SETMY_REPEATS);
    if(target == this->myPos)
      this->myPos = 255;
    else
      this->myPos = target;
    this->commitMyPosition();
    this->emitState();
  }
}
void SomfyShade::moveToMyPosition() {
  if(this->direction != 0 || this->myPos > 100 || this->myPos < 0) return;
  if(this->position == this->myPos) return; // Nothing to see here since we are already here.
  Serial.print("Seeking my Position:");
  Serial.print(this->myPos);
  Serial.println("%");
  this->seekingFixedPos = true;
  this->target = this->myPos;
  Serial.print("Moving to ");
  Serial.print(this->target);
  Serial.print("% from ");
  Serial.print(this->position);
  Serial.print("% using ");
  Serial.println(translateSomfyCommand(somfy_commands::My));
  this->seekingPos = true;
  SomfyRemote::sendCommand(somfy_commands::My);
}
void SomfyShade::sendCommand(somfy_commands cmd, uint8_t repeat) {
  if(this->bitLength == 0) this->bitLength = somfy.transceiver.config.type;
  if(cmd == somfy_commands::Up) {
    this->target = 0;
    this->seekingPos = false;
  }
  else if(cmd == somfy_commands::Down) {
    this->target = 100;
    this->seekingPos = false;
  }
  else if(cmd == somfy_commands::My) {
    if(this->isIdle() && this->myPos >= 0 && this->myPos <= 100) {
      this->moveToMyPosition();      
      return;
    }
    else {
      this->target = this->position;
      this->seekingPos = false;
    }
  }
  SomfyRemote::sendCommand(cmd, repeat);
}
void SomfyShade::sendTiltCommand(somfy_commands cmd) {
  if(cmd == somfy_commands::Up) {
    this->tiltTarget = 0;
    this->seekingTiltPos = false;
    SomfyRemote::sendCommand(cmd, TILT_REPEATS);
  }
  else if(cmd == somfy_commands::Down) {
    this->tiltTarget = 100;
    this->seekingTiltPos = false;
    SomfyRemote::sendCommand(cmd, TILT_REPEATS);
  }
  else if(cmd == somfy_commands::My) {
    this->tiltTarget = this->tiltPosition;
    this->seekingTiltPos = false;
    SomfyRemote::sendCommand(cmd);
  }
}
void SomfyShade::moveToTiltTarget(uint8_t target) {
  int8_t newDir = 0;
  somfy_commands cmd = somfy_commands::My;
  if(target < this->tiltPosition)
    cmd = somfy_commands::Up;
  else if(target > this->tiltPosition)
    cmd = somfy_commands::Down;
  Serial.print("Moving Tilt to ");
  Serial.print(target);
  Serial.print("% from ");
  Serial.print(this->tiltPosition);
  Serial.print("% using ");
  Serial.println(translateSomfyCommand(cmd));
  this->tiltTarget = target;
  if(target > 0 && target < 100) this->seekingTiltPos = true;
  else this->seekingTiltPos = false;
  if(cmd != somfy_commands::My)
    SomfyRemote::sendCommand(cmd, TILT_REPEATS);  
  else
    SomfyRemote::sendCommand(cmd);  
}
void SomfyShade::moveToTarget(uint8_t target) {
  int8_t newDir = 0;
  somfy_commands cmd = somfy_commands::My;
  if(target < this->position)
    cmd = somfy_commands::Up;
  else if(target > this->position)
    cmd = somfy_commands::Down;
  Serial.print("Moving to ");
  Serial.print(target);
  Serial.print("% from ");
  Serial.print(this->position);
  Serial.print("% using ");
  Serial.println(translateSomfyCommand(cmd));
  this->target = target;
  if(target > 0 && target != 100) this->seekingPos = true;
  else this->seekingPos = false;
  SomfyRemote::sendCommand(cmd);  
}
bool SomfyShade::save() {
  if(somfy.useNVS()) {
    char shadeKey[15];
    snprintf(shadeKey, sizeof(shadeKey), "SomfyShade%u", this->getShadeId());
    pref.begin(shadeKey);
    pref.clear();
    pref.putChar("shadeType", static_cast<uint8_t>(this->shadeType));
    pref.putUInt("remoteAddress", this->getRemoteAddress());
    pref.putString("name", this->name);
    pref.putBool("hasTilt", this->hasTilt);
    pref.putBool("paired", this->paired);
    pref.putUInt("upTime", this->upTime);
    pref.putUInt("downTime", this->downTime);
    pref.putUInt("tiltTime", this->tiltTime);
    pref.putFloat("currentPos", this->currentPos);
    pref.putFloat("currentTiltPos", this->currentTiltPos);
    pref.putUShort("myPos", this->myPos);
    uint32_t linkedAddresses[SOMFY_MAX_LINKED_REMOTES];
    memset(linkedAddresses, 0x00, sizeof(linkedAddresses));
    uint8_t j = 0;
    for(uint8_t i = 0; i < SOMFY_MAX_LINKED_REMOTES; i++) {
      SomfyLinkedRemote lremote = this->linkedRemotes[i];
      if(lremote.getRemoteAddress() != 0) linkedAddresses[j++] = lremote.getRemoteAddress();
    }
    pref.remove("linkedAddr");
    pref.putBytes("linkedAddr", linkedAddresses, sizeof(uint32_t) * SOMFY_MAX_LINKED_REMOTES);
    pref.end();
  }
  this->commit();
  return true;
}
bool SomfyShade::fromJSON(JsonObject &obj) {
  if(obj.containsKey("name")) strlcpy(this->name, obj["name"], sizeof(this->name));
  if(obj.containsKey("upTime")) this->upTime = obj["upTime"];
  if(obj.containsKey("downTime")) this->downTime = obj["downTime"];
  if(obj.containsKey("remoteAddress")) this->setRemoteAddress(obj["remoteAddress"]);
  if(obj.containsKey("tiltTime")) this->tiltTime = obj["tiltTime"];
  if(obj.containsKey("hasTilt")) this->hasTilt = obj["hasTilt"];
  if(obj.containsKey("bitLength")) this->bitLength = obj["bitLength"];
  if(obj.containsKey("shadeType")) {
    if(obj["shadeType"].is<const char *>()) {
      if(strncmp(obj["shadeType"].as<const char *>(), "roller", 7) == 0)
        this->shadeType = shade_types::roller;
      else if(strncmp(obj["shadeType"].as<const char *>(), "drapery", 8) == 0)
        this->shadeType = shade_types::drapery;
      else if(strncmp(obj["shadeType"].as<const char *>(), "blind", 5) == 0)
        this->shadeType = shade_types::blind;
    }
    else {
      this->shadeType = static_cast<shade_types>(obj["shadeType"].as<uint8_t>());
    }
  }
  if(obj.containsKey("linkedAddresses")) {
    uint32_t linkedAddresses[SOMFY_MAX_LINKED_REMOTES];
    memset(linkedAddresses, 0x00, sizeof(linkedAddresses));
    JsonArray arr = obj["linkedAddresses"];
    uint8_t i = 0;
    for(uint32_t addr : arr) {
      linkedAddresses[i++] = addr;
    }
    for(uint8_t j = 0; j < SOMFY_MAX_LINKED_REMOTES; j++) {
      this->linkedRemotes[j].setRemoteAddress(linkedAddresses[j]);
    }
  }
  return true;
}
bool SomfyShade::toJSON(JsonObject &obj) {
  //Serial.print("Serializing Shade:");
  //Serial.print(this->getShadeId());
  //Serial.print("  ");
  //Serial.println(this->name);
  obj["shadeId"] = this->getShadeId();
  obj["name"] = this->name;
  obj["remoteAddress"] = this->m_remoteAddress;
  obj["upTime"] = this->upTime;
  obj["downTime"] = this->downTime;
  obj["paired"] = this->paired;
  obj["remotePrefId"] = this->getRemotePrefId();
  obj["lastRollingCode"] = this->lastRollingCode;
  obj["position"] = this->position;
  obj["tiltPosition"] = this->tiltPosition;
  obj["tiltDirection"] = this->tiltDirection;
  obj["tiltTime"] = this->tiltTime;
  obj["tiltTarget"] = this->tiltTarget;
  obj["target"] = this->target;
  obj["myPos"] = this->myPos;
  obj["direction"] = this->direction;
  obj["hasTilt"] = this->hasTilt;
  obj["tiltTime"] = this->tiltTime;
  obj["shadeType"] = static_cast<uint8_t>(this->shadeType);
  obj["bitLength"] = this->bitLength;
  SomfyRemote::toJSON(obj);
  JsonArray arr = obj.createNestedArray("linkedRemotes");
  for(uint8_t i = 0; i < SOMFY_MAX_LINKED_REMOTES; i++) {
    SomfyLinkedRemote &lremote = this->linkedRemotes[i];
    if(lremote.getRemoteAddress() != 0) {
      JsonObject lro = arr.createNestedObject();
      lremote.toJSON(lro);
    }
  }
  return true;
}
bool SomfyRemote::toJSON(JsonObject &obj) {
  obj["remotePrefId"] = this->getRemotePrefId();
  obj["remoteAddress"] = this->getRemoteAddress();
  obj["lastRollingCode"] = this->lastRollingCode;
  return true;  
}
void SomfyRemote::setRemoteAddress(uint32_t address) { this->m_remoteAddress = address; snprintf(this->m_remotePrefId, sizeof(this->m_remotePrefId), "_%lu", (unsigned long)this->m_remoteAddress); }
uint32_t SomfyRemote::getRemoteAddress() { return this->m_remoteAddress; }
void SomfyShadeController::processFrame(somfy_frame_t &frame, bool internal) {
  for(uint8_t i = 0; i < SOMFY_MAX_SHADES; i++)
    if(this->shades[i].getShadeId() != 255) this->shades[i].processFrame(frame, internal);
}
void SomfyShadeController::processWaitingFrame() {
  for(uint8_t i = 0; i < SOMFY_MAX_SHADES; i++)
    if(this->shades[i].getShadeId() != 255) this->shades[i].processWaitingFrame();
}
void SomfyShadeController::emitState(uint8_t num) {
  for(uint8_t i = 0; i < SOMFY_MAX_SHADES; i++) {
    SomfyShade *shade = &this->shades[i];
    if(shade->getShadeId() == 255) continue;
    shade->emitState(num);
  }
}
void SomfyShadeController::publish() {
  StaticJsonDocument<128> doc;
  JsonArray arr = doc.to<JsonArray>();
  for(uint8_t i = 0; i < SOMFY_MAX_SHADES; i++) {
    SomfyShade *shade = &this->shades[i];
    if(shade->getShadeId() == 255) continue;
    arr.add(shade->getShadeId());
    shade->publish();
  }
  mqtt.publish("shades", arr);
}
uint8_t SomfyShadeController::getNextShadeId() {
  uint8_t nextId = 0;
  // There is no shortcut for this since the deletion of
  // a shade in the middle makes all of this very difficult.
  for(uint8_t i = 1; i < SOMFY_MAX_SHADES - 1; i++) {
    bool id_exists = false;
    for(uint8_t j = 0; j < SOMFY_MAX_SHADES; j++) {
      SomfyShade *shade = &this->shades[j];
      if(shade->getShadeId() == i) {
        id_exists = true;
        break;
      }
    }
    if(!id_exists) {
      Serial.print("Got next Shade Id:");
      Serial.print(i);
      return i;
    }
  }
  return 255;
}
uint8_t SomfyShadeController::shadeCount() {
  uint8_t count = 0;
  for(uint8_t i = 0; i < SOMFY_MAX_SHADES; i++) {
    if(this->shades[i].getShadeId() != 255) count++;
  }
  return count;
}
uint32_t SomfyShadeController::getNextRemoteAddress(uint8_t shadeId) {
  uint32_t address = this->startingAddress + shadeId;
  uint8_t i = 0;
  while(i < SOMFY_MAX_SHADES) {
    if(this->shades[i].getShadeId() != 255) {
      if(this->shades[i].getRemoteAddress() == address) {
        address++;
        i = 0; // Start over we cannot share addresses.
      }
      else i++;
    }
    else i++;
  }
  return address;
}
SomfyShade *SomfyShadeController::addShade(JsonObject &obj) {
  SomfyShade *shade = this->addShade();
  if(shade) {
    shade->fromJSON(obj);
    shade->save();
    shade->emitState("shadeAdded");
  }
  return shade;
}
SomfyShade *SomfyShadeController::addShade() {
  uint8_t shadeId = this->getNextShadeId();
  // So the next shade id will be the first one we run into with an id of 255 so
  // if it gets deleted in the middle then it will get the first slot that is empty.
  // There is no apparent way around this.  In the future we might actually add an indexer
  // to it for sorting later.
  if(shadeId == 255) return nullptr;
  SomfyShade *shade = &this->shades[shadeId - 1];
  if(shade) {
    shade->setShadeId(shadeId);
    this->isDirty = true;
    if(this->useNVS()) {
      for(uint8_t i = 0; i < sizeof(this->m_shadeIds); i++) {
        this->m_shadeIds[i] = this->shades[i].getShadeId();
      }
      sortArray<uint8_t>(this->m_shadeIds, sizeof(this->m_shadeIds));
      uint8_t id = 0;
      // This little diddy is about a bug I had previously that left duplicates in the
      // sorted array.  So we will walk the sorted array until we hit a duplicate where the previous
      // value == the current value.  Set it to 255 then sort the array again.
      // 1,1,2,2,3,3,255...
      bool hadDups = false;
      for(uint8_t i = 0; i < sizeof(this->m_shadeIds); i++) {
        if(this->m_shadeIds[i] == 255) break;
        if(id == this->m_shadeIds[i]) {
          id = this->m_shadeIds[i];
          this->m_shadeIds[i] = 255;
          hadDups = true;
        }
        else {
          id = this->m_shadeIds[i];
        }
      }
      if(hadDups) sortArray<uint8_t>(this->m_shadeIds, sizeof(this->m_shadeIds));
      pref.begin("Shades");
      pref.remove("shadeIds");
      int x = pref.putBytes("shadeIds", this->m_shadeIds, sizeof(this->m_shadeIds));
      Serial.printf("WROTE %d bytes to shadeIds\n", x);
      pref.end();
      for(uint8_t i = 0; i < sizeof(this->m_shadeIds); i++) {
        if(i != 0) Serial.print(",");
        else Serial.print("Shade Ids: ");
        Serial.print(this->m_shadeIds[i]);
      }
      Serial.println();
      pref.begin("Shades");
      pref.getBytes("shadeIds", this->m_shadeIds, sizeof(this->m_shadeIds));
      Serial.print("LENGTH:");
      Serial.println(pref.getBytesLength("shadeIds"));
      pref.end();
      for(uint8_t i = 0; i < sizeof(this->m_shadeIds); i++) {
        if(i != 0) Serial.print(",");
        else Serial.print("Shade Ids: ");
        Serial.print(this->m_shadeIds[i]);
      }
      Serial.println();
    }
  }
  return shade;
}
void SomfyRemote::sendCommand(somfy_commands cmd, uint8_t repeat) {
  somfy_frame_t frame;
  frame.rollingCode = this->getNextRollingCode();
  frame.remoteAddress = this->getRemoteAddress();
  frame.cmd = cmd;
  frame.repeats = repeat;
  frame.bitLength = this->bitLength;
  // Match the encKey to the rolling code.  These keys range from 160 to 175.
  frame.encKey = 0xA0 | static_cast<uint8_t>(frame.rollingCode & 0x000F);
  if(frame.bitLength == 0) frame.bitLength = bit_length;
  this->lastRollingCode = frame.rollingCode;
  somfy.sendFrame(frame, repeat);
  somfy.processFrame(frame, true);
}
void SomfyShadeController::sendFrame(somfy_frame_t &frame, uint8_t repeat) {
  somfy.transceiver.beginTransmit();
  //Serial.println("----------- Sending Raw -------------");
  Serial.print("CMD:");
  Serial.print(translateSomfyCommand(frame.cmd));
  Serial.print(" ADDR:");
  Serial.print(frame.remoteAddress);
  Serial.print(" RCODE:");
  Serial.print(frame.rollingCode);
  Serial.print(" REPEAT:");
  Serial.println(repeat);
  
  byte frm[10];
  frame.encodeFrame(frm);
  this->transceiver.sendFrame(frm, frame.bitLength == 56 ? 2 : 12, frame.bitLength);
  // Transform the repeat bytes
  switch(frame.cmd) {
    case somfy_commands::StepUp:
    case somfy_commands::StepDown:
      break;
    case somfy_commands::Prog:
      frm[7] = 132;
      frm[9] = 63;
      break;
    default:
      frm[9] = 46;
      break;
  }
  for(uint8_t i = 0; i < repeat; i++) {
    this->transceiver.sendFrame(frm, frame.bitLength == 56 ? 7 : 6, frame.bitLength);
  }
  this->transceiver.endTransmit();
}
bool SomfyShadeController::deleteShade(uint8_t shadeId) {
  for(uint8_t i = 0; i < SOMFY_MAX_SHADES; i++) {
    if(this->shades[i].getShadeId() == shadeId) {
      shades[i].emitState("shadeRemoved");
      this->shades[i].setShadeId(255);
    }
  }
  if(this->useNVS()) {
    for(uint8_t i = 0; i < sizeof(this->m_shadeIds) - 1; i++) {
      if(this->m_shadeIds[i] == shadeId) {
        this->m_shadeIds[i] = 255;
      }
    }
    
    //qsort(this->m_shadeIds, sizeof(this->m_shadeIds)/sizeof(this->m_shadeIds[0]), sizeof(this->m_shadeIds[0]), sort_asc);
    sortArray<uint8_t>(this->m_shadeIds, sizeof(this->m_shadeIds));
    
    pref.begin("Shades");
    pref.putBytes("shadeIds", this->m_shadeIds, sizeof(this->m_shadeIds));
    pref.end();
  }
  this->commit();
  return true;
}
bool SomfyShadeController::loadShadesFile(const char *filename) { return ShadeConfigFile::load(this, filename); }
uint16_t SomfyRemote::getNextRollingCode() {
  pref.begin("ShadeCodes");
  uint16_t code = pref.getUShort(this->m_remotePrefId, 0);
  code++;
  pref.putUShort(this->m_remotePrefId, code);
  pref.end();
  this->lastRollingCode = code;
  return code;
}
uint16_t SomfyRemote::setRollingCode(uint16_t code) {
  if(this->lastRollingCode != code) {
    pref.begin("ShadeCodes");
    pref.putUShort(this->m_remotePrefId, code);
    pref.end();  
    this->lastRollingCode = code;
  }
  return code;
}
bool SomfyShadeController::toJSON(DynamicJsonDocument &doc) {
  doc["maxShades"] = SOMFY_MAX_SHADES;
  doc["maxLinkedRemotes"] = SOMFY_MAX_LINKED_REMOTES;
  doc["startingAddress"] = this->startingAddress;
  JsonObject objRadio = doc.createNestedObject("transceiver");
  this->transceiver.toJSON(objRadio);
  JsonArray arr = doc.createNestedArray("shades");
  for(uint8_t i = 0; i < SOMFY_MAX_SHADES; i++) {
    SomfyShade *shade = &this->shades[i];
    if(shade->getShadeId() != 255) {
      JsonObject oshade = arr.createNestedObject();
      shade->toJSON(oshade);
    }
  }
  return true;
}
bool SomfyShadeController::toJSON(JsonObject &obj) {
  obj["maxShades"] = SOMFY_MAX_SHADES;
  obj["maxLinkedRemotes"] = SOMFY_MAX_LINKED_REMOTES;
  obj["startingAddress"] = this->startingAddress;
  JsonObject oradio = obj.createNestedObject("transceiver");
  this->transceiver.toJSON(oradio);
  JsonArray arr = obj.createNestedArray("shades");
  this->toJSON(arr);
  return true;
}
bool SomfyShadeController::toJSON(JsonArray &arr) {
  for(uint8_t i = 0; i < SOMFY_MAX_SHADES; i++) {
    SomfyShade &shade = this->shades[i];
    if(shade.getShadeId() != 255) {
      JsonObject oshade = arr.createNestedObject();
      shade.toJSON(oshade);
    }
  }
  return true;
}
void SomfyShadeController::loop() { 
  this->transceiver.loop(); 
  for(uint8_t i; i < SOMFY_MAX_SHADES; i++) {
    if(this->shades[i].getShadeId() != 255) this->shades[i].checkMovement();
  }
  // Only commit the file once per second.
  if(this->isDirty && millis() - this->lastCommit > 1000) {
    this->commit();
  }
}
SomfyLinkedRemote::SomfyLinkedRemote() {}

// Transceiver Implementation
#define TOLERANCE_MIN 0.7
#define TOLERANCE_MAX 1.3

static const uint32_t tempo_wakeup_pulse = 9415;
static const uint32_t tempo_wakeup_min = 9415 * TOLERANCE_MIN;
static const uint32_t tempo_wakeup_max = 9415 * TOLERANCE_MAX;
static const uint32_t tempo_wakeup_silence = 89565;
static const uint32_t tempo_wakeup_silence_min = 89565 * TOLERANCE_MIN;
static const uint32_t tempo_wakeup_silence_max = 89565 * TOLERANCE_MAX;
static const uint32_t tempo_synchro_hw_min = SYMBOL * 4 * TOLERANCE_MIN;
static const uint32_t tempo_synchro_hw_max = SYMBOL * 4 * TOLERANCE_MAX;
static const uint32_t tempo_synchro_sw_min = 4550 * TOLERANCE_MIN;
static const uint32_t tempo_synchro_sw_max = 4550 * TOLERANCE_MAX;
static const uint32_t tempo_half_symbol_min = SYMBOL * TOLERANCE_MIN;
static const uint32_t tempo_half_symbol_max = SYMBOL * TOLERANCE_MAX;
static const uint32_t tempo_symbol_min = SYMBOL * 2 * TOLERANCE_MIN;
static const uint32_t tempo_symbol_max = SYMBOL * 2 * TOLERANCE_MAX;
static const uint32_t tempo_if_gap = 30415;  // Gap between frames


static int16_t  bitMin = SYMBOL * TOLERANCE_MIN;
static uint16_t timing_index = 0;
static somfy_rx_t somfy_rx;
static somfy_rx_queue_t rx_queue;


void somfy_rx_queue_t::init() { 
  Serial.println("Initializing RX Queue");
  memset(&this->items[0], 0x00, sizeof(somfy_rx_t) * MAX_RX_BUFFER);
  memset(&this->index[0], 0xFF, MAX_RX_BUFFER);
  this->length = 0;
}
bool somfy_rx_queue_t::pop(somfy_rx_t *rx) {
  // Read off the data from the oldest index.
  //Serial.println("Popping RX Queue");
  for(uint8_t i = MAX_RX_BUFFER - 1; i >= 0; i--) {
    if(this->index[i] < MAX_RX_BUFFER) {
      uint8_t ndx = this->index[i];
      memcpy(rx, &this->items[this->index[i]], sizeof(somfy_rx_t));
      memset(&this->items[ndx], 0x00, sizeof(somfy_rx_t));
      this->length--;
      this->index[i] = 255;
      return true;      
    }
  }
  return false;
}
void Transceiver::sendFrame(byte *frame, uint8_t sync, uint8_t bitLength) {
  if(!this->config.enabled) return;
  uint32_t pin = 1 << this->config.TXPin;
  if (sync == 2 || sync == 12) {  // Only with the first frame.  Repeats do not get a wakeup pulse.
    // All information online for the wakeup pulse appears to be incorrect.  While there is a wakeup
    // pulse it only sends an initial pulse.  There is no further delay after this.
    
    // Wake-up pulse
    //Serial.printf("Sending wakeup pulse: %d\n", sync);
    REG_WRITE(GPIO_OUT_W1TS_REG, pin);
    delayMicroseconds(10920);
    //delayMicroseconds(9415);
    
    // There is no silence after the wakeup pulse.  I tested this with Telis and no silence
    // was detected.  I suspect that for some battery powered shades the shade would go back
    // to sleep from the time of the initial pulse while the silence was occurring.
    REG_WRITE(GPIO_OUT_W1TC_REG, pin);
    delayMicroseconds(7357);
    //delayMicroseconds(9565);
    //delay(80);
  }
  // Depending on the bitness of the protocol we will be sending a different hwsync.
  // 56-bit 2 pulses for the first frame and 7 for the repeats
  // 80-bit 24 pulses for the first frame and 14 pulses for the repeats
  for (int i = 0; i < sync; i++) {
    REG_WRITE(GPIO_OUT_W1TS_REG, pin);
    delayMicroseconds(4 * SYMBOL);
    REG_WRITE(GPIO_OUT_W1TC_REG, pin);
    delayMicroseconds(4 * SYMBOL);
  }
  // Software sync
  REG_WRITE(GPIO_OUT_W1TS_REG, pin);
  delayMicroseconds(4450);
  // Start 0
  REG_WRITE(GPIO_OUT_W1TC_REG, pin);
  delayMicroseconds(SYMBOL);
  // Payload starting with the most significant bit.  The frame is always supplied in 80 bits
  // but if the protocol is calling for 56 bits it will only send 56 bits of the frame.
  for (byte i = 0; i < bitLength; i++) {
    if (((frame[i / 8] >> (7 - (i % 8))) & 1) == 1) {
      REG_WRITE(GPIO_OUT_W1TC_REG, pin);
      delayMicroseconds(SYMBOL);
      REG_WRITE(GPIO_OUT_W1TS_REG, pin);
      delayMicroseconds(SYMBOL);
    } else {
      REG_WRITE(GPIO_OUT_W1TS_REG, pin);
      delayMicroseconds(SYMBOL);
      REG_WRITE(GPIO_OUT_W1TC_REG, pin);
      delayMicroseconds(SYMBOL);
    }
  }
  // End with a 0 no matter what.  This accommodates the 56-bit protocol by telling the
  // motor that there are no more follow on bits.
  REG_WRITE(GPIO_OUT_W1TS_REG, pin);
  //delayMicroseconds(SYMBOL/2);
    
  // Inter-frame silence for 56-bit protocols are around 34ms.  However, an 80 bit protocol should
  // reduce this by the transmission of SYMBOL * 24 or 15,360us
  REG_WRITE(GPIO_OUT_W1TC_REG, pin);
  // Below are the original calculations for inter-frame silence.  However, when actually inspecting this from
  // the remote it appears to be closer to 27500us.  The delayMicoseconds call cannot be called with
  // values larger than 16383.
  /*
  if(bitLength == 80)
    delayMicroseconds(15055);
  else
    delayMicroseconds(30415);
  */
  if(bitLength != 80) {
    delayMicroseconds(13750);
    delayMicroseconds(13750);
  }
}

void RECEIVE_ATTR Transceiver::handleReceive() {
    static unsigned long last_time = 0;
    const long time = micros();
    const unsigned int duration = time - last_time;
    if (duration < bitMin) {
        // The incoming bit is < 448us so it is probably a glitch so blow it off.
        // We need to ignore this bit.
        // REMOVE THIS AFTER WE DETERMINE THAT THE out-of-bounds stuff isn't a problem.  If there are bits
        // from the previous frame then we will capture this data here.
        if(somfy_rx.pulseCount < MAX_TIMINGS && somfy_rx.cpt_synchro_hw > 0) somfy_rx.pulses[somfy_rx.pulseCount++] = duration;
        return;
    }
    last_time = time;
    switch (somfy_rx.status) {
    case waiting_synchro:
        if(somfy_rx.pulseCount < MAX_TIMINGS) somfy_rx.pulses[somfy_rx.pulseCount++] = duration;
        if (duration > tempo_synchro_hw_min && duration < tempo_synchro_hw_max) {
            // We have found a hardware sync bit.  There should be at least 4 of these.
            ++somfy_rx.cpt_synchro_hw;
        }
        else if (duration > tempo_synchro_sw_min && duration < tempo_synchro_sw_max && somfy_rx.cpt_synchro_hw >= 4) {
            // If we have a full hardware sync then we should look for the software sync.  If we have a software sync
            // bit and enough hardware sync bits then we should start receiving data.  It turns out that a 56 bit packet
            // with give 4 or 14 bits of hardware sync.  An 80 bit packet give 12 or 24 bits of hw sync.  Early on
            // I had some shorter and longer hw syncs but I can no longer repeat this.
            memset(somfy_rx.payload, 0x00, sizeof(somfy_rx.payload));
            somfy_rx.previous_bit = 0x00;
            somfy_rx.waiting_half_symbol = false;
            somfy_rx.cpt_bits = 0;
            // Keep an eye on this as it is possible that we might get fewer or more synchro bits.
            if (somfy_rx.cpt_synchro_hw <= 7) somfy_rx.bit_length = 56;
            else if (somfy_rx.cpt_synchro_hw == 14) somfy_rx.bit_length = 56;
            else if (somfy_rx.cpt_synchro_hw == 12) somfy_rx.bit_length = 80;
            else if (somfy_rx.cpt_synchro_hw > 17) somfy_rx.bit_length = 80;
            somfy_rx.status = receiving_data;
        }
        else {
            // Reset and start looking for hardware sync again.
            somfy_rx.cpt_synchro_hw = 0;
            // Try to capture the wakeup pulse.
            if(duration > tempo_wakeup_min && duration < tempo_wakeup_max)
            {
                memset(&somfy_rx.payload, 0x00, sizeof(somfy_rx.payload));
                somfy_rx.previous_bit = 0x00;
                somfy_rx.waiting_half_symbol = false;
                somfy_rx.cpt_bits = 0;
                somfy_rx.bit_length = 56;
            }
            else if((somfy_rx.pulseCount > 20 && somfy_rx.cpt_synchro_hw == 0) || duration > 250000) {
              somfy_rx.pulseCount = 0;
            }
        }
        break;
    case receiving_data:
        if(somfy_rx.pulseCount < MAX_TIMINGS) somfy_rx.pulses[somfy_rx.pulseCount++] = duration;
        // We should be receiving data at this point.
        if (duration > tempo_symbol_min && duration < tempo_symbol_max && !somfy_rx.waiting_half_symbol) {
            somfy_rx.previous_bit = 1 - somfy_rx.previous_bit;
            // Bits come in high order bit first.
            somfy_rx.payload[somfy_rx.cpt_bits / 8] += somfy_rx.previous_bit << (7 - somfy_rx.cpt_bits % 8);
            ++somfy_rx.cpt_bits;
        }
        else if (duration > tempo_half_symbol_min && duration < tempo_half_symbol_max) {
            if (somfy_rx.waiting_half_symbol) {
                somfy_rx.waiting_half_symbol = false;
                somfy_rx.payload[somfy_rx.cpt_bits / 8] += somfy_rx.previous_bit << (7 - somfy_rx.cpt_bits % 8);
                ++somfy_rx.cpt_bits;
            }
            else {
                somfy_rx.waiting_half_symbol = true;
            }
        }
        else {
            
            ++somfy_rx.cpt_bits;
            
            /*
            // Start over we are not within our parameters for bit timing.
            memset(&somfy_rx.payload, 0x00, sizeof(somfy_rx.payload));
            somfy_rx.pulseCount = 0;
            somfy_rx.cpt_synchro_hw = 0;
            somfy_rx.previous_bit = 0x00;
            somfy_rx.waiting_half_symbol = false;
            somfy_rx.cpt_bits = 0;
            somfy_rx.bit_length = 56;
            somfy_rx.status = waiting_synchro;
            */
        }
        break;
    default:
        break;
    }
    if (somfy_rx.status == receiving_data && somfy_rx.cpt_bits >= somfy_rx.bit_length) {
        // Since we are operating within the interrupt all data really needs to be static
        // for the handoff to the frame decoder.  For this reason we are buffering up to
        // 3 total frames.  Althought it may not matter considering the lenght of a packet
        // will likely not push over the loop timing.  For now lets assume that there
        // may be some pressure on the loop for features.
        if(rx_queue.length >= MAX_RX_BUFFER) {
          // We have overflowed the buffer simply empty the last item
          // in this instance we will simply throw it away.
          uint8_t ndx = rx_queue.index[MAX_RX_BUFFER - 1];
          if(ndx < MAX_RX_BUFFER) rx_queue.items[ndx].pulseCount = 0;
          //memset(&this->items[ndx], 0x00, sizeof(somfy_rx_t));
          rx_queue.index[MAX_RX_BUFFER - 1] = 255;
          rx_queue.length--;
        }
        uint8_t first = 0;
        // Place this record in the first empty slot.  There will
        // be one since we cleared a space above should there
        // be an overflow.
        for(uint8_t i = 0; i < MAX_RX_BUFFER; i++) {
          if(rx_queue.items[i].pulseCount == 0) {
            first = i;
            memcpy(&rx_queue.items[i], &somfy_rx, sizeof(somfy_rx_t));
            break;
          }
        }
        // Move the index so that it is the at position 0.  The oldest item will fall off.
        for(uint8_t i = MAX_RX_BUFFER - 1; i > 0; i--) {
          rx_queue.index[i] = rx_queue.index[i - 1];
        }
        rx_queue.length++;
        rx_queue.index[0] = first;
        memset(&somfy_rx.payload, 0x00, sizeof(somfy_rx.payload));
        somfy_rx.cpt_synchro_hw = 0;
        somfy_rx.previous_bit = 0x00;
        somfy_rx.waiting_half_symbol = false;
        somfy_rx.cpt_bits = 0;
        somfy_rx.pulseCount = 0;
        somfy_rx.status = waiting_synchro;
    }
}
bool Transceiver::receive() {
    // Check to see if there is anything in the buffer
    if(rx_queue.length > 0) {
      //Serial.printf("Processing receive %d\n", rx_queue.length);
      somfy_rx_t rx;
      rx_queue.pop(&rx);
      this->frame.decodeFrame(&rx);
      this->emitFrame(&this->frame, &rx);
      return this->frame.valid;
      
    }
    return false;
}
void Transceiver::emitFrame(somfy_frame_t *frame, somfy_rx_t *rx) {
  if(sockEmit.activeClients(ROOM_EMIT_FRAME) > 0) {
    ClientSocketEvent evt("remoteFrame");
    char buf[30];
    snprintf(buf, sizeof(buf), "{\"encKey\":%d,", frame->encKey);
    evt.appendMessage(buf);
    snprintf(buf, sizeof(buf), "\"address\":%d,", frame->remoteAddress);
    evt.appendMessage(buf);
    snprintf(buf, sizeof(buf), "\"rcode\":%d,", frame->rollingCode);
    evt.appendMessage(buf);
    snprintf(buf, sizeof(buf), "\"command\":\"%s\",", translateSomfyCommand(frame->cmd));
    evt.appendMessage(buf);
    snprintf(buf, sizeof(buf), "\"rssi\":%d,", frame->rssi);
    evt.appendMessage(buf);
    snprintf(buf, sizeof(buf), "\"bits\":%d,", rx->bit_length);
    evt.appendMessage(buf);
    snprintf(buf, sizeof(buf), "\"valid\":%s,", frame->valid ? "true" : "false");
    evt.appendMessage(buf);
    snprintf(buf, sizeof(buf), "\"sync\":%d,\"pulses\":[", frame->hwsync);
    evt.appendMessage(buf);
    
    if(rx) {
      for(uint16_t i = 0; i < rx->pulseCount; i++) {
        snprintf(buf, sizeof(buf), "%s%d", i != 0 ? "," : "", rx->pulses[i]);
        evt.appendMessage(buf);
      }
    }
    evt.appendMessage("]}");
    sockEmit.sendToRoom(ROOM_EMIT_FRAME, &evt);
  }
}
void Transceiver::clearReceived(void) {
    //packet_received = false;
    //memset(receive_buffer, 0x00, sizeof(receive_buffer));
    if(this->config.enabled)
      attachInterrupt(interruptPin, handleReceive, CHANGE);
}
void Transceiver::enableReceive(void) {
    if(rxmode > 0) return;
    if(this->config.enabled) {
      Serial.print("Enabling receive on Pin #");
      Serial.println(this->config.RXPin);
      rxmode = 1;
      pinMode(this->config.RXPin, INPUT);
      interruptPin = digitalPinToInterrupt(this->config.RXPin);
      ELECHOUSE_cc1101.SetRx();
      attachInterrupt(interruptPin, handleReceive, CHANGE);
    }
}
void Transceiver::disableReceive(void) { 
  rxmode = 0;
  if(interruptPin > 0) detachInterrupt(interruptPin); 
  interruptPin = 0;
  
}
bool Transceiver::toJSON(JsonObject& obj) {
    Serial.println("Setting Transceiver Json");
    JsonObject objConfig = obj.createNestedObject("config");
    this->config.toJSON(objConfig);
    return true;
}
bool Transceiver::fromJSON(JsonObject& obj) {
    if (obj.containsKey("config")) {
      JsonObject objConfig = obj["config"];
      this->config.fromJSON(objConfig);
    }
    return true;
}
bool Transceiver::save() {
    this->config.save();
    this->config.apply();
    return true;
}
bool Transceiver::end() {
    this->disableReceive();
    return true;
}
void transceiver_config_t::fromJSON(JsonObject& obj) {
    Serial.print("Deserialize Radio JSON ");
    if(obj.containsKey("type")) this->type = obj["type"];
    if(obj.containsKey("CSNPin")) this->CSNPin = obj["CSNPin"];
    if(obj.containsKey("MISOPin")) this->MISOPin = obj["MISOPin"];
    if(obj.containsKey("MOSIPin")) this->MOSIPin = obj["MOSIPin"];
    if(obj.containsKey("RXPin")) this->RXPin = obj["RXPin"];
    if(obj.containsKey("SCKPin")) this->SCKPin = obj["SCKPin"];
    if(obj.containsKey("TXPin")) this->TXPin = obj["TXPin"];
    if(obj.containsKey("rxBandwidth")) this->rxBandwidth = obj["rxBandwidth"]; // float
    if(obj.containsKey("frequency")) this->frequency = obj["frequency"];  // float
    if(obj.containsKey("deviation")) this->deviation = obj["deviation"];  // float
    if(obj.containsKey("enabled")) this->enabled = obj["enabled"];
    if(obj.containsKey("txPower")) this->txPower = obj["txPower"];

    /*
    if (obj.containsKey("internalCCMode")) this->internalCCMode = obj["internalCCMode"];
    if (obj.containsKey("modulationMode")) this->modulationMode = obj["modulationMode"];
    if (obj.containsKey("channel")) this->channel = obj["channel"];
    if (obj.containsKey("channelSpacing")) this->channelSpacing = obj["channelSpacing"]; // float
    if (obj.containsKey("dataRate")) this->dataRate = obj["dataRate"]; // float
    if (obj.containsKey("syncMode")) this->syncMode = obj["syncMode"];
    if (obj.containsKey("syncWordHigh")) this->syncWordHigh = obj["syncWordHigh"];
    if (obj.containsKey("syncWordLow")) this->syncWordLow = obj["syncWordLow"];
    if (obj.containsKey("addrCheckMode")) this->addrCheckMode = obj["addrCheckMode"];
    if (obj.containsKey("checkAddr")) this->checkAddr = obj["checkAddr"];
    if (obj.containsKey("dataWhitening")) this->dataWhitening = obj["dataWhitening"];
    if (obj.containsKey("pktFormat")) this->pktFormat = obj["pktFormat"];
    if (obj.containsKey("pktLengthMode")) this->pktLengthMode = obj["pktLengthMode"];
    if (obj.containsKey("pktLength")) this->pktLength = obj["pktLength"];
    if (obj.containsKey("useCRC")) this->useCRC = obj["useCRC"];
    if (obj.containsKey("autoFlushCRC")) this->autoFlushCRC = obj["autoFlushCRC"];
    if (obj.containsKey("disableDCFilter")) this->disableDCFilter = obj["disableCRCFilter"];
    if (obj.containsKey("enableManchester")) this->enableManchester = obj["enableManchester"];
    if (obj.containsKey("enableFEC")) this->enableFEC = obj["enableFEC"];
    if (obj.containsKey("minPreambleBytes")) this->minPreambleBytes = obj["minPreambleBytes"];
    if (obj.containsKey("pqtThreshold")) this->pqtThreshold = obj["pqtThreshold"];
    if (obj.containsKey("appendStatus")) this->appendStatus = obj["appendStatus"];
    if (obj.containsKey("printBuffer")) this->printBuffer = obj["printBuffer"];
    */
    Serial.printf("SCK:%u MISO:%u MOSI:%u CSN:%u RX:%u TX:%u\n", this->SCKPin, this->MISOPin, this->MOSIPin, this->CSNPin, this->RXPin, this->TXPin);
}
void transceiver_config_t::toJSON(JsonObject& obj) {
    obj["type"] = this->type;
    obj["TXPin"] = this->TXPin;
    obj["RXPin"] = this->RXPin;
    obj["SCKPin"] = this->SCKPin;
    obj["MOSIPin"] = this->MOSIPin;
    obj["MISOPin"] = this->MISOPin;
    obj["CSNPin"] = this->CSNPin;
    obj["rxBandwidth"] = this->rxBandwidth; // float
    obj["frequency"] = this->frequency;  // float
    obj["deviation"] = this->deviation;  // float
    obj["txPower"] = this->txPower;
    /*
    obj["internalCCMode"] = this->internalCCMode;
    obj["modulationMode"] = this->modulationMode;
    obj["channel"] = this->channel;
    obj["channelSpacing"] = this->channelSpacing; // float
    obj["dataRate"] = this->dataRate; // float
    obj["syncMode"] = this->syncMode;
    obj["syncWordHigh"] = this->syncWordHigh;
    obj["syncWordLow"] = this->syncWordLow;
    obj["addrCheckMode"] = this->addrCheckMode;
    obj["checkAddr"] = this->checkAddr;
    obj["dataWhitening"] = this->dataWhitening;
    obj["pktFormat"] = this->pktFormat;
    obj["pktLengthMode"] = this->pktLengthMode;
    obj["pktLength"] = this->pktLength;
    obj["useCRC"] = this->useCRC;
    obj["autoFlushCRC"] = this->autoFlushCRC;
    obj["disableDCFilter"] = this->disableDCFilter;
    obj["enableManchester"] = this->enableManchester;
    obj["enableFEC"] = this->enableFEC;
    obj["minPreambleBytes"] = this->minPreambleBytes;
    obj["pqtThreshold"] = this->pqtThreshold;
    obj["appendStatus"] = this->appendStatus;
    obj["printBuffer"] = somfy.transceiver.printBuffer;
    */
    obj["enabled"] = this->enabled;
    obj["radioInit"] = this->radioInit;
    Serial.print("Serialize Radio JSON ");
    Serial.printf("SCK:%u MISO:%u MOSI:%u CSN:%u RX:%u TX:%u\n", this->SCKPin, this->MISOPin, this->MOSIPin, this->CSNPin, this->RXPin, this->TXPin);
}
void transceiver_config_t::save() {
    pref.begin("CC1101");
    pref.clear();
    pref.putUChar("type", this->type);
    pref.putUChar("TXPin", this->TXPin);
    pref.putUChar("RXPin", this->RXPin);
    pref.putUChar("SCKPin", this->SCKPin);
    pref.putUChar("MOSIPin", this->MOSIPin);
    pref.putUChar("MISOPin", this->MISOPin);
    pref.putUChar("CSNPin", this->CSNPin);
    pref.putFloat("frequency", this->frequency);  // float
    pref.putFloat("deviation", this->deviation);  // float
    pref.putFloat("rxBandwidth", this->rxBandwidth); // float
    pref.putBool("enabled", this->enabled);
    pref.putBool("radioInit", true);
    pref.putChar("txPower", this->txPower);
    
    /*
    pref.putBool("internalCCMode", this->internalCCMode);
    pref.putUChar("modulationMode", this->modulationMode);
    pref.putUChar("channel", this->channel);
    pref.putFloat("channelSpacing", this->channelSpacing); // float
    pref.putFloat("rxBandwidth", this->rxBandwidth); // float
    pref.putFloat("dataRate", this->dataRate); // float
    pref.putChar("txPower", this->txPower);
    pref.putUChar("syncMode", this->syncMode);
    pref.putUShort("syncWordHigh", this->syncWordHigh);
    pref.putUShort("syncWordLow", this->syncWordLow);
    pref.putUChar("addrCheckMode", this->addrCheckMode);
    pref.putUChar("checkAddr", this->checkAddr);
    pref.putBool("dataWhitening", this->dataWhitening);
    pref.putUChar("pktFormat", this->pktFormat);
    pref.putUChar("pktLengthMode", this->pktLengthMode);
    pref.putUChar("pktLength", this->pktLength);
    pref.putBool("useCRC", this->useCRC);
    pref.putBool("autoFlushCRC", this->autoFlushCRC);
    pref.putBool("disableDCFilter", this->disableDCFilter);
    pref.putBool("enableManchester", this->enableManchester);
    pref.putBool("enableFEC", this->enableFEC);
    pref.putUChar("minPreambleBytes", this->minPreambleBytes);
    pref.putUChar("pqtThreshold", this->pqtThreshold);
    pref.putBool("appendStatus", this->appendStatus);
    */
    pref.end();
   
    Serial.print("Save Radio Settings ");
    Serial.printf("SCK:%u MISO:%u MOSI:%u CSN:%u RX:%u TX:%u\n", this->SCKPin, this->MISOPin, this->MOSIPin, this->CSNPin, this->RXPin, this->TXPin);
}
void transceiver_config_t::removeNVSKey(const char *key) {
  if(pref.isKey(key)) {
    Serial.printf("Removing NVS Key: CC1101.%s\n", key);
    pref.remove(key);
  }
}
void transceiver_config_t::load() {
    pref.begin("CC1101");
    this->type = pref.getUChar("type", 56);
    this->TXPin = pref.getUChar("TXPin", this->TXPin);
    this->RXPin = pref.getUChar("RXPin", this->RXPin);
    this->SCKPin = pref.getUChar("SCKPin", this->SCKPin);
    this->MOSIPin = pref.getUChar("MOSIPin", this->MOSIPin);
    this->MISOPin = pref.getUChar("MISOPin", this->MISOPin);
    this->CSNPin = pref.getUChar("CSNPin", this->CSNPin);
    this->frequency = pref.getFloat("frequency", this->frequency);  // float
    this->deviation = pref.getFloat("deviation", this->deviation);  // float
    this->enabled = pref.getBool("enabled", this->enabled);
    this->txPower = pref.getChar("txPower", this->txPower);
    this->rxBandwidth = pref.getFloat("rxBandwidth", this->rxBandwidth);
    
    this->removeNVSKey("internalCCMode");
    this->removeNVSKey("modulationMode");
    this->removeNVSKey("channel");
    this->removeNVSKey("channelSpacing");
    this->removeNVSKey("dataRate");
    this->removeNVSKey("syncMode");
    this->removeNVSKey("syncWordHigh");
    this->removeNVSKey("syncWordLow");
    this->removeNVSKey("addrCheckMode");
    this->removeNVSKey("checkAddr");
    this->removeNVSKey("dataWhitening");
    this->removeNVSKey("pktFormat");
    this->removeNVSKey("pktLengthMode");
    this->removeNVSKey("pktLength");
    this->removeNVSKey("useCRC");
    this->removeNVSKey("autoFlushCRC");
    this->removeNVSKey("disableDCFilter");
    this->removeNVSKey("enableManchester");
    this->removeNVSKey("enableFEC");
    this->removeNVSKey("minPreambleBytes");
    this->removeNVSKey("pqtThreshold");
    this->removeNVSKey("appendStatus");
    pref.end();
    //this->printBuffer = somfy.transceiver.printBuffer;
}
void transceiver_config_t::apply() {
    somfy.transceiver.disableReceive();
    bit_length = this->type;    
    if(this->enabled) {
      bool radioInit = true;
      pref.begin("CC1101");
      radioInit = pref.getBool("radioInit", true);
      // If the radio locks up then we can simply reboot and re-enable the radio.
      pref.putBool("radioInit", false);
      this->radioInit = false;
      pref.end();
      if(!radioInit) return;
      Serial.print("Applying radio settings ");
      Serial.printf("Setting Data Pins RX:%u TX:%u\n", this->RXPin, this->TXPin);
      //if(this->TXPin != this->RXPin)
      //  pinMode(this->TXPin, OUTPUT);
      //pinMode(this->RXPin, INPUT);
      // Essentially these call only preform the two functions above.
      if(this->TXPin == this->RXPin)
        ELECHOUSE_cc1101.setGDO0(this->TXPin); // This pin may be shared.
      else
        ELECHOUSE_cc1101.setGDO(this->TXPin, this->RXPin); // GDO0, GDO2
      Serial.printf("Setting SPI Pins SCK:%u MISO:%u MOSI:%u CSN:%u\n", this->SCKPin, this->MISOPin, this->MOSIPin, this->CSNPin);
      ELECHOUSE_cc1101.setSpiPin(this->SCKPin, this->MISOPin, this->MOSIPin, this->CSNPin);
      Serial.println("Radio Pins Configured!");
      ELECHOUSE_cc1101.Init();
      ELECHOUSE_cc1101.setCCMode(0);                            // set config for internal transmission mode.
      ELECHOUSE_cc1101.setMHZ(this->frequency);                 // Here you can set your basic frequency. The lib calculates the frequency automatically (default = 433.92).The cc1101 can: 300-348 MHZ, 387-464MHZ and 779-928MHZ. Read More info from datasheet.
      ELECHOUSE_cc1101.setRxBW(this->rxBandwidth);              // Set the Receive Bandwidth in kHz. Value from 58.03 to 812.50. Default is 812.50 kHz.
      ELECHOUSE_cc1101.setDeviation(this->deviation);
      ELECHOUSE_cc1101.setPA(this->txPower);                    // Set TxPower. The following settings are possible depending on the frequency band.  (-30  -20  -15  -10  -6    0    5    7    10   11   12) Default is max!
      //ELECHOUSE_cc1101.setModulation(this->modulationMode);     // set modulation mode. 0 = 2-FSK, 1 = GFSK, 2 = ASK/OOK, 3 = 4-FSK, 4 = MSK.
      if (!ELECHOUSE_cc1101.getCC1101()) {
          Serial.println("Error setting up the radio");
          this->radioInit = false;
      }
      else {
          Serial.println("Successfully set up the radio");
          somfy.transceiver.enableReceive();
          this->radioInit = true;
      }
      pref.begin("CC1101");
      pref.putBool("radioInit", true);
      pref.end();
      
    }
    else {
      ELECHOUSE_cc1101.setSidle();
      somfy.transceiver.disableReceive();
      this->radioInit = false;
    }
    /*
    ELECHOUSE_cc1101.setChannel(this->channel);               // Set the Channelnumber from 0 to 255. Default is cahnnel 0.
    ELECHOUSE_cc1101.setChsp(this->channelSpacing);           // The channel spacing is multiplied by the channel number CHAN and added to the base frequency in kHz. Value from 25.39 to 405.45. Default is 199.95 kHz.
    ELECHOUSE_cc1101.setDRate(this->dataRate);                // Set the Data Rate in kBaud. Value from 0.02 to 1621.83. Default is 99.97 kBaud!
    ELECHOUSE_cc1101.setSyncMode(this->syncMode);             // Combined sync-word qualifier mode. 0 = No preamble/sync. 1 = 16 sync word bits detected. 2 = 16/16 sync word bits detected. 3 = 30/32 sync word bits detected. 4 = No preamble/sync, carrier-sense above threshold. 5 = 15/16 + carrier-sense above threshold. 6 = 16/16 + carrier-sense above threshold. 7 = 30/32 + carrier-sense above threshold.
    ELECHOUSE_cc1101.setSyncWord(this->syncWordHigh, this->syncWordLow); // Set sync word. Must be the same for the transmitter and receiver. (Syncword high, Syncword low)
    ELECHOUSE_cc1101.setAdrChk(this->addrCheckMode);          // Controls address check configuration of received packages. 0 = No address check. 1 = Address check, no broadcast. 2 = Address check and 0 (0x00) broadcast. 3 = Address check and 0 (0x00) and 255 (0xFF) broadcast.
    ELECHOUSE_cc1101.setAddr(this->checkAddr);                // Address used for packet filtration. Optional broadcast addresses are 0 (0x00) and 255 (0xFF).
    ELECHOUSE_cc1101.setWhiteData(this->dataWhitening);       // Turn data whitening on / off. 0 = Whitening off. 1 = Whitening on.
    ELECHOUSE_cc1101.setPktFormat(this->pktFormat);           // Format of RX and TX data. 0 = Normal mode, use FIFOs for RX and TX. 1 = Synchronous serial mode, Data in on GDO0 and data out on either of the GDOx pins. 2 = Random TX mode; sends random data using PN9 generator. Used for test. Works as normal mode, setting 0 (00), in RX. 3 = Asynchronous serial mode, Data in on GDO0 and data out on either of the GDOx pins.
    ELECHOUSE_cc1101.setLengthConfig(this->pktLengthMode);    // 0 = Fixed packet length mode. 1 = Variable packet length mode. 2 = Infinite packet length mode. 3 = Reserved
    ELECHOUSE_cc1101.setPacketLength(this->pktLength);        // Indicates the packet length when fixed packet length mode is enabled. If variable packet length mode is used, this value indicates the maximum packet length allowed.
    ELECHOUSE_cc1101.setCrc(this->useCRC);                    // 1 = CRC calculation in TX and CRC check in RX enabled. 0 = CRC disabled for TX and RX.
    ELECHOUSE_cc1101.setCRC_AF(this->autoFlushCRC);           // Enable automatic flush of RX FIFO when CRC is not OK. This requires that only one packet is in the RXIFIFO and that packet length is limited to the RX FIFO size.
    ELECHOUSE_cc1101.setDcFilterOff(this->disableDCFilter);   // Disable digital DC blocking filter before demodulator. Only for data rates â‰¤ 250 kBaud The recommended IF frequency changes when the DC blocking is disabled. 1 = Disable (current optimized). 0 = Enable (better sensitivity).
    ELECHOUSE_cc1101.setManchester(this->enableManchester);   // Enables Manchester encoding/decoding. 0 = Disable. 1 = Enable.
    ELECHOUSE_cc1101.setFEC(this->enableFEC);                 // Enable Forward Error Correction (FEC) with interleaving for packet payload (Only supported for fixed packet length mode. 0 = Disable. 1 = Enable.
    ELECHOUSE_cc1101.setPRE(this->minPreambleBytes);          // Sets the minimum number of preamble bytes to be transmitted. Values: 0 : 2, 1 : 3, 2 : 4, 3 : 6, 4 : 8, 5 : 12, 6 : 16, 7 : 24
    ELECHOUSE_cc1101.setPQT(this->pqtThreshold);              // Preamble quality estimator threshold. The preamble quality estimator increases an internal counter by one each time a bit is received that is different from the previous bit, and decreases the counter by 8 each time a bit is received that is the same as the last bit. A threshold of 4âˆ™PQT for this counter is used to gate sync word detection. When PQT=0 a sync word is always accepted.
    ELECHOUSE_cc1101.setAppendStatus(this->appendStatus);     // When enabled, two status bytes will be appended to the payload of the packet. The status bytes contain RSSI and LQI values, as well as CRC OK.
    */
    //somfy.transceiver.printBuffer = this->printBuffer;
}
bool Transceiver::begin() {
    this->config.load();
    this->config.apply();
    rx_queue.init();
    return true;
}
void Transceiver::loop() {
    if (this->receive()) {
        //this->clearReceived();
        somfy.processFrame(this->frame, false);
    }
    else {
      somfy.processWaitingFrame();
    }
}
somfy_frame_t& Transceiver::lastFrame() { return this->frame; }
void Transceiver::beginTransmit() {
    if(this->config.enabled) {
      this->disableReceive();
      pinMode(this->config.TXPin, OUTPUT);
      digitalWrite(this->config.TXPin, 0);
      ELECHOUSE_cc1101.SetTx();
    }
}
void Transceiver::endTransmit() {
    if(this->config.enabled) {
      ELECHOUSE_cc1101.setSidle();
      delay(100);
      this->enableReceive();
    }
}
