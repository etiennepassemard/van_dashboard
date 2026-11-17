// Modele de reglages : copier ce fichier en config.h puis le remplir.
#pragma once

// Equipements presents dans l'installation : 1 = oui, 0 = non.
// Un equipement a 0 n'a pas de page et n'est jamais contacte.

// Batterie LiFePO4 a BMS CBT Power (nom Bluetooth "VB...", par exemple Creabest)
#define USE_BATTERY 1
#define BATTERY_NAME "VB000000000"

// Frigo a compresseur famille Alpicool (applications BLIZZ-E, Alpicool...)
#define USE_FRIDGE 1
// Nom Bluetooth (A1-...), pas le surnom donne dans l'application. Vide "" = le premier appareil
// portant un nom habituel de ces frigos (voir "Frigo retenu" dans le Serial Monitor).
#define FRIGO_NAME ""
#define FRIGO_PERIOD_MS 60000UL  // intervalle entre deux lectures du frigo

// Appareils Victron (Instant Readout) : 2e champ 1 = present, 0 = absent.
// La page Synthese apparait des qu'un appareil Victron est present.
// Dans VictronConnect : appareil > Parametres (engrenage) > menu (3 points)
// > Infos produit > Instant readout via Bluetooth > Afficher.
// Recopier l'adresse MAC et la cle de chiffrement (32 caracteres).
Victron vic[] = {
  {"Solaire", 0, "aa:bb:cc:dd:ee:ff", ""},
  {"BMV-712", 0, "aa:bb:cc:dd:ee:ff", ""},
  {"Alternateur", 0, "aa:bb:cc:dd:ee:ff", ""},
};
#define PV_MAX_W 425    // puissance crete des panneaux (jauge MPPT)
#define ORION_MAX_A 50  // courant max de l'Orion XS (jauge)
#define SLEEP_MS (5UL * 60 * 1000)  // mise en veille apres 5 min sans toucher
