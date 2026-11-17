// Tableau de bord energie Bluetooth pour van -> Waveshare ESP32-C6-Touch-LCD-1.83
// Chaque equipement s'active ou non dans config.h.
// - Batterie LiFePO4 a BMS CBT Power, par exemple Creabest (BMS CBT Power "VB", protocole repris de aiobmsble)
// - Victron MPPT, BMV-712, Orion XS (annonces "Instant Readout", voir victron.h)
// - Frigo EZA Blizz-E (lecture seule, une connexion courte par minute, voir frigo.h)
// Page de synthese (flux d'energie) puis une page par appareil :
// glisser vers la gauche, toucher ou bouton BOOT = page suivante,
// glisser vers la droite = page precedente.
// Glisser vers le haut / le bas = plus / moins lumineux (reglage memorise).
// Bouton PWR (appui court) = eteint ou rallume l'ecran ; appui de 6 s = arret de la carte.
// Veille apres SLEEP_MS sans action ; un appui (ecran ou bouton) rallume l'ecran.
// Bibliotheques : "GFX Library for Arduino" + "U8g2" (pour les polices).
#include <BLEDevice.h>
#include <Preferences.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Arduino_GFX_Library.h>
#include "victron.h"
#include "frigo.h"

struct Victron {
  const char *title;
  bool use;  // 1 = appareil present dans l'installation
  const char *mac, *key;
  uint8_t k[16];
  bool on;
  VData d;
  unsigned long seen, logged;
};

// Reglages personnels (batterie, frigo, appareils Victron) : voir config.example.h
#include "config.h"

#define NVIC (sizeof(vic) / sizeof(vic[0]))

// Broches (wiki Waveshare)
#define LCD_SCK 1
#define LCD_DIN 2
#define LCD_CS 5
#define LCD_DC 3
#define LCD_RST 4
#define LCD_BL 6
#define TP_SDA 7
#define TP_SCL 8
#define TP_ADDR 0x15  // CST816D
#define BTN_BOOT 9     // bouton BOOT : broche de demarrage de l'ESP32-C6 (a confirmer sur la carte)
#define PMU_ADDR 0x34  // AXP2101 (gestion d'alimentation), signale l'appui sur PWR

#define W 240
#define H 284

Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_DIN);
Arduino_GFX *panel = new Arduino_ST7789(bus, LCD_RST, 0, true, W, H);
// Dessin en memoire puis envoi d'un bloc : pas de clignotement
Arduino_Canvas *canvas = new Arduino_Canvas(W, H, panel);
Arduino_GFX *gfx = canvas;

// Couleurs
#define C_BG RGB565(0, 0, 0)
#define C_TRACK RGB565(38, 38, 44)
#define C_TEXT RGB565(245, 245, 247)
#define C_DIM RGB565(142, 142, 147)
#define C_GREEN RGB565(48, 209, 88)
#define C_AMBER RGB565(255, 176, 32)
#define C_RED RGB565(255, 69, 58)
#define C_BLUE RGB565(10, 132, 255)
#define C_ICE RGB565(100, 210, 255)

static BLEUUID SVC_RX("0000ffe0-0000-1000-8000-00805f9b34fb");
static BLEUUID CHR_RX("0000ffe4-0000-1000-8000-00805f9b34fb");
static BLEUUID SVC_TX("0000ffe5-0000-1000-8000-00805f9b34fb");
static BLEUUID CHR_TX("0000ffe9-0000-1000-8000-00805f9b34fb");
static BLEUUID SVC_FRIGO("00001234-0000-1000-8000-00805f9b34fb");
static BLEUUID CHR_FRIGO_W("00001235-0000-1000-8000-00805f9b34fb");
static BLEUUID CHR_FRIGO_N("00001236-0000-1000-8000-00805f9b34fb");
static const char CMD_STATUS[] = "~11014642E00201FD35\r";

BLEScan *scan;
BLEAdvertisedDevice *volatile target = nullptr;
BLEClient *client = nullptr;
BLERemoteCharacteristic *tx = nullptr;
volatile bool connected = false;
const char *bleStatus = "Recherche...";

String frame;  // trame ASCII en cours de reception
volatile bool frameReady = false;
String readyFrame;

int soc = -1, cycles = 0;
float voltage = 0, current = 0, temp = 0;

BLEAdvertisedDevice *volatile frigoDev = nullptr;  // frigo reconnu par le scan
BLEClient *frigoClient = nullptr;
volatile unsigned long frigoSeen = 0, frigoRead = 0;  // derniere annonce, derniere lecture reussie
FrigoData frigo;
FrigoRx frigoRx;

int pages[3 + NVIC], npages = 0, cur = 0;  // -1 = synthese, -2 = frigo, 0 = batterie, i+1 = vic[i]
volatile bool dirty = true;  // nouvelles mesures
bool uiDirty = true;          // action de l'utilisateur : redessin immediat

const uint8_t LEVELS[] = {10, 25, 50, 75, 100};  // luminosite en %
#define NLEVELS (int)sizeof(LEVELS)
int level = NLEVELS - 1;
volatile bool sleeping = false;
unsigned long lastTouch = 0, overlayUntil = 0;
Preferences prefs;

volatile unsigned long retryAfter = 0;  // pause apres un echec de connexion Creabest

// Compare deux adresses MAC sans tenir compte des ':' ni de la casse
bool sameMac(const String &a, const char *b) {
  String x = a, y = b;
  x.replace(":", "");
  y.replace(":", "");
  return x.equalsIgnoreCase(y);
}

void logVictron(const Victron &v) {
  const VData &d = v.d;
  if (d.type == VIC_SOLAR)
    Serial.printf("%s: etat=%d err=%d %.2fV %.1fA %.0fW %.2fkWh\n", v.title, d.state, d.error, d.voltage, d.current, d.power, d.yieldKWh);
  else if (d.type == VIC_BMV)
    Serial.printf("%s: SOC=%.1f%% %.2fV %.3fA %.1fAh aux(%d)=%.2f reste=%dmin\n", v.title, d.soc, d.voltage, d.current, d.consumedAh, d.auxMode, d.aux, d.remainingMin);
  else if (d.type == VIC_ORION_XS)
    Serial.printf("%s: etat=%d err=%d sortie %.2fV %.1fA entree %.2fV %.1fA\n", v.title, d.state, d.error, d.voltage, d.current, d.inVoltage, d.inCurrent);
}

// Noms acceptes par l'application de ces frigos ; FRIGO_NAME, s'il est rempli, les remplace
bool isFrigoName(const char *s, int n) {
  while (n > 0 && s[n - 1] == ' ') n--;  // le frigo complete son nom par des espaces
  if (FRIGO_NAME[0]) return n == (int)strlen(FRIGO_NAME) && !memcmp(s, FRIGO_NAME, n);
  static const char *const P[] = {"WT-", "WH-", "A1-", "AK-", "AK1-", "AK2-", "AK3-", "MF-", "KGS-", "KGD-", "K25", "BLIZZ", "EZA"};
  for (auto p : P)
    if (n >= (int)strlen(p) && !strncasecmp(s, p, strlen(p))) return true;
  return false;
}

// Reconnait le frigo a son nom, et liste chaque nom vu une fois pour pouvoir le reperer
void noteFrigo(BLEAdvertisedDevice &dev, const char *name, int len) {
  if (USE_FRIDGE && isFrigoName(name, len)) {
    if (!frigoDev) {
      frigoDev = new BLEAdvertisedDevice(dev);
      Serial.printf("Frigo retenu : %.*s (%s)\n", len, name, dev.getAddress().toString().c_str());
    }
    if (dev.getAddress() == frigoDev->getAddress()) frigoSeen = millis();
  }
  static String names[24];
  static int nn = 0;
  for (int i = 0; i < nn; i++)
    if ((int)names[i].length() == len && !memcmp(names[i].c_str(), name, len)) return;
  if (nn == 24) return;
  names[nn++] = String(name, len);
  Serial.printf("Bluetooth vu : %.*s (%s)\n", len, name, dev.getAddress().toString().c_str());
}

// La bibliotheque ne decode pas les annonces (voir setup) : on lit nous-memes le nom
// et les donnees constructeur dans l'annonce brute (annonce + reponse au scan).
class ScanCb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    bool battery = false, named = false;
    const char *name = "sans nom";
    int nameLen = 8;
    const uint8_t *vm[4];  // donnees Victron, apres l'identifiant 0x02E1
    size_t vl[4], nv = 0;
    adFields(dev.getPayload(), dev.getPayloadLength(), [&](uint8_t type, const uint8_t *d, size_t n) {
      if (type == 0x08 || type == 0x09) {  // nom
        named = true;
        name = (const char *)d;
        nameLen = n;
        battery |= USE_BATTERY && n == strlen(BATTERY_NAME) && !memcmp(d, BATTERY_NAME, n);
      } else if (type == 0xFF && n > 2 && d[0] == 0xE1 && d[1] == 0x02 && nv < 4) {  // Victron
        vm[nv] = d + 2;
        vl[nv++] = n - 2;
      }
    });
    if (battery) {
      if (!target && !connected && millis() > retryAfter) target = new BLEAdvertisedDevice(dev);
      return;
    }
    if (named) noteFrigo(dev, name, nameLen);
    if (!nv) return;
    String addr = dev.getAddress().toString().c_str();
    bool known = false;
    for (auto &v : vic) {
      if (!v.on || !sameMac(addr, v.mac)) continue;
      known = true;
      VData d;
      bool ok = false;
      for (size_t k = 0; k < nv && !ok; k++) ok = victronDecode(vm[k], vl[k], v.k, d);
      if (ok) {
        v.d = d;
        v.seen = millis();
        dirty = true;
      }
      if (millis() - v.logged > 10000) {  // un message toutes les 10 s au plus
        v.logged = millis();
        if (ok) logVictron(v);
        // certaines annonces Victron ne contiennent pas de mesures : la cle n'est
        // signalee que si aucune mesure n'a pu etre lue depuis 30 s
        else if (!v.seen || millis() - v.seen > 30000) Serial.printf("%s: cle incorrecte (ou annonce non reconnue)\n", v.title);
      }
    }
    // Appareil Victron non configure : affiche son adresse pour la recopier
    static String shown[8];
    static int nshown = 0;
    if (known || vl[0] < 5 || vm[0][0] != 0x10) return;
    for (int i = 0; i < nshown; i++) if (shown[i] == addr) return;
    if (nshown < 8) shown[nshown++] = addr;
    Serial.printf("Victron vu : %s type=0x%02X (%.*s)\n", addr.c_str(), vm[0][4], nameLen, name);
  }
};

class ClientCb : public BLEClientCallbacks {
  void onConnect(BLEClient *) override {}
  void onDisconnect(BLEClient *) override {
    connected = false;
    dirty = true;
  }
};

void onNotify(BLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
  for (size_t i = 0; i < len; i++) {
    char c = (char)data[i];
    if (c == '~') frame = "";
    frame += c;
    if (c == '\r' && frame.startsWith("~")) {
      readyFrame = frame;
      frameReady = true;
      frame = "";
    }
  }
}

int hexByte(const String &s, int pos) {
  return (int)strtol(s.substring(pos, pos + 2).c_str(), nullptr, 16);
}

// Retourne true si la trame Creabest est valide et met a jour les mesures
bool parseFrame(const String &f) {
  Serial.print("RX: ");
  Serial.println(f);
  int n = f.length();
  if (n < 20 || f[n - 1] != '\r' || !f.startsWith("~22")) return false;

  // LRC : somme des caracteres ASCII entre '~' et la somme de controle
  uint32_t sum = 0;
  for (int i = 1; i < n - 5; i++) sum += (uint8_t)f[i];
  uint16_t lrc = (uint16_t)((sum ^ 0xFFFF) + 1);
  uint16_t got = (uint16_t)strtol(f.substring(n - 5, n - 1).c_str(), nullptr, 16);
  if (lrc != got) {
    Serial.printf("LRC KO: calc %04X recu %04X\n", lrc, got);
    return false;
  }

  // Octets decodes depuis l'hex (sans '~' ni '\r')
  int count = (n - 2) / 2;
  uint8_t msg[160];
  if (count > (int)sizeof(msg)) return false;
  for (int i = 0; i < count; i++) msg[i] = hexByte(f, 1 + i * 2);

  int cells = msg[6];
  int tempPos = 6 + cells * 2 + 1;
  if (tempPos >= count) return false;
  int temps = msg[tempPos];
  int p = tempPos + 2 * temps + 1;
  if (p + 9 > count) return false;

  // Temperature la plus haute des sondes
  temp = -100;
  for (int i = 0; i < temps; i++) {
    float t = (int16_t)((msg[tempPos + 1 + i * 2] << 8) | msg[tempPos + 2 + i * 2]) / 10.0f;
    if (t > temp) temp = t;
  }
  current = (int16_t)((msg[p] << 8) | msg[p + 1]) / 10.0f;
  voltage = ((msg[p + 2] << 8) | msg[p + 3]) / 10.0f;
  soc = min(100, (msg[p + 4] << 8) | msg[p + 5]);
  cycles = (msg[p + 7] << 8) | msg[p + 8];
  Serial.printf("SOC=%d%% U=%.1fV I=%.1fA T=%.1fC cycles=%d\n", soc, voltage, current, temp, cycles);
  return true;
}

// ---------- Dessin ----------
const int CX = 120, CY = 116, RO = 92, RI = 78;
const float ANG0 = 135, SPAN = 270;

// --- Anticrenelage : les pixels du bord melangent la couleur avec le fond ---
void blendPixel(int x, int y, uint16_t col, float a) {
  if (x < 0 || y < 0 || x >= W || y >= H || a <= 0) return;
  uint16_t *p = canvas->getFramebuffer() + y * W + x;
  if (a >= 1) {
    *p = col;
    return;
  }
  uint16_t c = *p;
  int r = ((c >> 11) & 31) + (((col >> 11) & 31) - ((c >> 11) & 31)) * a;
  int g = ((c >> 5) & 63) + (((col >> 5) & 63) - ((c >> 5) & 63)) * a;
  int b = (c & 31) + ((col & 31) - (c & 31)) * a;
  *p = (r << 11) | (g << 5) | b;
}

// Disque lisse
void smoothDisc(float cx, float cy, float rad, uint16_t col) {
  for (int y = floorf(cy - rad - 1); y <= cy + rad + 1; y++)
    for (int x = floorf(cx - rad - 1); x <= cx + rad + 1; x++)
      blendPixel(x, y, col, rad + 0.5f - hypotf(x - cx, y - cy));
}

// atan2 en degres, approche (erreur < 0,001 degre) : l'ESP32-C6 n'a pas d'unite flottante
float fastAtan2Deg(float y, float x) {
  float ax = fabsf(x), ay = fabsf(y), mx = fmaxf(ax, ay);
  if (mx == 0) return 0;
  float a = fminf(ax, ay) / mx, s = a * a;
  float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
  if (ay > ax) r = 1.57079637f - r;
  if (x < 0) r = 3.14159274f - r;
  if (y < 0) r = -r;
  return r * 57.2957795f;
}

// Texte centre horizontalement, y = ligne de base
void centered(const char *s, int cx, int y, const uint8_t *font, uint16_t col) {
  int16_t x1, y1;
  uint16_t w, h;
  gfx->setFont(font);
  gfx->setTextColor(col);
  gfx->getTextBounds(s, 0, y, &x1, &y1, &w, &h);
  gfx->setCursor(cx - w / 2 - x1, y);
  gfx->print(s);
}

// Fond, titre avec pastille d'etat, points de pagination
void frameUi(const char *title, bool live) {
  gfx->fillScreen(C_BG);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->setFont(u8g2_font_helvR10_tf);
  gfx->getTextBounds(title, 0, 19, &x1, &y1, &w, &h);
  int x = CX - (w + 15) / 2;
  smoothDisc(x + 4, 14, 4, live ? C_GREEN : C_AMBER);
  gfx->setTextColor(C_DIM);
  gfx->setCursor(x + 15 - x1, 19);
  gfx->print(title);
  for (int i = 0; i < npages; i++) {
    int dx = CX + (i * 2 - (npages - 1)) * 7;
    smoothDisc(dx, 277, 3, i == cur ? C_TEXT : C_TRACK);
  }
}

// Anneau lisse en une passe : piste grise sur SPAN degres puis valeur (ratio 0 a 1,
// negatif = vide), bouts arrondis. Chaque pixel est couvert selon sa distance a la
// ligne centrale de l'arc, ce qui adoucit les bords (anticrenelage).
void ring(float ratio, uint16_t col, int cx, int cy, int ro, int ri) {
  const float rm = (ro + ri) / 2.0f, hw = (ro - ri) / 2.0f, rin = ri - 1, rout = ro + 1;
  const bool val = ratio > 0;
  const float span = val ? SPAN * fminf(ratio, 1) : 0;
  // centres des bouts arrondis : debut commun, fin de piste, fin de valeur
  const float sx = cx + rm * cosf(ANG0 * DEG_TO_RAD), sy = cy + rm * sinf(ANG0 * DEG_TO_RAD);
  const float tx = cx + rm * cosf((ANG0 + SPAN) * DEG_TO_RAD), ty = cy + rm * sinf((ANG0 + SPAN) * DEG_TO_RAD);
  const float vx = cx + rm * cosf((ANG0 + span) * DEG_TO_RAD), vy = cy + rm * sinf((ANG0 + span) * DEG_TO_RAD);
  for (int y = cy - ro - 1; y <= cy + ro + 1; y++)
    for (int x = cx - ro - 1; x <= cx + ro + 1; x++) {
      float px = x - cx, py = y - cy, r2 = px * px + py * py;
      if (r2 < rin * rin || r2 > rout * rout) continue;  // hors de l'anneau
      float t = fastAtan2Deg(py, px) - ANG0;
      while (t < 0) t += 360;
      float dr = fabsf(sqrtf(r2) - rm);  // distance a l'arc
      float ds = (t > SPAN || (val && t > span)) ? hypotf(x - sx, y - sy) : 0;
      blendPixel(x, y, C_TRACK, hw + 0.5f - (t <= SPAN ? dr : fminf(ds, hypotf(x - tx, y - ty))));
      if (val) blendPixel(x, y, col, hw + 0.5f - (t <= span ? dr : fminf(ds, hypotf(x - vx, y - vy))));
    }
}

void gauge(float ratio, uint16_t col) { ring(ratio, col, CX, CY, RO, RI); }

// Trait lisse d'epaisseur w, bouts arrondis
void smoothLine(float x0, float y0, float x1, float y1, float w, uint16_t col) {
  const float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy, hw = w / 2;
  for (int y = floorf(fminf(y0, y1) - hw - 1); y <= fmaxf(y0, y1) + hw + 1; y++)
    for (int x = floorf(fminf(x0, x1) - hw - 1); x <= fmaxf(x0, x1) + hw + 1; x++) {
      float t = l2 ? fmaxf(0, fminf(1, ((x - x0) * dx + (y - y0) * dy) / l2)) : 0;
      blendPixel(x, y, col, hw + 0.5f - hypotf(x - x0 - t * dx, y - y0 - t * dy));
    }
}

// Fleche de flux de (x0,y0) vers (x1,y1) ; trait gris sans pointe si inactive
void flowArrow(float x0, float y0, float x1, float y1, bool on, uint16_t col) {
  smoothLine(x0, y0, x1, y1, 3, on ? col : C_TRACK);
  if (!on) return;
  float l = hypotf(x1 - x0, y1 - y0), ux = (x1 - x0) / l, uy = (y1 - y0) / l;
  smoothLine(x1, y1, x1 - 6 * ux - 6 * uy, y1 - 6 * uy + 6 * ux, 3, col);
  smoothLine(x1, y1, x1 - 6 * ux + 6 * uy, y1 - 6 * uy - 6 * ux, 3, col);
}

// Largeur d'un texte dans une police
uint16_t textWidth(const char *s, const uint8_t *font) {
  int16_t x1, y1;
  uint16_t w, h;
  gfx->setFont(font);
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return w;
}

// Nombre suivi de son unite, centres ensemble sur cx (y = ligne de base)
// Ecart et centrage comptes sur les pixels dessines : le 1 et le signe moins demarrent loin du curseur
#define UNIT_GAP 6
void valueUnit(const char *num, const char *unit, int cx, int y, const uint8_t *font) {
  int16_t nx, ux, y1;
  uint16_t nw, uw, h;
  gfx->setFont(u8g2_font_helvB14_tf);
  gfx->getTextBounds(unit, 0, 0, &ux, &y1, &uw, &h);
  gfx->setFont(font);
  gfx->getTextBounds(num, 0, 0, &nx, &y1, &nw, &h);
  const int left = cx - (nw + UNIT_GAP + uw) / 2;
  gfx->setTextColor(C_TEXT);
  gfx->setCursor(left - nx, y);
  gfx->print(num);
  gfx->setFont(u8g2_font_helvB14_tf);
  gfx->setTextColor(C_DIM);
  gfx->setCursor(left + nw + UNIT_GAP - ux, y);
  gfx->print(unit);
}

// Grand nombre au centre de l'anneau, police plus petite s'il ne tient pas
void bigValue(const char *num, const char *unit) {
  bool fits = textWidth(num, u8g2_font_logisoso42_tf) + UNIT_GAP + textWidth(unit, u8g2_font_helvB14_tf) <= 136;
  valueUnit(num, unit, CX, CY + 14, fits ? u8g2_font_logisoso42_tf : u8g2_font_logisoso32_tf);
}

void subline(const char *s, uint16_t col) { centered(s, CX, CY + 40, u8g2_font_helvR10_tf, col); }

void twoValues(const char *l, uint16_t lc, const char *r, uint16_t rc) {
  gfx->drawFastHLine(36, 212, 168, C_TRACK);
  centered(l, 78, 240, u8g2_font_helvB14_tf, lc);
  centered(r, 162, 240, u8g2_font_helvB14_tf, rc);
  gfx->drawFastVLine(CX, 222, 24, C_TRACK);
}

void footer(const char *s) { centered(s, CX, 264, u8g2_font_helvR10_tf, C_DIM); }

// Valeur formatee, "--" si indisponible
const char *num(char *b, size_t n, const char *f, float v) {
  if (isnan(v)) snprintf(b, n, "--");
  else snprintf(b, n, f, v);
  return b;
}

uint16_t socColor(float s) { return s > 50 ? C_GREEN : (s > 20 ? C_AMBER : C_RED); }

const char *flowText(float i) { return i > 0.5 ? "En charge" : (i < -0.5 ? "Décharge" : "Au repos"); }
uint16_t flowColor(float i) { return i > 0.5 ? C_GREEN : (i < -0.5 ? C_AMBER : C_DIM); }

const char *stateName(uint8_t s) {
  switch (s) {
    case 0: return "Arrêt";
    case 1: return "Basse consommation";
    case 2: return "Défaut";
    case 3: return "Bulk";
    case 4: return "Absorption";
    case 5: return "Float";
    case 6: return "Stockage";
    case 7: return "Égalisation";
    case 9: return "Onduleur";
    case 11: return "Alimentation";
    case 245: return "Démarrage";
    case 246: return "Absorption répétée";
    case 247: return "Reconditionnement";
    case 248: return "BatterySafe";
    case 249: return "Actif";
    case 252: return "Contrôle externe";
    default: return "État inconnu";
  }
}
uint16_t stateColor(const VData &d) {
  if (d.error || d.state == 2) return C_RED;
  return (d.state >= 3 && d.state <= 7) || d.state == 246 || d.state == 249 ? C_GREEN : C_DIM;
}

void drawBattery() {
  char buf[32];
  frameUi("Batterie", connected && soc >= 0);
  if (soc < 0) {
    gauge(-1, 0);
    centered("--", CX, CY + 16, u8g2_font_logisoso42_tf, C_DIM);
    subline(bleStatus, C_DIM);
    return;
  }
  gauge(soc / 100.0f, socColor(soc));
  snprintf(buf, sizeof(buf), "%d", soc);
  bigValue(buf, "%");
  subline(flowText(current), flowColor(current));
  char l[16], r[16];
  snprintf(l, sizeof(l), "%.1f V", voltage);
  snprintf(r, sizeof(r), "%+.1f A", current);
  twoValues(l, C_TEXT, r, flowColor(current) == C_DIM ? C_TEXT : flowColor(current));
  snprintf(buf, sizeof(buf), "%.0f°C   |   %d cycles", temp, cycles);
  footer(buf);
}

// Donnees recues il y a moins de 30 s
bool fresh(const Victron &v) { return v.seen && millis() - v.seen < 30000; }

const VData *liveVic(uint8_t type) {
  for (auto &v : vic)
    if (fresh(v) && v.d.type == type) return &v.d;
  return nullptr;
}

// Flux d'energie : sources en haut, batterie au centre, consommation en bas
void drawSummary() {
  const int cy = 146;
  const VData *sol = liveVic(VIC_SOLAR), *alt = liveVic(VIC_ORION_XS), *bmv = liveVic(VIC_BMV);
  const bool cbt = connected && soc >= 0;
  // Batterie : BMV-712 en priorite, sinon BMS Creabest
  float s = bmv ? bmv->soc : NAN, net = bmv ? bmv->voltage * bmv->current : NAN;
  if (isnan(s) && cbt) s = soc;
  if (isnan(net) && cbt) net = voltage * current;
  float pv = sol ? sol->power : NAN;                        // puissance des panneaux
  float pvOut = sol ? sol->voltage * sol->current : NAN;    // ce que le MPPT envoie
  float altW = alt ? alt->voltage * alt->current : NAN;
  // Consommation calculee : sources - charge de la batterie (chargeurs et appareils
  // cote systeme du shunt BMV)
  float load = (isnan(pvOut) ? 0 : pvOut) + (isnan(altW) ? 0 : altW) - net;
  if (load < 0) load = 0;  // bruit de mesure (NaN reste NaN)
  const bool pvOn = pv > 0.5f, altOn = altW > 0.5f, loadOn = load > 0.5f;

  char a[16];
  frameUi("Synthèse", !isnan(s));
  centered("Solaire", 54, 46, u8g2_font_helvR10_tf, C_DIM);
  centered(num(a, sizeof(a), "%.0f W", pv), 54, 70, u8g2_font_helvB14_tf, pvOn ? C_AMBER : C_DIM);
  centered("Alternateur", 186, 46, u8g2_font_helvR10_tf, C_DIM);
  centered(num(a, sizeof(a), "%.0f W", altW), 186, 70, u8g2_font_helvB14_tf, altOn ? C_BLUE : C_DIM);
  flowArrow(54, 80, 80, 106, pvOn, C_AMBER);
  flowArrow(186, 80, 160, 106, altOn, C_BLUE);

  if (isnan(s)) {
    ring(-1, 0, CX, cy, 50, 42);
    centered("--", CX, cy + 12, u8g2_font_logisoso32_tf, C_DIM);
  } else {
    ring(s / 100, socColor(s), CX, cy, 50, 42);
    snprintf(a, sizeof(a), "%.0f", s);
    valueUnit(a, "%", CX, cy + 12, u8g2_font_logisoso32_tf);
  }
  if (!isnan(net)) {
    snprintf(a, sizeof(a), "%+.0f W", net);
    centered(a, CX, cy + 32, u8g2_font_helvR10_tf, net > 5 ? C_GREEN : (net < -5 ? C_AMBER : C_DIM));
  }

  flowArrow(CX, cy + 42, CX, cy + 68, loadOn, C_TEXT);
  centered(num(a, sizeof(a), "%.0f W", load), CX, 238, u8g2_font_helvB14_tf, loadOn ? C_TEXT : C_DIM);
  centered("Consommation", CX, 260, u8g2_font_helvR10_tf, C_DIM);
}

void drawVictron(Victron &v) {
  char buf[40], l[16], r[16];
  bool live = fresh(v);
  frameUi(v.title, live);
  const VData &d = v.d;
  if (!v.seen) {
    gauge(-1, 0);
    centered("--", CX, CY + 16, u8g2_font_logisoso42_tf, C_DIM);
    subline("En attente du signal", C_DIM);
    return;
  }

  char a[16], b[16];
  if (d.type == VIC_SOLAR) {
    gauge(d.power / PV_MAX_W, C_AMBER);
    bigValue(num(buf, sizeof(buf), "%.0f", d.power), "W");
    if (d.error) snprintf(buf, sizeof(buf), "Erreur %d", d.error);
    subline(d.error ? buf : stateName(d.state), stateColor(d));
    twoValues(num(l, sizeof(l), "%.2f V", d.voltage), C_TEXT,
              num(r, sizeof(r), "%.1f A", d.current), d.current > 0.05 ? C_GREEN : C_TEXT);
    snprintf(buf, sizeof(buf), "Aujourd'hui  %s", num(a, sizeof(a), "%.2f kWh", d.yieldKWh));
    footer(buf);
  } else if (d.type == VIC_BMV) {
    gauge(d.soc / 100.0f, socColor(d.soc));  // la jauge reste l'etat de charge
    bigValue(num(buf, sizeof(buf), "%.0f", d.voltage * d.current), "W");
    if (d.current < -0.5 && d.remainingMin >= 0) {
      snprintf(buf, sizeof(buf), "Autonomie %d h %02d", d.remainingMin / 60, d.remainingMin % 60);
      subline(buf, C_AMBER);
    } else {
      subline(flowText(d.current), flowColor(d.current));
    }
    twoValues(num(l, sizeof(l), "%.2f V", d.voltage), C_TEXT,
              num(r, sizeof(r), "%.0f %%", d.soc), isnan(d.soc) ? C_TEXT : socColor(d.soc));
    num(a, sizeof(a), "%.1f Ah", d.consumedAh);
    if (d.auxMode == 0) snprintf(buf, sizeof(buf), "%s   |   Dém. %s", a, num(b, sizeof(b), "%.2f V", d.aux));
    else if (d.auxMode == 1) snprintf(buf, sizeof(buf), "%s   |   Milieu %s", a, num(b, sizeof(b), "%.2f V", d.aux));
    else if (d.auxMode == 2) snprintf(buf, sizeof(buf), "%s   |   %s", a, num(b, sizeof(b), "%.0f°C", d.aux));
    else snprintf(buf, sizeof(buf), "Consommé  %s", a);
    footer(buf);
  } else if (d.type == VIC_ORION_XS) {
    gauge(d.current / ORION_MAX_A, C_BLUE);
    bigValue(num(buf, sizeof(buf), "%.1f", d.current), "A");
    if (d.error) snprintf(buf, sizeof(buf), "Erreur %d", d.error);
    subline(d.error ? buf : stateName(d.state), stateColor(d));
    twoValues(num(l, sizeof(l), "%.2f V", d.voltage), C_TEXT,
              num(r, sizeof(r), "%.0f W", d.voltage * d.current), C_TEXT);
    snprintf(buf, sizeof(buf), "Entrée  %s   |   %s", num(a, sizeof(a), "%.2f V", d.inVoltage),
             num(b, sizeof(b), "%.1f A", d.inCurrent));
    footer(buf);
  }
}

// Reglage de luminosite affiche en surimpression pendant 1,5 s
void drawFrigo() {
  char buf[40], l[16];
  const char *unit = frigo.fahrenheit ? "°F" : "°C";
  frameUi("Frigo", frigoRead && millis() - frigoRead < 3 * FRIGO_PERIOD_MS);
  if (!frigoRead) {
    gauge(-1, 0);
    centered("--", CX, CY + 16, u8g2_font_logisoso42_tf, C_DIM);
    subline("Recherche du frigo", C_DIM);
    return;
  }
  const int over = frigo.current - frigo.target;  // degres au-dessus de la cible
  const uint16_t col = !frigo.on ? C_DIM : (over <= 3 ? C_ICE : (over <= 8 ? C_AMBER : C_RED));
  const int range = frigo.tempMax - frigo.tempMin;
  gauge(frigo.on && range > 0 ? (float)(frigo.current - frigo.tempMin) / range : -1, col);
  snprintf(buf, sizeof(buf), "%d", frigo.current);
  bigValue(buf, unit);
  snprintf(buf, sizeof(buf), "Cible %d %s", frigo.target, unit);
  subline(frigo.on ? buf : "Arrêt", col);
  twoValues(num(l, sizeof(l), "%.1f V", frigo.volts), C_TEXT, frigo.eco ? "ECO" : "MAX", !frigo.on ? C_DIM : (frigo.eco ? C_GREEN : C_AMBER));
  static const char *const SAVER[] = {"faible", "moyenne", "élevée"};
  if (frigo.hasZone2) snprintf(buf, sizeof(buf), "Freezer %d %s   |   Cible %d %s", frigo.current2, unit, frigo.target2, unit);
  else snprintf(buf, sizeof(buf), "Protection batterie %s", frigo.batSaver < 3 ? SAVER[frigo.batSaver] : "?");
  footer(buf);
}

void brightnessOverlay() {
  if (millis() >= overlayUntil) return;
  char buf[24];
  gfx->fillRoundRect(34, 70, 172, 90, 14, RGB565(58, 58, 66));
  snprintf(buf, sizeof(buf), "Luminosité %d %%", LEVELS[level]);
  centered(buf, CX, 104, u8g2_font_helvR10_tf, C_TEXT);
  gfx->fillRoundRect(52, 122, 136, 10, 5, C_BG);
  gfx->fillRoundRect(52, 122, 136 * LEVELS[level] / 100, 10, 5, C_TEXT);
}

unsigned long tRender = 0, tFlush = 0;  // duree du dernier dessin (ms)

void draw() {
  unsigned long t0 = millis();
  int p = pages[cur];
  if (p == -2) drawFrigo();
  else if (p < 0) drawSummary();
  else if (p == 0) drawBattery();
  else drawVictron(vic[p - 1]);
  brightnessOverlay();
  unsigned long t1 = millis();
  gfx->flush();
  tRender = t1 - t0;
  tFlush = millis() - t1;
}

// ---------- Ecran et tactile ----------
void setBacklight() {
  analogWrite(LCD_BL, sleeping ? 0 : LEVELS[level] * 255 / 100);
}

// Le tactile et les boutons sont lus toutes les 10 ms dans une tache a part, meme
// pendant un dessin ; leurs actions passent par une file et loop() les applique.
enum : uint8_t { EV_NEXT, EV_PREV, EV_UP, EV_DOWN, EV_WAKE, EV_SCREEN };
const char *const EV_NAME[] = {"page suivante", "page precedente", "plus lumineux", "moins lumineux", "reveil", "ecran"};
QueueHandle_t events = nullptr;
bool pmuOk = false;

void post(uint8_t ev) { xQueueSend(events, &ev, 0); }

bool i2cRead(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(addr, n) != n) return false;
  for (int i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

bool i2cWrite(uint8_t addr, uint8_t reg, uint8_t v) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(v);
  return Wire.endTransmission() == 0;
}

// Un glissement est decide des que le doigt a parcouru 40 points, sans attendre qu'il se leve
void touchStep() {
  static bool down = false, done = false;
  static int x0 = 0, y0 = 0, x = 0, y = 0, n = 0;
  static uint8_t gest = 0;
  static const char *act = "";
  uint8_t b[6];  // geste, nombre de doigts, X (poids fort sur 4 bits, poids faible), Y
  bool ok = i2cRead(TP_ADDR, 0x01, b, 6);
  if (ok && b[0]) gest = b[0];  // geste reconnu par la puce : seulement note dans le journal
  if (ok && (b[1] & 0x0F)) {
    x = (b[2] & 0x0F) << 8 | b[3];
    y = (b[4] & 0x0F) << 8 | b[5];
    if (!down) {
      down = true;
      x0 = x;
      y0 = y;
      n = 0;
      gest = 0;
      done = sleeping;  // l'appui qui rallume l'ecran ne fait rien d'autre
      act = EV_NAME[EV_WAKE];
      if (done) post(EV_WAKE);
    }
    n++;
    int dx = x - x0, dy = y - y0;
    if (!done && (abs(dx) > 40 || abs(dy) > 40)) {
      uint8_t ev = abs(dy) > abs(dx) ? (dy < 0 ? EV_UP : EV_DOWN) : (dx > 0 ? EV_PREV : EV_NEXT);
      post(ev);
      act = EV_NAME[ev];
      done = true;
    }
    return;
  }
  if (!down) return;
  down = false;
  if (!done) {  // simple appui
    post(EV_NEXT);
    act = EV_NAME[EV_NEXT];
  }
  Serial.printf("Tactile : %d,%d -> %d,%d, %d mesures, geste 0x%02X -> %s\n", x0, y0, x, y, n, gest, act);
}

void buttonsStep() {
  static uint8_t boot = 0, tick = 0;
  if (digitalRead(BTN_BOOT) == LOW) {
    if (boot < 2 && ++boot == 2) {  // 2 lectures de suite : anti-rebond
      Serial.println("Bouton BOOT");
      post(EV_NEXT);
    }
  } else {
    boot = 0;
  }
  if (!pmuOk || ++tick < 5) return;  // AXP2101 lu toutes les 50 ms
  tick = 0;
  uint8_t st;
  if (i2cRead(PMU_ADDR, 0x49, &st, 1) && (st & 0x08)) {  // 0x49 bit 3 : appui court sur PWR
    i2cWrite(PMU_ADDR, 0x49, 0x08);                      // efface ce seul indicateur
    Serial.println("Bouton PWR");
    post(EV_SCREEN);
  }
}

void inputTask(void *) {
  for (;;) {
    touchStep();
    buttonsStep();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// Applique une action (dans loop) ; en veille, toute action ne fait que rallumer l'ecran
void handle(uint8_t ev) {
  lastTouch = millis();
  if (sleeping) {
    sleeping = false;
    setBacklight();
    uiDirty = true;
    return;
  }
  if (ev == EV_SCREEN) {
    sleeping = true;
    setBacklight();
    return;
  }
  if (ev == EV_NEXT || ev == EV_PREV) cur = (cur + (ev == EV_PREV ? -1 : 1) + npages) % npages;
  if (ev == EV_UP || ev == EV_DOWN) {
    level = constrain(level + (ev == EV_UP ? 1 : -1), 0, NLEVELS - 1);
    setBacklight();
    prefs.putUChar("level", level);
    overlayUntil = millis() + 1500;
  }
  uiDirty = true;
}

// ---------- Bluetooth ----------
void startScan() {
  scan->clearResults();
  scan->start(0, nullptr, false);  // en continu, en arriere-plan
}

bool connectBattery() {
  if (!client) {
    client = BLEDevice::createClient();
    client->setClientCallbacks(new ClientCb());
  }
  if (!client->connectTimeout(target, 5000)) return false;

  BLERemoteService *srx = client->getService(SVC_RX);
  BLERemoteService *stx = client->getService(SVC_TX);
  if (!srx || !stx) { client->disconnect(); return false; }
  BLERemoteCharacteristic *rx = srx->getCharacteristic(CHR_RX);
  tx = stx->getCharacteristic(CHR_TX);
  if (!rx || !tx || !rx->canNotify()) { client->disconnect(); return false; }
  rx->registerForNotify(onNotify);
  connected = true;
  return true;
}

void onFrigo(BLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
  Serial.print("Frigo RX:");
  for (size_t i = 0; i < len; i++) Serial.printf(" %02X", data[i]);
  Serial.println();
  if (frigoRx.feed(data, len, frigo)) {
    frigoRead = millis();
    dirty = true;
  }
}

// Une lecture : connexion, demande d'etat, deconnexion. Le frigo n'accepte qu'une connexion
// a la fois, le telephone peut donc s'y connecter le reste du temps.
void readFrigo() {
  if (!frigoClient) frigoClient = BLEDevice::createClient();
  if (!frigoClient->connectTimeout(frigoDev, 5000)) {
    Serial.println("Frigo : connexion impossible");
    return;
  }
  BLERemoteService *svc = frigoClient->getService(SVC_FRIGO);
  BLERemoteCharacteristic *wr = svc ? svc->getCharacteristic(CHR_FRIGO_W) : nullptr;
  BLERemoteCharacteristic *nt = svc ? svc->getCharacteristic(CHR_FRIGO_N) : nullptr;
  if (wr && nt && nt->canNotify()) {
    const unsigned long before = frigoRead;
    frigoRx.n = 0;
    nt->registerForNotify(onFrigo);
    for (int i = 0; i < 3 && frigoRead == before && frigoClient->isConnected(); i++) {
      wr->writeValue((uint8_t *)FRIGO_QUERY, sizeof(FRIGO_QUERY), true);
      for (int t = 0; t < 50 && frigoRead == before; t++) vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (frigoRead == before) Serial.println("Frigo : pas de reponse valide");
    else Serial.printf("Frigo : %d (cible %d) %.1fV marche=%d eco=%d\n", frigo.current, frigo.target, frigo.volts, frigo.on, frigo.eco);
  } else {
    Serial.println("Frigo : service 1234 absent, protocole different. Services :");
    for (auto &s : *frigoClient->getServices()) Serial.printf("  %s\n", s.first.c_str());
  }
  frigoClient->disconnect();
}

// Connexion a la batterie et commandes, dans une tache a part : une connexion
// lente (jusqu'a 5 s) ne fige plus l'ecran ni le tactile
void bleTask(void *) {
  unsigned long lastCmd = 0, lastScan = millis(), lastFrigo = 0;
  for (;;) {
    if (!connected && target) {
      bleStatus = "Connexion...";
      dirty = true;
      scan->stop();  // on ne peut pas se connecter pendant un scan
      if (connectBattery()) {
        bleStatus = "Connecté";
        lastCmd = 0;
      } else {
        Serial.println("Connexion echouee");
        bleStatus = "Recherche...";
        retryAfter = millis() + 10000;
        delete target;
        target = nullptr;
      }
      startScan();
      lastScan = millis();
      dirty = true;
    }

    // Vide la liste des appareils vus de temps en temps (memoire)
    if (millis() - lastScan > 60000) {
      lastScan = millis();
      scan->stop();
      startScan();
    }

    // Le frigo cesse d'emettre quand le telephone y est connecte : pas d'annonce recente, pas d'essai
    if (frigoDev && millis() - frigoSeen < 30000 && (!lastFrigo || millis() - lastFrigo > FRIGO_PERIOD_MS)) {
      lastFrigo = millis();
      scan->stop();  // on ne peut pas se connecter pendant un scan
      readFrigo();
      startScan();
      lastScan = millis();
    }

    if (connected && (millis() - lastCmd > 5000 || lastCmd == 0)) {
      lastCmd = millis();
      tx->writeValue((uint8_t *)CMD_STATUS, strlen(CMD_STATUS), false);
    }

    if (frameReady) {
      frameReady = false;
      if (parseFrame(readyFrame)) dirty = true;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);  // n'attend jamais si personne ne lit le port USB
#endif
  prefs.begin("ecran", false);
  level = prefs.getUChar("level", NLEVELS - 1);
  if (level >= NLEVELS) level = NLEVELS - 1;
  setBacklight();
  lastTouch = millis();
  gfx->begin();
  gfx->setUTF8Print(true);  // accents et signe degre
  Wire.begin(TP_SDA, TP_SCL);
  pinMode(BTN_BOOT, INPUT_PULLUP);
  uint8_t v;
  pmuOk = i2cRead(PMU_ADDR, 0x41, &v, 1) && i2cWrite(PMU_ADDR, 0x41, v | 0x08);  // 0x41 bit 3 : signaler l'appui court sur PWR
  if (pmuOk) i2cWrite(PMU_ADDR, 0x49, 0x08);
  else Serial.println("AXP2101 introuvable : bouton PWR inactif");

  bool anyVic = false;
  for (size_t i = 0; i < NVIC; i++) {
    vic[i].on = vic[i].use && hexKey(vic[i].key, vic[i].k);
    anyVic |= vic[i].on;
    if (vic[i].use && !vic[i].on) Serial.printf("Cle invalide pour %s\n", vic[i].title);
  }
  if (anyVic) pages[npages++] = -1;  // synthese, seulement avec au moins un Victron
  if (USE_BATTERY) pages[npages++] = 0;
  for (size_t i = 0; i < NVIC; i++)
    if (vic[i].on) pages[npages++] = i + 1;
  if (USE_FRIDGE) pages[npages++] = -2;
  if (!npages) {
    Serial.println("Aucun equipement active dans config.h");
    pages[npages++] = 0;
  }
  draw();

  BLEDevice::init("");
  scan = BLEDevice::getScan();
  // Toutes les annonces, meme repetees, mais sans decodage par la bibliotheque : elle
  // ajoutait chaque annonce a la precedente et vidait la memoire en une minute environ
  scan->setAdvertisedDeviceCallbacks(new ScanCb(), true, false);
  scan->setActiveScan(true);
  startScan();

  events = xQueueCreate(8, 1);
  if (!events || xTaskCreate(inputTask, "entrees", 4096, nullptr, 3, nullptr) != pdPASS)
    Serial.println("Erreur : tactile et boutons inactifs (tache non creee)");
  if (xTaskCreate(bleTask, "bluetooth", 8192, nullptr, 1, nullptr) != pdPASS)
    Serial.println("Erreur : Bluetooth inactif (tache non creee)");
}

void loop() {
  static unsigned long lastDraw = 0, lastStat = 0;
  uint8_t ev;
  // Attend une action au plus 50 ms, puis traite toutes celles en attente
  if (!events) delay(50);
  else if (xQueueReceive(events, &ev, pdMS_TO_TICKS(50)) == pdTRUE) {
    do {
      handle(ev);
    } while (xQueueReceive(events, &ev, 0) == pdTRUE);
  }

  if (!sleeping && millis() - lastTouch > SLEEP_MS) {
    sleeping = true;
    setBacklight();
  }
  if (overlayUntil && millis() > overlayUntil) {  // efface la surimpression
    overlayUntil = 0;
    uiDirty = true;
  }

  // Redessine tout de suite apres une action, au plus 1 fois par seconde pour les
  // nouvelles mesures, et au moins toutes les 2 s
  if (!sleeping && (uiDirty || (dirty && millis() - lastDraw > 1000) || millis() - lastDraw > 2000)) {
    uiDirty = false;
    dirty = false;
    lastDraw = millis();
    draw();
  }

  if (millis() - lastStat > 10000) {
    lastStat = millis();
    Serial.printf("Dessin : %lu ms + envoi a l'ecran %lu ms, memoire libre %u octets\n", tRender, tFlush, (unsigned)ESP.getFreeHeap());
  }
}
