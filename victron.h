// Decodage des annonces Bluetooth "Instant Readout" Victron.
// Format repris de victron-ble (github.com/keshavdv/victron-ble).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

#ifdef ARDUINO
#include "mbedtls/aes.h"
static void aesEcb(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, key, 128);
  mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in, out);
  mbedtls_aes_free(&ctx);
}
#else
#include <CommonCrypto/CommonCryptor.h>  // test sur Mac
static void aesEcb(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
  size_t n;
  CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionECBMode, key, 16, nullptr, in, 16, out, 16, &n);
}
#endif

// Champs d'une annonce Bluetooth : suite de [longueur][type][donnees], la longueur
// comptant le type. Appelle f(type, donnees, taille des donnees) pour chaque champ complet.
template <class F>
static void adFields(const uint8_t *p, size_t len, F f) {
  for (size_t i = 0; p && i < len;) {
    size_t n = p[i];
    if (n == 0) {  // octet de bourrage
      i++;
      continue;
    }
    if (i + n >= len) break;  // champ tronque
    f(p[i + 1], p + i + 2, n - 1);
    i += n + 1;
  }
}

#define VIC_SOLAR 0x01
#define VIC_BMV 0x02
#define VIC_ORION_XS 0x0F

struct VData {
  uint8_t type = 0;
  uint8_t state = 255, error = 0;
  float voltage = NAN, current = NAN;  // batterie (MPPT, BMV), sortie (Orion XS)
  float power = NAN, yieldKWh = NAN;   // MPPT
  float soc = NAN, consumedAh = NAN, aux = NAN;  // BMV
  int auxMode = 3, remainingMin = -1;            // BMV
  float inVoltage = NAN, inCurrent = NAN;        // Orion XS
};

// Lecture de bits, poids faible d'abord
struct Bits {
  const uint8_t *d;
  int i = 0;
  uint32_t u(int n) {
    uint32_t v = 0;
    for (int k = 0; k < n; k++, i++) v |= (uint32_t)((d[i >> 3] >> (i & 7)) & 1) << k;
    return v;
  }
  int32_t s(int n) {
    uint32_t v = u(n);
    return (v & (1u << (n - 1))) ? (int32_t)(v | (~0u << n)) : (int32_t)v;
  }
};

static bool hexKey(const char *hex, uint8_t key[16]) {
  if (!hex || strlen(hex) != 32) return false;
  for (int i = 0; i < 16; i++) {
    unsigned b;
    char t[3] = {hex[2 * i], hex[2 * i + 1], 0};
    char *end;
    b = strtoul(t, &end, 16);
    if (*end) return false;
    key[i] = (uint8_t)b;
  }
  return true;
}

// p : donnees constructeur sans l'identifiant 0x02E1 (commence par 0x10)
static bool victronDecode(const uint8_t *p, size_t len, const uint8_t key[16], VData &o) {
  if (len < 9 || len > 8 + 32 || p[0] != 0x10 || p[7] != key[0]) return false;
  uint16_t iv = p[5] | (p[6] << 8);
  size_t n = len - 8;
  uint8_t d[32] = {0};
  for (size_t b = 0; b * 16 < n; b++) {  // AES-128-CTR, compteur petit-boutiste
    uint8_t ctr[16] = {0}, ks[16];
    uint32_t c = iv + b;
    ctr[0] = c;
    ctr[1] = c >> 8;
    ctr[2] = c >> 16;
    aesEcb(key, ctr, ks);
    for (size_t k = 0; k < 16 && b * 16 + k < n; k++) d[b * 16 + k] = p[8 + b * 16 + k] ^ ks[k];
  }

  o = VData();
  o.type = p[4];
  Bits r{d};
  if (o.type == VIC_SOLAR) {
    o.state = r.u(8);
    o.error = r.u(8);
    int32_t v = r.s(16), i = r.s(16);
    uint32_t y = r.u(16), pw = r.u(16);
    if (v != 0x7FFF) o.voltage = v / 100.0f;
    if (i != 0x7FFF) o.current = i / 10.0f;
    if (y != 0xFFFF) o.yieldKWh = y / 100.0f;  // unite 10 Wh
    if (pw != 0xFFFF) o.power = pw;
  } else if (o.type == VIC_BMV) {
    uint32_t rem = r.u(16);
    int32_t v = r.s(16);
    r.u(16);  // alarme
    uint32_t aux = r.u(16);
    o.auxMode = r.u(2);
    int32_t i = r.s(22);
    uint32_t ah = r.u(20), soc = r.u(10);
    if (rem != 0xFFFF) o.remainingMin = rem;
    if (v != 0x7FFF) o.voltage = v / 100.0f;
    if ((i & 0x3FFFFF) != 0x3FFFFF) o.current = i / 1000.0f;
    if (ah != 0xFFFFF) o.consumedAh = -(float)ah / 10.0f;
    if (soc != 0x3FF) o.soc = soc / 10.0f;
    if (o.auxMode == 0) o.aux = (int16_t)aux / 100.0f;      // batterie de demarrage (V)
    else if (o.auxMode == 1) o.aux = aux / 100.0f;          // point milieu (V)
    else if (o.auxMode == 2) o.aux = aux / 100.0f - 273.15f;  // temperature (C)
  } else if (o.type == VIC_ORION_XS) {
    o.state = r.u(8);
    o.error = r.u(8);
    uint32_t ov = r.u(16), oi = r.u(16), iv2 = r.u(16), ii = r.u(16);
    if (ov != 0xFFFF) o.voltage = ov / 100.0f;
    if (oi != 0xFFFF) o.current = oi / 10.0f;
    if (iv2 != 0xFFFF) o.inVoltage = iv2 / 100.0f;
    if (ii != 0xFFFF) o.inCurrent = ii / 10.0f;
  } else {
    return false;
  }
  return true;
}
