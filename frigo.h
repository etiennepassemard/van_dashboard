// Frigos a compresseur de la famille Alpicool (dont EZA Blizz-E, application BLIZZ-E).
// Format repris de github.com/jgalea/fridgekit et github.com/dandwhelan/Alpicool50l12vfridgefreezer.
#pragma once
#include <stdint.h>
#include <stddef.h>

// Seule commande envoyee : demande d'etat. Le croquis ne change aucun reglage du frigo.
static const uint8_t FRIGO_QUERY[] = {0xFE, 0xFE, 0x03, 0x01, 0x02, 0x00};

struct FrigoData {
  bool on = false, eco = false, fahrenheit = false;
  int batSaver = 0;               // protection batterie : 0 faible, 1 moyenne, 2 elevee
  int target = 0, current = 0;    // compartiment principal, dans l'unite choisie sur le frigo
  int tempMin = 0, tempMax = 0;   // limites de reglage
  float volts = 0;
  bool hasZone2 = false;          // deuxieme compartiment avec sonde
  int target2 = 0, current2 = 0;
};

// f : trame complete, FE FE LEN CMD parametres... somme (2 octets)
static bool frigoDecode(const uint8_t *f, size_t n, FrigoData &o) {
  if (n < 6 || f[0] != 0xFE || f[1] != 0xFE || f[2] != n - 3) return false;
  uint32_t sum = 0, got = f[n - 2] << 8 | f[n - 1];
  for (size_t i = 0; i < n - 2; i++) sum += f[i];
  if (got != sum && got != sum * 2) return false;  // certains frigos envoient le double
  const uint8_t *p = f + 4;
  const size_t np = n - 6;
  if (f[3] != 0x01 || np < 18 || p[14] == 0x80) return false;  // 0x80 = pas de sonde
  o = FrigoData();
  o.on = p[1];
  o.eco = p[2] == 1;
  o.batSaver = p[3];
  o.target = (int8_t)p[4];
  o.tempMax = (int8_t)p[5];
  o.tempMin = (int8_t)p[6];
  o.fahrenheit = p[9] == 1;
  o.current = (int8_t)p[14];
  o.volts = p[16] + p[17] / 10.0f;
  if (np > 26 && p[26] != 0x80) {
    o.hasZone2 = true;
    o.target2 = (int8_t)p[18];
    o.current2 = (int8_t)p[26];
  }
  return true;
}

// Reconstitue les trames : une reponse peut arriver en plusieurs notifications
struct FrigoRx {
  uint8_t b[64];
  size_t n = 0;
  // true si une reponse valide vient d'etre decodee dans o
  bool feed(const uint8_t *d, size_t len, FrigoData &o) {
    bool ok = false;
    for (size_t i = 0; i < len; i++) {
      if (n < 2 && d[i] != 0xFE) {  // cherche l'en-tete FE FE
        n = 0;
        continue;
      }
      b[n++] = d[i];
      if (n < 3) continue;
      const size_t total = b[2] + 3u;
      if (total > sizeof(b)) {
        n = 0;
      } else if (n == total) {
        ok |= frigoDecode(b, n, o);
        n = 0;
      }
    }
    return ok;
  }
};
