/**
 * ESP32 P10 RGB Fullscreen Countdown avec Interface Web
 * Compte à rebours plein écran avec réglages via interface web
 * 
 * Ce programme affiche un compte à rebours en plein écran et permet de configurer :
 * - La date/heure cible du compte à rebours
 * - La couleur du texte
 * - La taille de police
 * 
 * L'interface web est accessible à l'adresse http://ip_de_l'esp32/
 * 
 * Auteur: Clément Saillant (electron-rare)
 * Date: Août 2025
 */

 #define bridur 250

#define PxMATRIX_SPI_FREQUENCY 10000000

// Option d'optimisation démarrage : définir FAST_BOOT pour réduire fortement les délais init
#ifndef FAST_BOOT
#define FAST_BOOT 1
#endif

#if FAST_BOOT
#define BOOT_DELAY(ms) ((void)0)
#define BOOT_SPLASH_MS 300   // durée splash réduite
#define WIFI_STEP_DELAY(ms) vTaskDelay(pdMS_TO_TICKS(5))
#else
#define BOOT_DELAY(ms) delay(ms)
#define BOOT_SPLASH_MS 2000
#define WIFI_STEP_DELAY(ms) delay(ms)
#endif

#include <Arduino.h>
#include <PxMatrix.h>
#include <RTClib.h>
// Polices DejaVu avec support Latin-1 complet (accents et caractères spéciaux)
#include "DejaVuSans9ptLat1.h"      // Normal
#include "DejaVuSansBold9ptLat1.h"  // Gras
#include "DejaVuSansOblique9ptLat1.h" // Italique
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "esp_err.h"

// Version firmware (uniformisé avec main)
static const char* FIRMWARE_VERSION = "1.0.0"; // garder synchro avec src/main.cpp

// === Personnalisation Auteur / GitHub ===
static const char* AUTHOR_NAME = "Clément Saillant (electron-rare)"; 
static const char* GITHUB_URL = "https://github.com/electron-rare";   

// Pins pour la matrice LED
#define P_LAT 5
#define P_A   19
#define P_B   23
#define P_C   18
#define P_OE  4

// Définition des priorités des tâches (ajustées pour éviter les conflits WiFi)
#define TASK_DISPLAY_PRIORITY      2
#define TASK_WEBSERVER_PRIORITY    1  
#define TASK_COUNTDOWN_PRIORITY    1
#define TASK_NETWORK_PRIORITY      3  // Priorité plus élevée pour le réseau

// Taille des stacks pour les tâches
#define TASK_DISPLAY_STACK     4096
#define TASK_WEBSERVER_STACK   4096
#define TASK_COUNTDOWN_STACK   2048
#define TASK_NETWORK_STACK     4096

// Handles pour les tâches
TaskHandle_t displayTaskHandle = NULL;
TaskHandle_t webServerTaskHandle = NULL;
TaskHandle_t countdownTaskHandle = NULL;
TaskHandle_t networkTaskHandle = NULL;

// Mutex pour protéger les ressources partagées
SemaphoreHandle_t displayMutex;
SemaphoreHandle_t countdownMutex;
SemaphoreHandle_t preferencesMutex;

// Configuration des panneaux - définie par les build flags
#ifndef MATRIX_WIDTH
#define MATRIX_WIDTH 32
#endif

#ifndef MATRIX_HEIGHT
#define MATRIX_HEIGHT 16
#endif

#ifndef MATRIX_PANELS_X
#define MATRIX_PANELS_X 3
#endif

#ifndef MATRIX_PANELS_Y
#define MATRIX_PANELS_Y 1
#endif

// Calcul des dimensions totales
#define TOTAL_WIDTH (MATRIX_WIDTH * MATRIX_PANELS_X)
#define TOTAL_HEIGHT (MATRIX_HEIGHT * MATRIX_PANELS_Y)

// Configuration du timer
hw_timer_t * timer = nullptr;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

// Temps d'affichage ajusté selon le nombre de panneaux
uint8_t display_draw_time = (MATRIX_PANELS_X * MATRIX_PANELS_Y > 4) ? 20 : 30;

// Objet matrice
PxMATRIX display(TOTAL_WIDTH, TOTAL_HEIGHT, P_LAT, P_OE, P_A, P_B, P_C);

// RTC
RTC_DS3231 rtc;

// Préférences
Preferences preferences;

// Configuration WiFi - Modifiez selon vos besoins
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Configuration Point d'accès (si pas de WiFi)
// DNS pour le portail captif
const byte DNS_PORT = 53;
DNSServer dnsServer;
const char* ap_ssid = "HOKA_CLOCK";
const char* ap_password = "hokahoka";


// Mode de fonctionnement WiFi
bool useStationMode = false; // true = se connecter au WiFi, false = créer un point d'accès

// Serveur web
WebServer server(80);

// Couleurs prédéfinies
uint16_t myRED      = display.color565(255, 0, 0);
uint16_t myGREEN    = display.color565(0, 255, 0);
uint16_t myBLUE     = display.color565(0, 0, 255);
uint16_t myYELLOW   = display.color565(255, 255, 0);
uint16_t myCYAN     = display.color565(0, 255, 255);
uint16_t myMAGENTA  = display.color565(255, 0, 255);
uint16_t myWHITE    = display.color565(255, 255, 255);
uint16_t myBLACK    = display.color565(0, 0, 0);
uint16_t myORANGE   = display.color565(255, 165, 0);

// Prototypes des fonctions
void IRAM_ATTR display_updater();
void display_update_enable(bool is_enable); // prototype pour usage anticipé

// Fonction de vérification de la sanité des mutex
bool checkMutexSanity() {
  // Vérifier que les mutex sont valides
  if (displayMutex == NULL || countdownMutex == NULL || preferencesMutex == NULL) {
    Serial.println("ERROR: One or more mutex is NULL!");
    return false;
  }
  
  // Vérifier l'état du scheduler
  if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
    Serial.println("WARNING: Scheduler not running");
    return false;
  }
  
  // Vérifier que nous ne sommes pas dans une ISR
  if (xPortInIsrContext()) {
    Serial.println("WARNING: In ISR context");
    return false;
  }
  
  return true;
}

// Fonction de récupération d'urgence en cas de corruption
void emergencyRecovery() {
  Serial.println("EMERGENCY RECOVERY: System corruption detected");
  
  // Arrêter tous les timers
  if (timer != nullptr) {
    timerEnd(timer);
    timer = nullptr;
  }
  
  // Arrêter toutes les tâches
  if (displayTaskHandle != NULL) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = NULL;
  }
  if (countdownTaskHandle != NULL) {
    vTaskDelete(countdownTaskHandle);
    countdownTaskHandle = NULL;
  }
  if (networkTaskHandle != NULL) {
    vTaskDelete(networkTaskHandle);
    networkTaskHandle = NULL;
  }
  
  // Nettoyage des mutex
  if (displayMutex != NULL) {
    vSemaphoreDelete(displayMutex);
    displayMutex = NULL;
  }
  if (countdownMutex != NULL) {
    vSemaphoreDelete(countdownMutex);
    countdownMutex = NULL;
  }
  if (preferencesMutex != NULL) {
    vSemaphoreDelete(preferencesMutex);
    preferencesMutex = NULL;
  }
  
  // Délai avant redémarrage
  delay(1000);
  
  // Redémarrage du système
  ESP.restart();
}

// Fonction de réparation de la corruption NVS
void repairNVSCorruption() {
  Serial.println("Attempting NVS corruption repair...");
  
  // Fermer toutes les instances de preferences ouvertes
  preferences.end();
  
  // Attendre un peu
  delay(100);
  
  // Essayer de vider et réinitialiser la partition NVS
  esp_err_t err = nvs_flash_erase();
  if (err == ESP_OK) {
    Serial.println("NVS partition erased successfully");
    err = nvs_flash_init();
    if (err == ESP_OK) {
      Serial.println("NVS partition reinitialized successfully");
      return;
    }
  }
  
  Serial.println("NVS repair failed - will restart");
  delay(1000);
  ESP.restart();
}

// Utilitaire : copie tronquée en respectant les limites UTF-8 (nombre de caractères et non octets)
// maxChars : nombre maximum de caractères (glyphes) à conserver (excluant le nul final)
// destSize : taille du buffer destination (incluant place pour nul)
size_t utf8SafeCopyTruncate(const String &src, char *dest, size_t destSize, size_t maxChars) {
  if (destSize == 0) return 0;
  const uint8_t *bytes = (const uint8_t*)src.c_str();
  size_t i = 0;       // index source en octets
  size_t o = 0;       // index destination
  size_t chars = 0;   // compte de caractères
  while (bytes[i] != '\0' && chars < maxChars && o + 1 < destSize) {
    uint8_t c = bytes[i];
    size_t seqLen = 1;
    if ((c & 0x80) == 0x00) seqLen = 1;                // 0xxxxxxx
    else if ((c & 0xE0) == 0xC0) seqLen = 2;           // 110xxxxx
    else if ((c & 0xF0) == 0xE0) seqLen = 3;           // 1110xxxx
    else if ((c & 0xF8) == 0xF0) seqLen = 4;           // 11110xxx
    else { // octet invalide -> arrêter
      break;
    }
    // Vérifier que la séquence tient dans la source
    for (size_t k = 1; k < seqLen; ++k) {
      uint8_t nc = bytes[i + k];
      if ((nc & 0xC0) != 0x80) { // pas une continuation valide
        seqLen = 1; // tronquer au premier octet
        break;
      }
    }
    // Vérifier que ça rentre dans dest
    if (o + seqLen + 1 >= destSize) break;
    // Copier
    for (size_t k = 0; k < seqLen; ++k) {
      dest[o++] = (char)bytes[i + k];
    }
    i += seqLen;
    chars++;
  }
  dest[o] = '\0';
  return o; // octets copiés
}

// === Classe helper pour gestion automatique des mutex ===
class MutexGuard {
private:
  SemaphoreHandle_t mutex;
  bool acquired;
  
public:
  MutexGuard(SemaphoreHandle_t mtx, TickType_t timeout = pdMS_TO_TICKS(500)) : mutex(mtx), acquired(false) {
    if (mutex != NULL) {
      acquired = xSemaphoreTake(mutex, timeout) == pdTRUE;
    }
  }
  
  ~MutexGuard() {
    if (acquired && mutex != NULL) {
      xSemaphoreGive(mutex);
    }
  }
  
  bool isLocked() const { return acquired; }
  
  // Interdire la copie pour éviter double libération
  MutexGuard(const MutexGuard&) = delete;
  MutexGuard& operator=(const MutexGuard&) = delete;
};

// Macros pour simplifier l'utilisation
#define MUTEX_GUARD(mutex, timeout) MutexGuard guard_##mutex(mutex, timeout)
#define MUTEX_GUARD_CHECK(mutex, timeout) MutexGuard guard_##mutex(mutex, timeout); if (!guard_##mutex.isLocked())

// Watchdog pour surveiller la sanité du système
unsigned long lastWatchdogTime = 0;
int corruptionCounter = 0;

void systemWatchdog() {
  unsigned long now = millis();
  
  // Exécuter toutes les 30 secondes
  if (now - lastWatchdogTime > 30000) {
    lastWatchdogTime = now;
    
    // Vérifier la sanité des mutex
    if (!checkMutexSanity()) {
      corruptionCounter++;
      Serial.printf("System corruption detected #%d\n", corruptionCounter);
      
      // Si corruption répétée, déclencher une récupération d'urgence
      if (corruptionCounter >= 3) {
        Serial.println("Too many corruptions - triggering emergency recovery");
        emergencyRecovery();
      }
    } else {
      // Réinitialiser le compteur si tout va bien
      corruptionCounter = 0;
    }
    
    // Vérifier l'état des tâches
    if (displayTaskHandle == NULL || countdownTaskHandle == NULL || networkTaskHandle == NULL) {
      Serial.println("One or more tasks missing - system instability detected");
      corruptionCounter++;
    }
    
    // Imprimer les statistiques de mémoire
    Serial.printf("Free heap: %d bytes, Min free: %d bytes\n", 
                  ESP.getFreeHeap(), ESP.getMinFreeHeap());
  }
}
#define MUTEX_TIMEOUT_FAST     pdMS_TO_TICKS(50)   // Opérations rapides
#define MUTEX_TIMEOUT_NORMAL   pdMS_TO_TICKS(200)  // Opérations normales
#define MUTEX_TIMEOUT_SLOW     pdMS_TO_TICKS(1000) // Opérations lentes (I/O)
#define MUTEX_TIMEOUT_CRITICAL pdMS_TO_TICKS(2000) // Opérations critiques

// Prototypes des tâches
void DisplayTask(void * parameter);
void CountdownTask(void * parameter);
void NetWebTask(void * parameter);

// Variables pour le countdown
DateTime countdownTarget;
bool countdownExpired = false;
bool blinkLastSeconds = false;  // Clignotement pour les 10 dernières secondes
bool blinkState = true;
unsigned long lastBlinkTime = 0;
// Paramètres de clignotement configurables (pour les 10 dernières secondes)
bool blinkEnabled = true;       // 1 = clignote sur la fin du compte à rebours
int blinkIntervalMs = 500;      // Intervalle de clignotement en ms (modifiable via Web UI)
int blinkWindowSeconds = 10;    // Nombre de dernières secondes pendant lesquelles le clignotement est actif

// Marquee (défilement) pour le texte final si trop long
volatile bool marqueeActive = false; // indicateur global pour la tâche d'affichage
// Paramètres configurables et état du défilement
bool marqueeEnabled = true;              // activation auto si texte trop long
int marqueeIntervalMs = 40;              // intervalle ms entre déplacements (1 px)
int marqueeGap = 24;                     // espace en pixels avant répétition
int marqueeMode = 0;                     // 0=Auto (si overflow, scroll gauche), 1=Toujours gauche, 2=Aller-Retour, 3=Une fois
int marqueeReturnIntervalMs = 60;        // vitesse différente pour le retour (aller-retour)
int marqueeBouncePauseLeftMs = 400;      // pause extrémité gauche (ms)
int marqueeBouncePauseRightMs = 400;     // pause extrémité droite (ms)
int marqueeOneShotDelayMs = 800;         // délai centré avant départ (ms)
bool marqueeOneShotStopCenter = true;    // recadrer au centre à la fin
int marqueeOneShotRestartSec = 0;        // redémarrage automatique (0=pas de restart)
// Accélération progressive
bool marqueeAccelEnabled = false;
int marqueeAccelStartIntervalMs = 80;    // intervalle initial
int marqueeAccelEndIntervalMs = 30;      // intervalle final
int marqueeAccelDurationMs = 3000;       // durée interpolation sur un cycle (ms)
static int marqueeTextWidth = 0;         // largeur pixels du texte courant
static int marqueeOffset = 0;            // position X courante
static unsigned long lastMarqueeStep = 0; // dernière étape
static int marqueeDirection = -1;        // pour mode aller-retour
static bool marqueeOneShotDone = false;  // pour mode une fois
static bool marqueeInPause = false;      // pause extrémités bounce
static unsigned long marqueePauseUntil = 0; // fin pause
static bool marqueeOneShotCenterPhase = false; // phase centrée initiale
static unsigned long marqueeOneShotStart = 0;  // début phase centrée
static unsigned long marqueeOneShotRestartAt = 0; // moment de relance
static unsigned long marqueeCycleStartMs = 0;   // début cycle pour accélération
// Flag de forçage de recalcul layout (modifié via Web)
volatile bool forceLayout = false;
// Padding supplémentaire aux extrémités (espaces visuels entrée/sortie défilement)
int marqueeEdgePadding = 2; // pixels

// --- Normalisation accents (UTF-8 -> ASCII approximatif) ---
// Remplace les caractères accentués français communs par leur équivalent non accentué.
// Objectif: permettre un rendu cohérent avec les polices GFX limitées au Basic ASCII.
size_t foldAccents(const char *in, char *out, size_t outSize) {
  if (!in || !out || outSize == 0) return 0;
  size_t o = 0;
  while (*in && o + 1 < outSize) {
    unsigned char c = (unsigned char)*in;
    if (c < 0x80) {
      // ASCII direct
      out[o++] = *in++;
    } else if ((c == 0xC3) && in[1]) {
      unsigned char d = (unsigned char)in[1];
      char rep = 0; // remplacement
      switch (d) {
        case 0x80: case 0x81: rep='A'; break; // ÀÁ
        case 0x82: rep='A'; break; // Â
        case 0x84: rep='A'; break; // Ä
        case 0x87: rep='C'; break; // Ç
        case 0x88: case 0x89: rep='E'; break; // ÈÉ
        case 0x8A: case 0x8B: rep='E'; break; // ÊË
        case 0x8E: case 0x8F: rep='I'; break; // ÎÏ
        case 0x94: case 0x96: rep='O'; break; // ÔÖ
        case 0x99: case 0x9B: rep='U'; break; // ÙÛ
        case 0x9C: rep='U'; break; // Ü
        case 0xA0: case 0xA1: rep='a'; break; // àá
        case 0xA2: rep='a'; break; // â
        case 0xA4: rep='a'; break; // ä
        case 0xA7: rep='c'; break; // ç
        case 0xA8: case 0xA9: rep='e'; break; // è é
        case 0xAA: case 0xAB: rep='e'; break; // ê ë
        case 0xAE: case 0xAF: rep='i'; break; // î ï
        case 0xB4: case 0xB6: rep='o'; break; // ô ö
        case 0xB9: case 0xBB: rep='u'; break; // ù û
        case 0xBC: rep='u'; break; // ü
      }
      if (rep) {
        out[o++] = rep;
        in += 2;
      } else {
        // caractère non géré -> ignorer les deux bytes
        in += 2;
      }
    } else {
      // Séquence multioctet non gérée -> ignorer octet
      in++;
    }
  }
  out[o] = '\0';
  return o;
}

// Conversion UTF-8 -> Latin-1 (ISO-8859-1). Les caractères hors plage 0x00-0xFF
// (émojis, symboles hors Latin-1) sont remplacés par '?'.
// Nécessaire car la police étendue générée couvre 0x20-0xFF (octets simples) alors
// que l'entrée (depuis le formulaire web) est en UTF-8 multioctet.
size_t utf8ToLatin1(const char *in, char *out, size_t outSize) {
  if (!in || !out || outSize == 0) return 0;
  size_t o = 0;
  while (*in && o + 1 < outSize) {
    unsigned char c = (unsigned char)*in;
    if (c < 0x80) { // ASCII direct
      out[o++] = c; in++;
    } else if ((c & 0xE0) == 0xC0 && in[1]) { // 2 octets
      unsigned char c2 = (unsigned char)in[1];
      if ((c2 & 0xC0) == 0x80) {
        uint16_t code = ((c & 0x1F) << 6) | (c2 & 0x3F); // U+0080..U+07FF
        if (code <= 0x00FF) {
          out[o++] = (char)code;
        } else {
          out[o++] = '?';
        }
        in += 2;
      } else {
        out[o++] = '?'; in++;
      }
    } else if ((c & 0xF0) == 0xE0 && in[1] && in[2]) { // 3 octets
      unsigned char c2 = (unsigned char)in[1];
      unsigned char c3 = (unsigned char)in[2];
      if (((c2 & 0xC0) == 0x80) && ((c3 & 0xC0) == 0x80)) {
        uint16_t code = ((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
        if (code <= 0x00FF) {
          out[o++] = (char)code;
        } else {
          // Remplacements spécifiques ponctuation “smart” courante
          switch (code) {
            case 0x2018: // ‘
            case 0x2019: // ’
              out[o++] = '\''; break;
            case 0x201C: // “
            case 0x201D: // ”
              out[o++] = '"'; break;
            case 0x2013: // –
            case 0x2014: // —
              out[o++] = '-'; break;
            case 0x2026: // …
              if (o + 3 < outSize) { out[o++]='.'; out[o++]='.'; out[o++]='.'; }
              else out[o++]='.'; // fallback partiel si peu de place
              break;
            default:
              out[o++] = '?';
          }
        }
        in += 3;
      } else {
        out[o++] = '?'; in++;
      }
    } else { // 4 octets ou séquence invalide -> non représentable
      out[o++] = '?';
      // Skipper au moins 1 octet (si séquence UTF-8 valide on pourrait consommer tous, mais
      // pour simplicité on avance octet par octet sur séquences plus longues)
      in++;
    }
  }
  out[o] = '\0';
  return o;
}

// Variable pour indiquer qu'il faut sauvegarder les paramètres
volatile bool saveRequested = false;
volatile unsigned long saveRequestTime = 0;

// Paramètres configurables via l'interface web
int countdownYear = 2025;
int countdownMonth = 12;
int countdownDay = 31;
int countdownHour = 23;
int countdownMinute = 59;
int countdownSecond = 0;
char countdownTitle[51] = "COUNTDOWN";
// Polices DejaVu avec taille automatique et style configurable
int fontStyle = 0; // 0=Normal, 1=Gras, 2=Italique
int countdownColorR = 0;
int countdownColorG = 255;
int countdownColorB = 0;
uint16_t countdownColor;

// Paramètres du message de fin
int endMessageColorR = 255;
int endMessageColorG = 215;
int endMessageColorB = 0;
uint16_t endMessageColor;
int endMessageEffect = 0; // 0=static, 1=blink, 2=fade, 3=rainbow

int displayBrightness = 250; // -1 = auto (calculé selon nombre de panneaux)

// Format d'affichage (0=jours, 1=heures, 2=minutes, 3=secondes uniquement)
int displayFormat = 0;

// HTML Page
const char MAIN_page[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="fr"><head>
<meta charset="utf-8" />
<meta name="author" content="__AUTHOR_NAME__">
<title>ESP32 P10 Countdown</title>
<meta name="viewport" content="width=device-width,initial-scale=1" />
<style>
:root { --accent:#009688; --accent-hover:#017a6f; --danger:#d32f2f; --bg:#101418; --panel:#1c242b; --panel-alt:#232e36; --text:#e5ecec; --muted:#7d8b91; --ok:#4caf50; --warn:#ffc107; font-size:16px; }
* { box-sizing:border-box; }
body { margin:0; font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Arial,sans-serif; background:var(--bg); color:var(--text); -webkit-font-smoothing:antialiased; }
h1 { font-size:1.35rem; margin:0 0 1rem; text-align:center; letter-spacing:.5px; }
h2 { font-size:.95rem; margin:1.5rem 0 .75rem; text-transform:uppercase; letter-spacing:.08em; color:var(--muted); }
a { color:var(--accent); }
.wrap { max-width:760px; margin:0 auto; padding:clamp(12px,2vw,32px); }
.grid { display:grid; gap:14px; }
.card { background:linear-gradient(145deg,var(--panel) 0%,var(--panel-alt) 100%); padding:18px 20px 22px; border-radius:14px; position:relative; box-shadow:0 2px 4px rgba(0,0,0,.3),0 8px 32px -8px rgba(0,0,0,.45); overflow:hidden; }
form .field { display:flex; flex-direction:column; gap:4px; }
.field-group { display:flex; flex-direction:column; gap:8px; }
.field-group > label { font-size:.75rem; font-weight:600; letter-spacing:.06em; color:var(--accent); margin-bottom:4px; }
label { font-size:.70rem; font-weight:600; letter-spacing:.06em; text-transform:uppercase; color:var(--muted); }
input:not([type=color]), select { background:#0d1418; border:1px solid #233038; border-radius:8px; padding:10px 11px; font:inherit; color:var(--text); outline:none; transition:.2s border-color,.2s background; }
input:focus, select:focus { border-color:var(--accent); background:#111b20; }
.color-input-group { display:flex; align-items:center; gap:12px; }
.color-wheel-container { position:relative; width:120px; height:120px; }
.color-wheel { width:120px; height:120px; border-radius:50%; background:conic-gradient(from 0deg, #ff0000 0deg, #ffff00 60deg, #00ff00 120deg, #00ffff 180deg, #0000ff 240deg, #ff00ff 300deg, #ff0000 360deg); cursor:crosshair; position:relative; border:3px solid #333; box-shadow:0 4px 12px rgba(0,0,0,0.3); }
.color-wheel-inner { width:40px; height:40px; border-radius:50%; background:radial-gradient(circle, rgba(255,255,255,1) 0%, rgba(255,255,255,0) 70%); position:absolute; top:50%; left:50%; transform:translate(-50%, -50%); pointer-events:none; }
.color-wheel-cursor { width:16px; height:16px; border:3px solid #fff; border-radius:50%; position:absolute; transform:translate(-50%, -50%); pointer-events:none; box-shadow:0 2px 6px rgba(0,0,0,0.4); transition:all 0.1s ease; }
.color-preview { width:60px; height:60px; border-radius:12px; border:3px solid #333; background:var(--preview-color,#00ff00); box-shadow:0 2px 8px rgba(0,0,0,.3); position:relative; }
.color-preview::after { content:''; position:absolute; inset:3px; border-radius:8px; background:inherit; }
.auto-save-indicator { position:fixed; top:20px; right:20px; background:var(--panel); padding:8px 16px; border-radius:8px; font-size:0.8rem; color:var(--accent); box-shadow:0 4px 12px rgba(0,0,0,0.3); transform:translateX(100%); opacity:0; transition:all 0.3s ease; z-index:1000; }
.auto-save-indicator.show { transform:translateX(0); opacity:1; }
.auto-save-indicator.success { color:var(--ok); }
.auto-save-indicator.error { color:var(--danger); }
.inline { display:flex; gap:10px; }
.inline > * { flex:1; }
.actions { display:flex; flex-wrap:wrap; gap:10px; margin-top:8px; }
button { --b:var(--accent); flex:1; cursor:pointer; border:none; border-radius:10px; padding:12px 18px; font:600 .9rem/1 system-ui; letter-spacing:.04em; background:linear-gradient(135deg,var(--b) 0%, #00b39e 100%); color:#fff; box-shadow:0 3px 10px -2px rgba(0,0,0,.5); transition:.25s transform,.25s filter; }
button:hover { transform:translateY(-2px); filter:brightness(1.08); }
button:active { transform:translateY(0); filter:brightness(.92); }
button.primary { --b:var(--accent); background:linear-gradient(135deg,var(--accent),#00b39e); }
button.secondary { --b:#2d3a41; background:linear-gradient(135deg,#2d3a41,#37464f); }
button.danger { --b:var(--danger); background:linear-gradient(135deg,#d13737,#ef5350); }
.swatch { width:28px; height:28px; border-radius:6px; cursor:pointer; position:relative; box-shadow:0 0 0 2px #141c22, 0 0 0 4px rgba(0,0,0,.3); transition:.2s transform,.2s box-shadow; }
.swatch:hover { transform:scale(1.1); box-shadow:0 0 0 2px #1f2d35,0 0 0 5px rgba(0,0,0,.55); }
.swatch.active { outline:2px solid #fff; }
.preview-panel { padding:18px 12px 24px; border-radius:16px; background:#000; position:relative; display:flex; justify-content:center; align-items:center; }
.led-matrix { display:grid; grid-template-columns:repeat(96,4px); grid-template-rows:repeat(16,4px); gap:1px; background:#111; border:2px solid #333; padding:4px; border-radius:8px; }
.led-pixel { width:4px; height:4px; background:#001100; border-radius:1px; transition:background 0.1s; }
.led-pixel.active { box-shadow:0 0 2px currentColor; }
#titlePreview { display:block; font-size:.65rem; margin-top:8px; color:var(--muted); text-align:center; }
.blink .led-pixel.active { animation:ledBlink .9s steps(2,start) infinite; }
@keyframes ledBlink { to { background:#001100 !important; box-shadow:none !important; } }
.swatches { display:flex; flex-wrap:wrap; gap:6px; margin-top:6px; }
.swatch { width:30px; height:30px; border-radius:6px; cursor:pointer; position:relative; box-shadow:0 0 0 2px #141c22, 0 0 0 4px rgba(0,0,0,.3); transition:.2s transform,.2s box-shadow; }
.swatch:hover { transform:scale(1.1); box-shadow:0 0 0 2px #1f2d35,0 0 0 5px rgba(0,0,0,.55); }
.swatch.active { outline:2px solid #fff; }
.quick { display:flex; flex-wrap:wrap; gap:6px; margin-top:4px; }
.quick button { flex:1 0 120px; font-size:.65rem; padding:8px 10px; background:#25323a; }
.inline-note { font-size:.65rem; color:var(--muted); margin:6px 0 2px; }
.save-indicator { position:absolute; top:10px; right:14px; font-size:.65rem; letter-spacing:.1em; display:flex; align-items:center; gap:6px; color:var(--muted); }
.save-indicator.active { color:var(--accent); }
.dot { width:9px; height:9px; border-radius:50%; background:var(--muted); position:relative; overflow:hidden; }
.save-indicator.active .dot { background:var(--accent); box-shadow:0 0 0 4px rgba(0,150,136,.25); }
.toast { position:fixed; top:14px; left:50%; transform:translateX(-50%) translateY(-20px); background:#182125; color:var(--text); padding:10px 18px; border-radius:999px; font-size:.7rem; letter-spacing:.08em; opacity:0; pointer-events:none; transition:.35s; box-shadow:0 4px 22px -6px rgba(0,0,0,.6); }
.toast.show { opacity:1; transform:translateX(-50%) translateY(0); }
.row-2 { display:grid; gap:12px; grid-template-columns:repeat(auto-fit,minmax(120px,1fr)); }
footer { margin:40px 0 10px; text-align:center; font-size:.6rem; letter-spacing:.15em; color:var(--muted); }
@media (max-width:680px){ .row-2 { grid-template-columns:repeat(auto-fit,minmax(100px,1fr)); } }
</style>
</head><body>
<div class="auto-save-indicator" id="autoSaveIndicator">💾 Sauvegarde automatique...</div>
<div class="wrap grid" style="gap:18px;">
  <div class="card preview-panel" id="previewCard">
    <div class="save-indicator" id="saveState"><span class="dot"></span><span id="saveLabel">PRÊT</span></div>
    <div class="led-matrix" id="ledMatrix"></div>
    <span id="titlePreview"></span>
    <div class="inline-note" id="expireNote" style="display:none;">Le compte à rebours est terminé.</div>
    <div class="quick" aria-label="Ajustements rapides">
      <button type="button" data-add="60">+1 min</button>
      <button type="button" data-add="300">+5 min</button>
      <button type="button" data-add="3600">+1 h</button>
      <button type="button" data-add="86400">+1 jour</button>
      <button type="button" data-add="-300">-5 min</button>
      <button type="button" data-set="EOD">Fin de journée</button>
    </div>
  </div>
  
  <!-- Section Configuration Countdown -->
  <div class="card">
    <h2>⏰ Configuration du Countdown</h2>
    <form action="/settings" method="POST" id="settingsForm" autocomplete="off">
      <div class="grid" style="gap:16px;">
        <div class="field">
          <label for="title">📝 Titre du countdown</label>
          <input id="title" name="title" maxlength="50" required placeholder="BONNE ANNÉE 2026" />
          <small style="color:var(--muted); font-size:0.7rem; margin-top:4px;">Ce texte s'affiche quand le countdown se termine</small>
        </div>
        
        <div class="field-group">
          <label>📅 Date et heure cible</label>
          <div class="inline">
            <div class="field"><input type="date" id="date" name="date" required /></div>
            <div class="field"><input type="time" id="time" name="time" step="1" required /></div>
          </div>
        </div>
        
        <div class="field-group">
          <label>🎨 Style d'affichage</label>
          <div class="inline">
            <div class="field">
              <label for="fontStyle">Police</label>
              <select id="fontStyle" name="fontStyle">
                <option value="0">DejaVu Normal</option>
                <option value="1">DejaVu Gras</option>
                <option value="2">DejaVu Italique</option>
              </select>
            </div>
            <div class="field">
              <label for="colorWheel">Couleur</label>
              <div class="color-input-group">
                <div class="color-wheel-container">
                  <div class="color-wheel" id="colorWheel">
                    <div class="color-wheel-inner"></div>
                    <div class="color-wheel-cursor" id="colorCursor"></div>
                  </div>
                </div>
                <div class="color-preview" id="colorPreview"></div>
              </div>
            </div>
          </div>
          <div class="swatches" id="swatches"></div>
        </div>
      </div>
      
      <!-- Section Style du message de fin -->
      <div class="card">
        <h3>🎨 Style du message de fin</h3>
        <p>Personnalisez l'apparence du titre quand le countdown se termine</p>
        
        <div class="grid-2">
          <div class="field">
            <label for="endEffect">✨ Effet d'affichage</label>
            <select id="endEffect" name="endEffect">
              <option value="static">🔴 Statique</option>
              <option value="blink">💫 Clignotant</option>
              <option value="fade">🌊 Fade in/out</option>
              <option value="rainbow">🌈 Arc-en-ciel</option>
            </select>
          </div>
          <div class="field">
            <label for="endColorWheel">Couleur du message</label>
            <div class="color-input-group">
              <div class="color-wheel-container">
                <div class="color-wheel" id="endColorWheel">
                  <div class="color-wheel-inner"></div>
                  <div class="color-wheel-cursor" id="endColorCursor"></div>
                </div>
              </div>
              <div class="color-preview" id="endColorPreview"></div>
            </div>
          </div>
        </div>
        <div class="swatches" id="endSwatches"></div>
      </div>
      
      <!-- Champs cachés pour la soumission -->
      <input type="hidden" id="colorR" name="colorR" value="0" />
      <input type="hidden" id="colorG" name="colorG" value="255" />
      <input type="hidden" id="colorB" name="colorB" value="0" />
      <input type="hidden" id="endColorR" name="endColorR" value="255" />
      <input type="hidden" id="endColorG" name="endColorG" value="215" />
      <input type="hidden" id="endColorB" name="endColorB" value="0" />
    </form>
  </div>
  
  <!-- Boutons d'action principaux -->
  <div class="card">
    <div class="actions">
      <button type="button" id="btnSaveCountdown" class="primary">💾 Sauvegarder Configuration</button>
      <button type="button" id="btnSyncTime" class="secondary">🕐 Synchroniser Heure</button>
      <button type="button" id="btnReset" class="danger">🔄 Reset</button>
    </div>
  </div>
  
  <div class="card" id="formCard">
  <h1>Paramètres Avancés <span id="fwVer" style="font-size:.55rem;opacity:.6;vertical-align:middle;"></span></h1>
    <form action="/settings" method="POST" id="advancedForm" autocomplete="off">
        <button type="button" class="secondary" id="btnSyncTime" title="Synchroniser le RTC avec l'heure locale du navigateur">Sync Heure</button>
        <button type="button" class="danger" id="btnReset">Réinitialiser</button>
      </div>
      <h2>Clignotement</h2>
      <div class="row-2">
        <div class="field">
          <label for="blinkEnabled">Activer Clignotement</label>
          <select id="blinkEnabled" name="blinkEnabled">
            <option value="1" selected>Oui</option>
            <option value="0">Non</option>
          </select>
        </div>
        <div class="field">
          <label for="blinkInterval">Intervalle (ms)</label>
          <input type="number" id="blinkInterval" name="blinkInterval" min="50" max="5000" step="50" value="500" />
        </div>
        <div class="field" style="grid-column:1/-1;">
          <label for="blinkWindow">Dernières secondes (fenêtre)</label>
          <input type="number" id="blinkWindow" name="blinkWindow" min="1" max="3600" step="1" value="10" />
        </div>
      </div>
      <h2>Défilement</h2>
      <div class="row-2">
        <div class="field">
          <label for="marqueeEnabled">Activer Défilement</label>
          <select id="marqueeEnabled" name="marqueeEnabled">
            <option value="1" selected>Oui</option>
            <option value="0">Non</option>
          </select>
        </div>
        <div class="field">
          <label for="marqueeInterval">Vitesse (ms/pixel)</label>
          <input type="number" id="marqueeInterval" name="marqueeInterval" min="5" max="500" step="5" value="40" />
        </div>
        <div class="field">
          <label for="marqueeGap">Espace (px)</label>
          <input type="number" id="marqueeGap" name="marqueeGap" min="4" max="256" step="2" value="24" />
        </div>
        <div class="field" style="grid-column:1/-1;">
          <label for="marqueeMode">Mode</label>
          <select id="marqueeMode" name="marqueeMode">
            <option value="0" selected>Auto (si dépasse)</option>
            <option value="1">Toujours (gauche)</option>
            <option value="2">Aller-Retour</option>
            <option value="3">Une seule fois</option>
          </select>
          <div class="inline-note">Aller-Retour et Une seule fois nécessitent un texte plus large que l'afficheur (sauf Toujours).</div>
        </div>
        <div class="field">
          <label for="marqueeReturnInterval">Vitesse Retour (ms/pixel)</label>
          <input type="number" id="marqueeReturnInterval" name="marqueeReturnInterval" min="5" max="500" step="5" value="60" />
        </div>
        <div class="field">
          <label for="marqueeBouncePauseLeft">Pause Gauche (ms)</label>
          <input type="number" id="marqueeBouncePauseLeft" name="marqueeBouncePauseLeft" min="0" max="3000" step="50" value="400" />
        </div>
        <div class="field">
          <label for="marqueeBouncePauseRight">Pause Droite (ms)</label>
          <input type="number" id="marqueeBouncePauseRight" name="marqueeBouncePauseRight" min="0" max="3000" step="50" value="400" />
        </div>
        <div class="field">
          <label for="marqueeOneShotDelay">Délai Centré One-Shot (ms)</label>
          <input type="number" id="marqueeOneShotDelay" name="marqueeOneShotDelay" min="0" max="5000" step="100" value="800" />
        </div>
        <div class="field">
          <label for="marqueeOneShotStopCenter">One-Shot Fin Centrée</label>
          <select id="marqueeOneShotStopCenter" name="marqueeOneShotStopCenter">
            <option value="1" selected>Oui</option>
            <option value="0">Non</option>
          </select>
        </div>
        <div class="field">
          <label for="marqueeOneShotRestart">Restart One-Shot (s)</label>
          <input type="number" id="marqueeOneShotRestart" name="marqueeOneShotRestart" min="0" max="86400" step="1" value="0" />
        </div>
        <div class="field" style="grid-column:1/-1;">
          <label>Accélération Progressive</label>
          <div style="display:flex;gap:8px;flex-wrap:wrap;align-items:flex-end;">
            <select id="marqueeAccelEnabled" name="marqueeAccelEnabled">
              <option value="0" selected>Off</option>
              <option value="1">On</option>
            </select>
            <label style="font-size:12px;">Début(ms)<input type="number" id="marqueeAccelStart" name="marqueeAccelStart" min="5" max="500" step="5" value="80" style="width:80px;margin-left:4px;" /></label>
            <label style="font-size:12px;">Fin(ms)<input type="number" id="marqueeAccelEnd" name="marqueeAccelEnd" min="5" max="500" step="5" value="30" style="width:80px;margin-left:4px;" /></label>
            <label style="font-size:12px;">Durée(ms)<input type="number" id="marqueeAccelDuration" name="marqueeAccelDuration" min="100" max="20000" step="100" value="3000" style="width:90px;margin-left:4px;" /></label>
          </div>
          <div class="inline-note">Interpolation linéaire entre vitesse début et fin à chaque cycle (aller, retour ou boucle complete).</div>
        </div>
      </div>
      <h2>Luminosité</h2>
      <div class="row-2">
        <div class="field">
          <label for="brightnessMode">Mode</label>
          <select id="brightnessMode">
            <option value="auto" selected>Auto</option>
            <option value="manual">Manuel</option>
          </select>
        </div>
        <div class="field" style="grid-column:1/-1;">
          <label for="brightness">Valeur (0-255)</label>
          <input type="range" id="brightness" name="brightness" min="0" max="255" value="150" />
          <div style="font-size:.65rem; color:var(--muted);">Auto = -1 (adapté au nombre de panneaux)</div>
          <input type="hidden" id="brightnessHidden" name="brightness" value="-1" />
        </div>
      </div>
      <div class="inline-note">Les modifications sont appliquées immédiatement sur l'afficheur après envoi.</div>
    </form>
  </div>
  <footer>ESP32 P10 RGB Fullscreen Countdown • 2025 • __AUTHOR_NAME__ (<a href="__GITHUB_URL__" style="color:#4caf50" target="_blank" rel="noopener">GitHub</a>) • <a href="/about" style="color:#888">À propos</a></footer>
</div>
<div class="toast" id="toast"></div>
<script>
// === Utilitaires Couleur ===
const hex = n => n.toString(16).padStart(2,'0');
function hexToRgb(h){h=h.replace('#','');return {r:parseInt(h.substr(0,2),16),g:parseInt(h.substr(2,2),16),b:parseInt(h.substr(4,2),16)};}
function rgbToHex(r,g,b){return '#'+hex(r)+hex(g)+hex(b);} 

// === Éléments DOM ===
const el = id => document.getElementById(id);
const ledMatrix = el('ledMatrix');
const titlePreview = el('titlePreview');
const expireNote = el('expireNote');
const form = el('settingsForm');
const saveState = el('saveState');
const saveLabel = el('saveLabel');
const toast = el('toast');
const swatchesBox = el('swatches');
const autoSaveIndicator = el('autoSaveIndicator');

// === Gestion de la sauvegarde automatique ===
let autoSaveTimeout = null;
let isAutoSaving = false;

function showAutoSaveIndicator(message, type = 'info') {
  autoSaveIndicator.textContent = message;
  autoSaveIndicator.className = `auto-save-indicator show ${type}`;
  setTimeout(() => {
    autoSaveIndicator.classList.remove('show');
  }, 2000);
}

function triggerAutoSave() {
  if (autoSaveTimeout) clearTimeout(autoSaveTimeout);
  
  autoSaveTimeout = setTimeout(() => {
    if (!isAutoSaving) {
      performAutoSave();
    }
  }, 1500); // Attendre 1.5s après la dernière modification
}

function performAutoSave() {
  isAutoSaving = true;
  showAutoSaveIndicator('💾 Sauvegarde en cours...', 'info');
  
  const formData = new FormData(el('settingsForm'));
  
  fetch('/settings', {
    method: 'POST',
    body: formData
  })
  .then(r => r.text())
  .then(() => {
    showAutoSaveIndicator('✅ Sauvegardé automatiquement', 'success');
    isAutoSaving = false;
  })
  .catch(() => {
    showAutoSaveIndicator('❌ Erreur de sauvegarde', 'error');
    isAutoSaving = false;
  });
}

// === Classe pour gérer les roues de couleur ===
class ColorWheel {
  constructor(wheelId, cursorId, previewId, onColorChange) {
    this.wheel = el(wheelId);
    this.cursor = el(cursorId);
    this.preview = el(previewId);
    this.onColorChange = onColorChange;
    this.isDragging = false;
    this.currentColor = '#00ff00';
    
    this.setupEvents();
    this.setColor('#00ff00');
  }
  
  setupEvents() {
    this.wheel.addEventListener('mousedown', (e) => this.startDrag(e));
    this.wheel.addEventListener('touchstart', (e) => this.startDrag(e));
    document.addEventListener('mousemove', (e) => this.drag(e));
    document.addEventListener('touchmove', (e) => this.drag(e));
    document.addEventListener('mouseup', () => this.stopDrag());
    document.addEventListener('touchend', () => this.stopDrag());
  }
  
  startDrag(e) {
    this.isDragging = true;
    this.updateColor(e);
    e.preventDefault();
  }
  
  drag(e) {
    if (!this.isDragging) return;
    this.updateColor(e);
    e.preventDefault();
  }
  
  stopDrag() {
    this.isDragging = false;
  }
  
  updateColor(e) {
    const rect = this.wheel.getBoundingClientRect();
    const centerX = rect.width / 2;
    const centerY = rect.height / 2;
    
    let x, y;
    if (e.touches) {
      x = e.touches[0].clientX - rect.left - centerX;
      y = e.touches[0].clientY - rect.top - centerY;
    } else {
      x = e.clientX - rect.left - centerX;
      y = e.clientY - rect.top - centerY;
    }
    
    const angle = Math.atan2(y, x) * 180 / Math.PI;
    const distance = Math.min(Math.sqrt(x*x + y*y), centerX - 10);
    
    // Convertir l'angle en couleur HSV
    let hue = (angle + 360) % 360;
    const saturation = Math.min(distance / (centerX - 10), 1) * 100;
    const value = 100;
    
    // Convertir HSV en RGB
    const rgb = this.hsvToRgb(hue, saturation, value);
    const color = `rgb(${rgb.r}, ${rgb.g}, ${rgb.b})`;
    const hex = this.rgbToHex(rgb.r, rgb.g, rgb.b);
    
    this.currentColor = hex;
    this.setColor(hex);
    
    // Position du curseur
    const cursorX = centerX + Math.cos(angle * Math.PI / 180) * distance;
    const cursorY = centerY + Math.sin(angle * Math.PI / 180) * distance;
    this.cursor.style.left = cursorX + 'px';
    this.cursor.style.top = cursorY + 'px';
    
    if (this.onColorChange) {
      this.onColorChange(hex, rgb);
    }
  }
  
  setColor(hex) {
    this.currentColor = hex;
    this.preview.style.backgroundColor = hex;
    this.preview.style.setProperty('--preview-color', hex);
  }
  
  hsvToRgb(h, s, v) {
    h /= 360;
    s /= 100;
    v /= 100;
    
    const c = v * s;
    const x = c * (1 - Math.abs(((h * 6) % 2) - 1));
    const m = v - c;
    
    let r, g, b;
    if (h < 1/6) { r = c; g = x; b = 0; }
    else if (h < 2/6) { r = x; g = c; b = 0; }
    else if (h < 3/6) { r = 0; g = c; b = x; }
    else if (h < 4/6) { r = 0; g = x; b = c; }
    else if (h < 5/6) { r = x; g = 0; b = c; }
    else { r = c; g = 0; b = x; }
    
    return {
      r: Math.round((r + m) * 255),
      g: Math.round((g + m) * 255),
      b: Math.round((b + m) * 255)
    };
  }
  
  rgbToHex(r, g, b) {
    return `#${((1 << 24) + (r << 16) + (g << 8) + b).toString(16).slice(1)}`;
  }
}

// === Création de la matrice LED ===
const MATRIX_WIDTH = 96; // 3 panneaux de 32px
const MATRIX_HEIGHT = 16;
let ledPixels = [];

// Initialiser la matrice LED
function initLedMatrix() {
  ledMatrix.innerHTML = '';
  ledPixels = [];
  for(let i = 0; i < MATRIX_WIDTH * MATRIX_HEIGHT; i++) {
    const pixel = document.createElement('div');
    pixel.className = 'led-pixel';
    ledMatrix.appendChild(pixel);
    ledPixels.push(pixel);
  }
}

// Fonction pour allumer un pixel à une position donnée
function setPixel(x, y, color) {
  if(x >= 0 && x < MATRIX_WIDTH && y >= 0 && y < MATRIX_HEIGHT) {
    const index = y * MATRIX_WIDTH + x;
    if(ledPixels[index]) {
      ledPixels[index].style.backgroundColor = color;
      ledPixels[index].classList.add('active');
    }
  }
}

// Effacer tous les pixels
function clearMatrix() {
  ledPixels.forEach(pixel => {
    pixel.style.backgroundColor = '#001100';
    pixel.classList.remove('active');
  });
}

// Dessiner du texte simplifié sur la matrice (simulation basique)
function drawTextOnMatrix(text, color) {
  clearMatrix();
  
  // Police bitmap 5x7 simplifiée pour les chiffres et :
  const font = {
    '0': [[1,1,1],[1,0,1],[1,0,1],[1,0,1],[1,1,1]],
    '1': [[0,1,0],[1,1,0],[0,1,0],[0,1,0],[1,1,1]],
    '2': [[1,1,1],[0,0,1],[1,1,1],[1,0,0],[1,1,1]],
    '3': [[1,1,1],[0,0,1],[1,1,1],[0,0,1],[1,1,1]],
    '4': [[1,0,1],[1,0,1],[1,1,1],[0,0,1],[0,0,1]],
    '5': [[1,1,1],[1,0,0],[1,1,1],[0,0,1],[1,1,1]],
    '6': [[1,1,1],[1,0,0],[1,1,1],[1,0,1],[1,1,1]],
    '7': [[1,1,1],[0,0,1],[0,0,1],[0,0,1],[0,0,1]],
    '8': [[1,1,1],[1,0,1],[1,1,1],[1,0,1],[1,1,1]],
    '9': [[1,1,1],[1,0,1],[1,1,1],[0,0,1],[1,1,1]],
    ':': [[0],[1],[0],[1],[0]],
    ' ': [[0,0,0],[0,0,0],[0,0,0],[0,0,0],[0,0,0]],
    '-': [[0,0,0],[0,0,0],[1,1,1],[0,0,0],[0,0,0]]
  };
  
  let x = 8; // Position de départ
  for(let i = 0; i < text.length; i++) {
    const char = text[i];
    const pattern = font[char];
    if(pattern) {
      for(let row = 0; row < pattern.length; row++) {
        for(let col = 0; col < pattern[row].length; col++) {
          if(pattern[row][col]) {
            setPixel(x + col, 5 + row, color);
          }
        }
      }
      x += (pattern[0] ? pattern[0].length : 3) + 1; // Espacement entre caractères
    }
  }
}

// === Configuration des swatches ===
const SWATCHES = ['#00ff00','#ffffff','#ff0000','#00bcd4','#ffeb3b','#ff9800','#ff00ff','#2196f3','#9c27b0','#4caf50'];

// Fonction pour créer les swatches
function createSwatches(containerId, colorWheel) {
  const container = el(containerId);
  
  container.innerHTML = '';
  SWATCHES.forEach(color => {
    const swatch = document.createElement('div');
    swatch.className = 'swatch';
    swatch.style.background = color;
    swatch.dataset.color = color;
    swatch.title = color;
    swatch.addEventListener('click', () => {
      colorWheel.setColor(color);
      if (colorWheel.onColorChange) {
        const rgb = hexToRgb(color);
        colorWheel.onColorChange(color, rgb);
      }
      showToast('Couleur appliquée');
    });
    container.appendChild(swatch);
  });
}

// === Variables de logique ===
let target = null; // Date target JS
let expired=false; let blink=false; let lastBlink=0;
let lastServerSync = 0; let offsetMs = 0; // Diff client/serveur (si future sync ajoutée)

function showToast(msg,ok=true){toast.textContent=msg;toast.classList.add('show');toast.style.background= ok?'#1e2c31':'#452222'; setTimeout(()=>toast.classList.remove('show'),2600);} 

function setSaving(active,label){if(active){saveState.classList.add('active');saveLabel.textContent=label||'SAUVEGARDE';}else{saveState.classList.remove('active');saveLabel.textContent=label||'PRÊT';}}

function updateColor(val,fromSwatch=false){
  const {r,g,b}=hexToRgb(val); 
  el('colorR').value=r; 
  el('colorG').value=g; 
  el('colorB').value=b; 
  document.querySelectorAll('.swatch').forEach(s=>s.classList.toggle('active',s.dataset.color===val)); 
  if(fromSwatch){showToast('Couleur appliquée');} 
  updateLedPreview();
  if (mainColorWheel) {
    mainColorWheel.setColor(val);
  }
}

function parseFormDateTime(){ const d=el('date').value; const t=el('time').value || '00:00:00'; if(!d) return null; const parts = d.split('-').map(Number); const time=t.split(':').map(Number); return new Date(parts[0],parts[1]-1,parts[2],time[0]||0,time[1]||0,time[2]||0); }

function refreshTarget(){ target = parseFormDateTime(); }

// Fonction pour mettre à jour la prévisualisation LED
function updateLedPreview() {
  if(!target) return;
  
  const now = new Date(Date.now()+offsetMs);
  let diff = (target - now)/1000;
  let displayText = '';
  let color = mainColorWheel ? mainColorWheel.currentColor : '#00ff00';
  
  if(diff <= 0) {
    displayText = el('title').value || 'FINI';
    expired = true;
    expireNote.style.display='block';
    
    // Utiliser la couleur du message de fin
    if(endColorWheel) {
      color = endColorWheel.currentColor;
    }
  } else {
    expired = false;
    expireNote.style.display='none';
    let d=Math.floor(diff/86400);
    let h=Math.floor(diff%86400/3600);
    let m=Math.floor(diff%3600/60);
    let s=Math.floor(diff%60);
    
    // Format adaptatif comme dans l'original
    if(d>0) displayText=`${d}d ${h.toString().padStart(2,'0')}:${m.toString().padStart(2,'0')}`;
    else if(h>0) displayText=`${h.toString().padStart(2,'0')}:${m.toString().padStart(2,'0')}:${s.toString().padStart(2,'0')}`;
    else if(m>0) displayText=`${m.toString().padStart(2,'0')}:${s.toString().padStart(2,'0')}`;
    else displayText=s.toString().padStart(2,'0');
    
    // Gestion du clignotement pour les dernières secondes
    const bw = parseInt(el('blinkWindow').value)||10;
    if(d===0 && h===0 && m===0 && s<=bw) {
      if(Date.now()-lastBlink>450) {
        lastBlink=Date.now();
        blink=!blink;
        ledMatrix.classList.toggle('blink',blink);
      }
    } else {
      ledMatrix.classList.remove('blink');
    }
  }
  
  drawTextOnMatrix(displayText, color);
}

function updatePreviewLoop(ts){ 
  updateLedPreview();
  requestAnimationFrame(updatePreviewLoop); 
}
requestAnimationFrame(updatePreviewLoop);

function syncFieldsToPreview(){ 
  titlePreview.textContent = (el('title').value||'').toUpperCase(); 
  refreshTarget(); 
  updateLedPreview();
}

// Gestionnaires d'événements pour les champs principaux
['title','date','time','fontStyle'].forEach(id => {
  el(id).addEventListener('input', () => {
    syncFieldsToPreview();
    triggerAutoSave();
  });
});

// Initialisation des roues de couleur
let mainColorWheel, endColorWheel;

function initializeColorWheels() {
  // Roue de couleur principale (pour le countdown)
  mainColorWheel = new ColorWheel('colorWheel', 'colorCursor', 'colorPreview', (hex, rgb) => {
    el('colorR').value = rgb.r;
    el('colorG').value = rgb.g;
    el('colorB').value = rgb.b;
    updateLedPreview();
    triggerAutoSave();
  });
  
  // Roue de couleur pour le message de fin
  endColorWheel = new ColorWheel('endColorWheel', 'endColorCursor', 'endColorPreview', (hex, rgb) => {
    el('endColorR').value = rgb.r;
    el('endColorG').value = rgb.g;
    el('endColorB').value = rgb.b;
    updateLedPreview();
    triggerAutoSave();
  });
  
  // Couleur par défaut pour le message de fin (doré)
  endColorWheel.setColor('#FFD700');
}

// Bouton de sauvegarde manuelle (optionnel maintenant)
el('btnSaveCountdown').addEventListener('click', () => {
  if (isAutoSaving) return;
  performAutoSave();
});

// Bouton reset
el('btnReset').addEventListener('click', () => {
  if(!confirm('Réinitialiser tous les paramètres ?')) return;
  fetch('/reset')
    .then(() => {
      showToast('Reset effectué');
      setTimeout(() => location.reload(), 1000);
    })
    .catch(() => showToast('Erreur reset', false));
});

// Synchronisation de l'heure RTC
el('btnSyncTime').addEventListener('click', () => {
  const now = new Date();
  const epoch = now.getTime();
  const tz = now.getTimezoneOffset();
  showToast('Envoi heure locale...');
  fetch(`/syncTime?epoch=${epoch}&tz=${tz}`)
    .then(r => r.json())
    .then(j => {
      if(j.status === 'OK') {
        showAutoSaveIndicator('✅ RTC synchronisé', 'success');
      } else {
        showAutoSaveIndicator('❌ Erreur sync', 'error');
      }
    })
    .catch(() => showAutoSaveIndicator('❌ Erreur réseau sync', 'error'));
});

// Ajustements rapides
document.querySelectorAll('.quick button[data-add], .quick button[data-set]').forEach(btn=>btn.addEventListener('click',()=>{
  if(!target) refreshTarget(); 
  if(btn.dataset.set==='EOD'){ 
    const n=new Date(); 
    n.setHours(23,59,59,0); 
    target=n; 
  } else { 
    const add=parseInt(btn.dataset.add,10); 
    target = new Date((target?target:new Date()).getTime()+add*1000); 
  }
  el('date').value = target.toISOString().split('T')[0]; 
  const t = target.toTimeString().split(' ')[0]; 
  el('time').value = t; 
  syncFieldsToPreview(); 
  showToast('Nouvelle cible: ' + t); 
  triggerAutoSave(); 
}));

// Chargement initial depuis l'ESP32
window.addEventListener('load',()=>{
  // Initialiser la matrice LED
  initLedMatrix();
  
  // Initialiser les roues de couleur
  initializeColorWheels();
  
  // Créer les swatches
  createSwatches('swatches', mainColorWheel);
  
  // Injecter version firmware (fourni côté C++ via endpoint futur ou placeholder compilé)
  document.getElementById('fwVer').textContent = 'v__FWVER__';
  const now=new Date(); const tomorrow=new Date(now.getTime()+86400000); el('date').value=tomorrow.toISOString().split('T')[0]; el('time').value='23:59:59';
  fetch('/getSettings').then(r=>r.json()).then(d=>{ el('title').value=d.title; const ds=`${d.year}-${String(d.month).padStart(2,'0')}-${String(d.day).padStart(2,'0')}`; el('date').value=ds; const ts=`${String(d.hour).padStart(2,'0')}:${String(d.minute).padStart(2,'0')}:${String(d.second).padStart(2,'0')}`; el('time').value=ts; if(d.blinkEnabled!==undefined){ el('blinkEnabled').value = d.blinkEnabled ? '1' : '0'; } if(d.blinkIntervalMs!==undefined){ el('blinkInterval').value = d.blinkIntervalMs; } if(d.blinkWindow!==undefined){ el('blinkWindow').value = d.blinkWindow; } if(d.marqueeEnabled!==undefined){ el('marqueeEnabled').value = d.marqueeEnabled ? '1':'0'; } if(d.marqueeIntervalMs!==undefined){ el('marqueeInterval').value = d.marqueeIntervalMs; } if(d.marqueeGap!==undefined){ el('marqueeGap').value = d.marqueeGap; } if(d.marqueeMode!==undefined){ el('marqueeMode').value = d.marqueeMode; } if(d.marqueeReturnIntervalMs!==undefined){ el('marqueeReturnInterval').value = d.marqueeReturnIntervalMs; } if(d.marqueeBouncePauseLeftMs!==undefined){ el('marqueeBouncePauseLeft').value = d.marqueeBouncePauseLeftMs; } if(d.marqueeBouncePauseRightMs!==undefined){ el('marqueeBouncePauseRight').value = d.marqueeBouncePauseRightMs; } if(d.marqueeOneShotDelayMs!==undefined){ el('marqueeOneShotDelay').value = d.marqueeOneShotDelayMs; } if(d.marqueeOneShotStopCenter!==undefined){ el('marqueeOneShotStopCenter').value = d.marqueeOneShotStopCenter? '1':'0'; } if(d.marqueeOneShotRestartSec!==undefined){ el('marqueeOneShotRestart').value = d.marqueeOneShotRestartSec; } if(d.marqueeAccelEnabled!==undefined){ el('marqueeAccelEnabled').value = d.marqueeAccelEnabled? '1':'0'; } if(d.marqueeAccelStartIntervalMs!==undefined){ el('marqueeAccelStart').value = d.marqueeAccelStartIntervalMs; } if(d.marqueeAccelEndIntervalMs!==undefined){ el('marqueeAccelEnd').value = d.marqueeAccelEndIntervalMs; } if(d.marqueeAccelDurationMs!==undefined){ el('marqueeAccelDuration').value = d.marqueeAccelDurationMs; } if(d.brightness!==undefined){ if(parseInt(d.brightness,10)===-1){ el('brightnessMode').value='auto'; el('brightnessHidden').value='-1'; } else { el('brightnessMode').value='manual'; el('brightness').value=d.brightness; el('brightnessHidden').value=d.brightness; } } if(d.fontStyle!==undefined){ el('fontStyle').value = d.fontStyle; } const color=rgbToHex(d.colorR,d.colorG,d.colorB); el('colorPicker').value=color; updateColor(color); syncFieldsToPreview(); setSaving(false,'PRÊT'); showToast('Paramètres chargés'); }).catch(()=>{ showToast('Échec chargement paramètres',false); setSaving(false,'HORS LIGNE'); syncFieldsToPreview(); });
});
</script>
</body></html>
)rawliteral";
  // Ancien script legacy supprimé (désormais encapsulé dans MAIN_page)
// Note: La fonction display_update_enable() a été intégrée dans setup()
// pour une meilleure gestion avec FreeRTOS

// Prototypes des fonctions de calcul de countdown
void updateCountdown(int &days, int &hours, int &minutes, int &seconds);
void updateDisplayFormat(int days, int hours, int minutes, int seconds);

// === Fonctions helper pour opérations critiques avec mutex ===
inline bool safeTakeCountdownData(int &days, int &hours, int &minutes, int &seconds, bool &expired) {
  MUTEX_GUARD(countdownMutex, MUTEX_TIMEOUT_FAST);
  if (guard_countdownMutex.isLocked()) {
    updateCountdown(days, hours, minutes, seconds);
    updateDisplayFormat(days, hours, minutes, seconds);
    expired = countdownExpired;
    return true;
  }
  return false;
}

inline bool safeUpdateCountdownTarget(const DateTime &newTarget) {
  MUTEX_GUARD(countdownMutex, MUTEX_TIMEOUT_NORMAL);
  if (guard_countdownMutex.isLocked()) {
    countdownTarget = newTarget;
    countdownExpired = false; // Reset si nouvelle date
    return true;
  }
  return false;
}

// Calcul du temps restant
void updateCountdown(int &days, int &hours, int &minutes, int &seconds) {
  DateTime now = rtc.now();
  
  // Vérifier si le countdown est expiré
  if (now >= countdownTarget) {
    countdownExpired = true;
    days = hours = minutes = 0;
    seconds = 0;
    blinkLastSeconds = false; // Pas de clignotement une fois expiré
    return;
  }
  
  countdownExpired = false;
  
  // Calculer la différence
  TimeSpan diff = countdownTarget - now;
  long totalSeconds = diff.totalseconds();
  
  days = totalSeconds / 86400;
  hours = (totalSeconds % 86400) / 3600;
  minutes = (totalSeconds % 3600) / 60;
  seconds = totalSeconds % 60;
  
  // Activer le clignotement uniquement sur la fenêtre configurée des dernières secondes
  int bw = blinkWindowSeconds;
  if (bw < 1) bw = 1;
  blinkLastSeconds = (days == 0 && hours == 0 && minutes == 0 && seconds <= bw);
}

// Détermine le format d'affichage en fonction du temps restant
void updateDisplayFormat(int days, int hours, int minutes, int seconds) {
  if (days > 0) {
    displayFormat = 0;  // Format jours
  } else if (hours > 0) {
    displayFormat = 1;  // Format heures
  } else if (minutes > 0) {
    displayFormat = 2;  // Format minutes
  } else {
    displayFormat = 3;  // Format secondes uniquement
  }
}

// Fonction pour obtenir la largeur du texte
uint16_t getTextWidth(const char* text, const GFXfont* font = NULL) {
  int16_t x1, y1;
  uint16_t w, h;
  
  if (font) display.setFont(font);
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  display.setFont(); // Reset to default font
  
  return w;
}

// Dessine les deux points du séparateur d'horloge
void drawColon(int16_t x, int16_t y, uint16_t colonColor) {
  display.fillRect(x, y, 2, 2, colonColor);
  display.fillRect(x, y+4, 2, 2, colonColor);
}

// Sélection de police selon le style choisi (Normal, Gras, Italique)
const GFXfont* getOptimalFont() {
  switch (fontStyle) {
    case 1: return &DejaVuSans_Bold9pt8b;    // Gras
    case 2: return &DejaVuSans_Oblique9pt8b; // Italique
    default: return &DejaVuSans9ptLat1;      // Normal (défaut)
  }
}

// Calcul automatique de la taille optimale selon le texte et la police
int calculateAutoTextSize(const char* text, const GFXfont* font) {
  // Taille de base selon la largeur totale disponible - ajustée pour minuteur
  int baseSize;
  if (TOTAL_WIDTH >= 128) {
    baseSize = 2; // Taille réduite pour 4+ panneaux (128+ pixels)
  } else if (TOTAL_WIDTH >= 96) {
    baseSize = 1; // Taille réduite pour 3 panneaux (96 pixels) - mieux pour minuteur
  } else if (TOTAL_WIDTH >= 64) {
    baseSize = 1; // Taille appropriée pour 2 panneaux (64 pixels)
  } else {
    baseSize = 1; // Taille standard pour 1 panneau (32 pixels)
  }
  
  // Adapter selon la longueur du texte (textes longs = taille plus petite)
  int textLen = strlen(text);
  if (textLen > 8 && baseSize > 1) {
    return baseSize - 1; // Réduire la taille pour les textes longs (seuil abaissé)
  }
  
  return baseSize;
}

// Affichage du compte à rebours en plein écran
void displayFullscreenCountdown(int days, int hours, int minutes, int seconds) {
  // --- Cache layout ---
  static char lastText[64] = ""; // élargi pour titres jusqu'à 50 chars
  static bool lastExpired = false;
  static int16_t cachedX = 0, cachedY = 0;
  static uint8_t cachedSetSize = 1;
  static bool cachedIsEndMsg = false;
  static uint16_t cachedTextPixelWidth = 0; // pour calcul marquee
  static int16_t cachedFontXOffset = 0;     // x1 pour centrage correct

  // Recalculer systématiquement la couleur utilisateur (évite usage d'une valeur obsolète)
  int localR, localG, localB;
  uint16_t userColor;
  
  if (countdownExpired) {
    // Utiliser les couleurs du message de fin
    localR = endMessageColorR;
    localG = endMessageColorG;
    localB = endMessageColorB;
    if (localR < 0) localR = 0; if (localR > 255) localR = 255;
    if (localG < 0) localG = 0; if (localG > 255) localG = 255;
    if (localB < 0) localB = 0; if (localB > 255) localB = 255;
    userColor = display.color565(localR, localG, localB);
  } else {
    // Utiliser les couleurs du countdown
    localR = countdownColorR;
    localG = countdownColorG;
    localB = countdownColorB;
    if (localR < 0) localR = 0; if (localR > 255) localR = 255;
    if (localG < 0) localG = 0; if (localG > 255) localG = 255;
    if (localB < 0) localB = 0; if (localB > 255) localB = 255;
    userColor = display.color565(localR, localG, localB);
  }

  // Gestion des effets d'affichage
  uint16_t displayColor = userColor;
  
  if (countdownExpired) {
    // Effets pour le message de fin
    unsigned long currentTime = millis();
    static unsigned long lastEffectTime = 0;
    static bool effectState = false;
    static uint8_t rainbowHue = 0;
    
    switch (endMessageEffect) {
      case 1: // Clignotant
        if (currentTime - lastEffectTime >= 500) {
          lastEffectTime = currentTime;
          effectState = !effectState;
        }
        displayColor = effectState ? userColor : myBLACK;
        break;
        
      case 2: // Fade in/out
        {
          int fadePhase = (currentTime / 50) % 100; // Cycle de 5 secondes
          if (fadePhase > 50) fadePhase = 100 - fadePhase;
          float fadeFactor = fadePhase / 50.0f;
          int fadeR = (int)(localR * fadeFactor);
          int fadeG = (int)(localG * fadeFactor);
          int fadeB = (int)(localB * fadeFactor);
          displayColor = display.color565(fadeR, fadeG, fadeB);
        }
        break;
        
      case 3: // Arc-en-ciel
        if (currentTime - lastEffectTime >= 100) {
          lastEffectTime = currentTime;
          rainbowHue = (rainbowHue + 5) % 360;
        }
        {
          // Conversion HSV vers RGB simple
          float h = rainbowHue / 60.0f;
          float s = 1.0f, v = 1.0f;
          int i = (int)h;
          float f = h - i;
          float p = v * (1 - s);
          float q = v * (1 - s * f);
          float t = v * (1 - s * (1 - f));
          float r, g, b;
          switch (i) {
            case 0: r = v; g = t; b = p; break;
            case 1: r = q; g = v; b = p; break;
            case 2: r = p; g = v; b = t; break;
            case 3: r = p; g = q; b = v; break;
            case 4: r = t; g = p; b = v; break;
            default: r = v; g = p; b = q; break;
          }
          displayColor = display.color565((int)(r * 255), (int)(g * 255), (int)(b * 255));
        }
        break;
        
      default: // Statique
        displayColor = userColor;
        break;
    }
  } else if (blinkEnabled && blinkLastSeconds) {
    // Clignotement configurable des 10 dernières secondes (si activé)
    unsigned long currentTime = millis();
    int localInterval = blinkIntervalMs;
    if (localInterval < 50) localInterval = 50;       // bornes de sécurité
    if (localInterval > 5000) localInterval = 5000;
    if (currentTime - lastBlinkTime >= (unsigned long)localInterval) {
      lastBlinkTime = currentTime;
      blinkState = !blinkState;
    }
    displayColor = blinkState ? userColor : myBLACK;
  }

  // Préparer le texte cible
  char currentText[64];
  if (countdownExpired) {
    // Police DejaVu avec support complet UTF-8 -> Latin-1
    utf8ToLatin1(countdownTitle, currentText, sizeof(currentText));
  } else {
    switch (displayFormat) {
      case 0:  snprintf(currentText, sizeof(currentText), "%dD %02d:%02d", days, hours, minutes); break;
      case 1:  snprintf(currentText, sizeof(currentText), "%02d:%02d:%02d", hours, minutes, seconds); break;
      case 2:  snprintf(currentText, sizeof(currentText), "%02d:%02d", minutes, seconds); break;
      default: snprintf(currentText, sizeof(currentText), "%02d", seconds); break;
    }
  }

  bool needRecalc = forceLayout || countdownExpired != lastExpired || (strcmp(currentText, lastText) != 0);

  if (needRecalc) {
    // Utiliser uniquement la police DejaVu optimale
    const GFXfont *font = getOptimalFont();
    display.setFont(font);
    
    // Calcul automatique de la taille basé sur la résolution du panneau
    int optimalSize = calculateAutoTextSize(currentText, font);
    display.setTextSize(optimalSize);
    cachedSetSize = optimalSize;
    
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(currentText, 0, 0, &x1, &y1, &w, &h);
  cachedTextPixelWidth = w; // conserver largeur
  cachedY = (TOTAL_HEIGHT - h) / 2 - y1;
  cachedFontXOffset = x1;

    // Décider activation selon le mode
    marqueeTextWidth = w;
    marqueeActive = false;
    marqueeOneShotDone = (marqueeMode == 3) ? marqueeOneShotDone : false; // réinitialiser si changement de texte
    if (marqueeEnabled) {
      switch (marqueeMode) {
        case 0: // Auto (continuous gauche si dépasse)
          if (w > TOTAL_WIDTH) marqueeActive = true;
          break;
        case 1: // Toujours gauche
          marqueeActive = true;
          break;
        case 2: // Aller-Retour seulement si dépasse
          if (w > TOTAL_WIDTH) marqueeActive = true;
          break;
        case 3: // Une seule fois (si dépasse et pas déjà fini)
          if (w > TOTAL_WIDTH && !marqueeOneShotDone) marqueeActive = true;
          break;
      }
    }

    if (marqueeActive) {
      marqueeInPause = false;
      marqueePauseUntil = 0;
      if (marqueeMode == 2) { // bounce
    // Démarre avec padding gauche
    marqueeOffset = marqueeEdgePadding;
        marqueeDirection = -1;
        // pause initiale gauche
        if (marqueeBouncePauseLeftMs > 0) { marqueeInPause = true; marqueePauseUntil = millis() + marqueeBouncePauseLeftMs; }
      } else if (marqueeMode == 1 || marqueeMode == 0) {
    marqueeOffset = TOTAL_WIDTH + marqueeEdgePadding; // continuous depuis la droite + padding
      } else if (marqueeMode == 3) { // one-shot centré d'abord
        marqueeOneShotCenterPhase = true;
        marqueeOneShotStart = millis();
        cachedX = (TOTAL_WIDTH - w) / 2 - x1; // centré
      }
      lastMarqueeStep = millis();
      marqueeCycleStartMs = millis();
    } else {
      cachedX = (TOTAL_WIDTH - w) / 2 - x1; // centré
    }

    // Si texte court (pas de marquee) on définit cachedX, sinon il sera dynamique
    if (!marqueeActive) {
      cachedX = (TOTAL_WIDTH - w) / 2 - x1;
    }
  // Conserver version pliée (marquee & mesure)
  strncpy(lastText, currentText, sizeof(lastText)-1);
  lastText[sizeof(lastText)-1] = '\0';
    lastExpired = countdownExpired;
    cachedIsEndMsg = countdownExpired;
  forceLayout = false;
  } else {
    const GFXfont *font = getOptimalFont();
    display.setFont(font);
    display.setTextSize(cachedSetSize);
  }

  // Gestion de l'avancement du marquee (hors section critique)
  if (marqueeActive) {
    unsigned long nowMs = millis();
    // ONE SHOT: phase centrée -> attendre délai puis lancer scroll
    if (marqueeMode == 3 && marqueeOneShotCenterPhase) {
      if (nowMs - marqueeOneShotStart >= (unsigned long)marqueeOneShotDelayMs) {
        marqueeOneShotCenterPhase = false;
        marqueeOffset = TOTAL_WIDTH; // début scroll
        lastMarqueeStep = nowMs;
        marqueeCycleStartMs = nowMs;
      } else {
        // ne rien faire pendant la phase centrée
      }
    } else if (marqueeMode == 3 && marqueeOneShotCenterPhase == false && marqueeOneShotDone) {
      // terminé : si restart demandé
      if (marqueeOneShotRestartSec > 0 && nowMs >= marqueeOneShotRestartAt && marqueeOneShotRestartAt != 0) {
        // relance cycle
        marqueeOneShotDone = false;
        marqueeOneShotCenterPhase = true;
        marqueeOneShotStart = nowMs;
        lastMarqueeStep = nowMs;
        marqueeCycleStartMs = nowMs;
      }
    } else {
      // BOUNCE: gestion pause
      if (marqueeMode == 2 && marqueeInPause) {
        if (nowMs >= marqueePauseUntil) {
          marqueeInPause = false;
          lastMarqueeStep = nowMs; // reset timer pour éviter saut
          marqueeCycleStartMs = nowMs; // nouveau cycle après pause
        }
      }
      int forwardInt = marqueeIntervalMs; if (forwardInt < 5) forwardInt = 5; if (forwardInt > 500) forwardInt = 500;
      int returnInt = marqueeReturnIntervalMs; if (returnInt < 5) returnInt = 5; if (returnInt > 500) returnInt = 500;

      // Accélération progressive
      if (marqueeAccelEnabled) {
        int startI = marqueeAccelStartIntervalMs; if (startI < 5) startI = 5; if (startI > 500) startI = 500;
        int endI = marqueeAccelEndIntervalMs; if (endI < 5) endI = 5; if (endI > 500) endI = 500;
        unsigned long elapsed = nowMs - marqueeCycleStartMs;
        float t = (marqueeAccelDurationMs <= 0) ? 1.0f : (float)elapsed / (float)marqueeAccelDurationMs;
        if (t > 1.0f) t = 1.0f;
        int interp = startI + (int)((endI - startI) * t);
        // Pour bounce: appliquer sur direction actuelle (séparément pour retour si différent)
        if (marqueeMode == 2) {
          if (marqueeDirection == -1) forwardInt = interp; else returnInt = interp; // direction -1 = vers gauche (forward logique), 1 = retour
        } else if (marqueeMode == 0 || marqueeMode == 1 || marqueeMode == 3) {
          forwardInt = interp;
        }
      }
      int effectiveInt = forwardInt;
      if (marqueeMode == 2 && marqueeDirection == 1) effectiveInt = returnInt; // retour
      if (!marqueeInPause && nowMs - lastMarqueeStep >= (unsigned long)effectiveInt) {
        lastMarqueeStep = nowMs;
        if (marqueeMode == 2) { // bounce
          marqueeOffset += marqueeDirection; // -1 gauche, +1 droite
          int minX = TOTAL_WIDTH - marqueeTextWidth - marqueeEdgePadding; // borne gauche avec padding
          if (marqueeOffset <= minX) { marqueeOffset = minX; marqueeDirection = 1; if (marqueeBouncePauseRightMs>0){ marqueeInPause=true; marqueePauseUntil=nowMs+marqueeBouncePauseRightMs; } marqueeCycleStartMs = nowMs; }
          if (marqueeOffset >= marqueeEdgePadding) { marqueeOffset = marqueeEdgePadding; marqueeDirection = -1; if (marqueeBouncePauseLeftMs>0){ marqueeInPause=true; marqueePauseUntil=nowMs+marqueeBouncePauseLeftMs; } marqueeCycleStartMs = nowMs; }
        } else if (marqueeMode == 3) { // one-shot scrolling phase
          if (!marqueeOneShotDone && !marqueeOneShotCenterPhase) {
            marqueeOffset--; // vers la gauche
            if (marqueeOffset + marqueeTextWidth < 0) {
              marqueeActive = false; marqueeOneShotDone = true;
              if (marqueeOneShotStopCenter) {
                // recadrer avec correction x1
                cachedX = (TOTAL_WIDTH - marqueeTextWidth) / 2 - cachedFontXOffset;
              }
              if (marqueeOneShotRestartSec > 0) {
                marqueeOneShotRestartAt = nowMs + (unsigned long)marqueeOneShotRestartSec * 1000UL;
              }
            }
          }
        } else { // continuous modes
          marqueeOffset--;
          int localGap = marqueeGap; if (localGap < 4) localGap = 4; if (localGap > 256) localGap = 256;
          if (marqueeOffset + marqueeTextWidth < 0) {
            marqueeOffset = TOTAL_WIDTH + localGap + marqueeEdgePadding; // boucle avec padding
            marqueeCycleStartMs = nowMs; // nouveau cycle -> reset accel
          }
        }
      }
    }
  }

  // Section critique minimale : effacement + écriture tampon
  portENTER_CRITICAL(&timerMux);
  display.clearDisplay();
  // Toujours la couleur choisie (même si expiré) conformément à la demande
  display.setTextColor(displayColor);
  if (marqueeActive) {
    if (marqueeMode == 2) { // bounce
      display.setCursor(marqueeOffset, cachedY);
      display.print(lastText);
    } else if (marqueeMode == 3) { // one-shot
      if (marqueeOneShotCenterPhase) {
        display.setCursor((TOTAL_WIDTH - marqueeTextWidth)/2 - cachedFontXOffset, cachedY);
        display.print(lastText);
      } else if (marqueeOneShotDone && marqueeOneShotStopCenter) {
        display.setCursor((TOTAL_WIDTH - marqueeTextWidth)/2 - cachedFontXOffset, cachedY);
        display.print(lastText);
      } else {
        display.setCursor(marqueeOffset, cachedY);
        display.print(lastText);
      }
    } else {
      // continuous / always
      int drawX = marqueeOffset;
      display.setCursor(drawX, cachedY);
      display.print(lastText);
      int localGap = marqueeGap; if (localGap < 4) localGap = 4; if (localGap > 256) localGap = 256;
      int secondX = marqueeOffset + marqueeTextWidth + localGap;
      if (secondX < TOTAL_WIDTH) {
        display.setCursor(secondX, cachedY);
        display.print(lastText);
      }
    }
  } else {
    display.setCursor(cachedX, cachedY);
    display.print(lastText);
  }
  portEXIT_CRITICAL(&timerMux);
}

// Chargement des paramètres depuis la mémoire flash (version thread-safe)
void loadSettings() {
  Serial.println("Loading settings from NVS...");
  
  // Utilisation de la classe helper pour mutex automatique avec timeout approprié
  MUTEX_GUARD_CHECK(preferencesMutex, MUTEX_TIMEOUT_SLOW) {
    Serial.println("Failed to acquire preferences mutex - using defaults");
    return;
  }
  
  // Prendre aussi le mutex countdown pour cohérence des paramètres
  MUTEX_GUARD_CHECK(countdownMutex, MUTEX_TIMEOUT_NORMAL) {
    Serial.println("Failed to acquire countdown mutex - using defaults");
    return;
  }
  
  bool success = false;
  do {
    if (!preferences.begin("countdown", true)) {
      Serial.println("Failed to begin preferences for reading");
      break;
    }
    
    // Paramètres du countdown
    countdownYear = preferences.getInt("cd_Year", 2025);
    countdownMonth = preferences.getInt("cd_Month", 12);
    countdownDay = preferences.getInt("cd_Day", 31);
    countdownHour = preferences.getInt("cd_Hour", 23);
    countdownMinute = preferences.getInt("cd_Minute", 59);
    countdownSecond = preferences.getInt("cd_Second", 0);
    
    // Paramètres d'affichage - style de police et couleur
    fontStyle = preferences.getInt("fontStyle", 0); // 0=Normal, 1=Gras, 2=Italique
    countdownColorR = preferences.getInt("colorR", 0);
    countdownColorG = preferences.getInt("colorG", 255);
    countdownColorB = preferences.getInt("colorB", 0);
  blinkEnabled = preferences.getBool("blinkEn", true);
  blinkIntervalMs = preferences.getInt("blinkInt", 500);
  blinkWindowSeconds = preferences.getInt("blinkWin", 10);
  marqueeEnabled = preferences.getBool("mqEn", true);
  marqueeIntervalMs = preferences.getInt("mqInt", 40);
  marqueeGap = preferences.getInt("mqGap", 24);
  marqueeMode = preferences.getInt("mqMode", 0);
  marqueeReturnIntervalMs = preferences.getInt("mqRetInt", 60);
  marqueeBouncePauseLeftMs = preferences.getInt("mqPL", 400);
  marqueeBouncePauseRightMs = preferences.getInt("mqPR", 400);
  marqueeOneShotDelayMs = preferences.getInt("mqOsDelay", 800);
  marqueeOneShotStopCenter = preferences.getBool("mqOsStopC", true);
  marqueeOneShotRestartSec = preferences.getInt("mqOsRst", 0);
  marqueeAccelEnabled = preferences.getBool("mqAccEn", false);
  marqueeAccelStartIntervalMs = preferences.getInt("mqAccSt", 80);
  marqueeAccelEndIntervalMs = preferences.getInt("mqAccEnd", 30);
  marqueeAccelDurationMs = preferences.getInt("mqAccDur", 3000);
  displayBrightness = preferences.getInt("bright", 250);
    
    String savedTitle = preferences.getString("cd_Title", "COUNTDOWN");
    strcpy(countdownTitle, savedTitle.c_str());
    
    preferences.end();
    success = true;
    
  } while(false);
  
  // Les mutex sont libérés automatiquement par le destructeur de MutexGuard
  
  if (success) {
    Serial.println("Settings loaded successfully");
  } else {
    Serial.println("Using default settings");
  }
  
  // Mise à jour de la couleur
  countdownColor = display.color565(countdownColorR, countdownColorG, countdownColorB);
  
  // Mise à jour de la date cible
  countdownTarget = DateTime(countdownYear, countdownMonth, countdownDay, 
                           countdownHour, countdownMinute, countdownSecond);
  // Appliquer brightness (auto si -1)
  int effectiveBrightness;
  if (displayBrightness < 0) {
    effectiveBrightness = 150;
    if (MATRIX_PANELS_X > 2) effectiveBrightness = 100;
    if (MATRIX_PANELS_X > 4) effectiveBrightness = 80;
    if (MATRIX_PANELS_X > 6) effectiveBrightness = 60;
  } else {
    effectiveBrightness = displayBrightness;
  }
  display.setBrightness(effectiveBrightness);
}

// Sauvegarde des paramètres dans la mémoire flash (version thread-safe optimisée)
void saveSettings() {
  Serial.println("Saving settings to NVS...");
  
  // Vérifications de sécurité préliminaires
  if (!checkMutexSanity()) {
    Serial.println("Mutex sanity check failed - aborting save");
    return;
  }
  
  // Désactiver temporairement le timer d'affichage pour éviter les conflits
  bool timerWasEnabled = (timer != nullptr);
  if (timerWasEnabled) {
    display_update_enable(false);
    vTaskDelay(pdMS_TO_TICKS(10)); // Attendre que l'IRQ se termine
  }
  
  // Tentative d'acquisition des mutex avec timeout très court pour éviter les blocages
  if (preferencesMutex && xSemaphoreTake(preferencesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    
    bool prefsStarted = false;
    
    // Retry logic pour preferences.begin()
    for (int retry = 0; retry < 3 && !prefsStarted; retry++) {
      prefsStarted = preferences.begin("countdown", false);
      if (!prefsStarted) {
        vTaskDelay(pdMS_TO_TICKS(10));
      }
    }
    
    if (prefsStarted) {
      // Paramètres du countdown - capture des valeurs locales pour éviter les races
      int year, month, day, hour, minute, second;
      int colorR, colorG, colorB;
      int fStyle;
      
      // Acquisition TRÈS rapide du mutex countdown pour capturer les valeurs
      if (countdownMutex && xSemaphoreTake(countdownMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        year = countdownYear;
        month = countdownMonth;
        day = countdownDay;
        hour = countdownHour;
        minute = countdownMinute;
        second = countdownSecond;
        colorR = countdownColorR;
        colorG = countdownColorG;
        colorB = countdownColorB;
        fStyle = fontStyle;
        xSemaphoreGive(countdownMutex);
      } else {
        // Fallback sans mutex - utiliser les valeurs directement (atomic reads sur ESP32)
        Serial.println("Mutex timeout - using direct read (atomic)");
        year = countdownYear;
        month = countdownMonth;
        day = countdownDay;
        hour = countdownHour;
        minute = countdownMinute;
        second = countdownSecond;
        colorR = countdownColorR;
        colorG = countdownColorG;
        colorB = countdownColorB;
        fStyle = fontStyle;
      }
      
      // Sauvegarde optimisée - grouper les écritures pour réduire les opérations NVS
      try {
        preferences.putInt("cd_Year", year);
        preferences.putInt("cd_Month", month);
        preferences.putInt("cd_Day", day);
        preferences.putInt("cd_Hour", hour);
        preferences.putInt("cd_Minute", minute);
        preferences.putInt("cd_Second", second);
        
        // Paramètres d'affichage - style de police et couleur
        preferences.putInt("fontStyle", fStyle);
        preferences.putInt("colorR", colorR);
        preferences.putInt("colorG", colorG);
        preferences.putInt("colorB", colorB);
        
        // Paramètres du message de fin
        preferences.putInt("endColorR", endMessageColorR);
        preferences.putInt("endColorG", endMessageColorG);
        preferences.putInt("endColorB", endMessageColorB);
        preferences.putInt("endEffect", endMessageEffect);
        preferences.putBool("blinkEn", blinkEnabled);
        preferences.putInt("blinkInt", blinkIntervalMs);
        preferences.putInt("blinkWin", blinkWindowSeconds);
        preferences.putBool("mqEn", marqueeEnabled);
        preferences.putInt("mqInt", marqueeIntervalMs);
        preferences.putInt("mqGap", marqueeGap);
        preferences.putInt("mqMode", marqueeMode);
        preferences.putInt("mqRetInt", marqueeReturnIntervalMs);
        preferences.putInt("mqPL", marqueeBouncePauseLeftMs);
        preferences.putInt("mqPR", marqueeBouncePauseRightMs);
        preferences.putInt("mqOsDelay", marqueeOneShotDelayMs);
        preferences.putBool("mqOsStopC", marqueeOneShotStopCenter);
        preferences.putInt("mqOsRst", marqueeOneShotRestartSec);
        preferences.putBool("mqAccEn", marqueeAccelEnabled);
        preferences.putInt("mqAccSt", marqueeAccelStartIntervalMs);
        preferences.putInt("mqAccEnd", marqueeAccelEndIntervalMs);
        preferences.putInt("mqAccDur", marqueeAccelDurationMs);
        preferences.putInt("bright", displayBrightness);
        
        preferences.putString("cd_Title", String(countdownTitle));
        
        Serial.println("Settings saved successfully");
        
        // Mise à jour de la couleur et de la date cible en dehors du contexte NVS
        countdownColor = display.color565(colorR, colorG, colorB);
        countdownTarget = DateTime(year, month, day, hour, minute, second);
        
      } catch (...) {
        Serial.println("Exception during NVS write - data may be corrupted");
      }
      
      preferences.end();
    } else {
      Serial.println("Failed to begin preferences after retries");
    }
    
    xSemaphoreGive(preferencesMutex);
  } else {
    Serial.println("Could not acquire preferences mutex - save skipped");
  }
  
  // Réactiver le timer d'affichage si nécessaire
  if (timerWasEnabled) {
    vTaskDelay(pdMS_TO_TICKS(5)); // Petit délai avant réactivation
    display_update_enable(true);
  }
}

// Connexion WiFi
void connecting_To_WiFi() {
  Serial.println("\n-------------WIFI mode (STA async)");
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false); // éviter écritures flash lentes
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);
  uint32_t startAttempt = millis();
  const uint32_t timeoutMs = 8000; // timeout réduit
  while (WiFi.status() != WL_CONNECTED && (millis() - startAttempt) < timeoutMs) {
    WIFI_STEP_DELAY(50);
    Serial.print('.');
    // Donner du temps au scheduler (éviter WDT)
    yield();
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi connected in %lu ms\n", (unsigned long)(millis() - startAttempt));
    Serial.print("IP: "); Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connect timeout -> fallback AP");
    useStationMode = false;
  }
  Serial.println("-------------");
}

// Configuration du point d'accès
void set_ESP32_Access_Point() {
  Serial.println("\n-------------");
  Serial.println("WIFI mode : AP");
  WiFi.mode(WIFI_AP);
  Serial.println("-------------");

  Serial.println("Configuring Access Point...");
  WiFi.softAP(ap_ssid, ap_password);
  IPAddress local_ip(192, 168, 1, 1);
  IPAddress gateway(192, 168, 1, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(local_ip, gateway, subnet);
  Serial.print("SSID : "); Serial.println(ap_ssid);
  Serial.print("AP IP : "); Serial.println(WiFi.softAPIP());
}

// Gestionnaire de la page principale
void handleRoot() {
  String page = MAIN_page;
  page.replace("v__FWVER__", String(FIRMWARE_VERSION));
  page.replace("__AUTHOR_NAME__", String(AUTHOR_NAME));
  page.replace("__GITHUB_URL__", String(GITHUB_URL));
  server.send(200, "text/html", page);
}

// Gestionnaire des paramètres actuels en JSON
void handleGetSettings() {
  String json = "{";
  json += "\"title\":\"" + String(countdownTitle) + "\",";
  json += "\"year\":" + String(countdownYear) + ",";
  json += "\"month\":" + String(countdownMonth) + ",";
  json += "\"day\":" + String(countdownDay) + ",";
  json += "\"hour\":" + String(countdownHour) + ",";
  json += "\"minute\":" + String(countdownMinute) + ",";
  json += "\"second\":" + String(countdownSecond) + ",";
  json += "\"fontStyle\":" + String(fontStyle) + ",";
  json += "\"colorR\":" + String(countdownColorR) + ",";
  json += "\"colorG\":" + String(countdownColorG) + ",";
  json += "\"colorB\":" + String(countdownColorB) + ",";
  json += "\"blinkEnabled\":" + String(blinkEnabled ? 1 : 0) + ",";
  json += "\"blinkIntervalMs\":" + String(blinkIntervalMs) + ",";
  json += "\"blinkWindow\":" + String(blinkWindowSeconds) + ",";
  json += "\"marqueeEnabled\":" + String(marqueeEnabled ? 1 : 0) + ",";
  json += "\"marqueeIntervalMs\":" + String(marqueeIntervalMs) + ",";
  json += "\"marqueeGap\":" + String(marqueeGap) + ",";
  json += "\"marqueeMode\":" + String(marqueeMode) + ",";
  json += "\"marqueeReturnIntervalMs\":" + String(marqueeReturnIntervalMs) + ",";
  json += "\"marqueeBouncePauseLeftMs\":" + String(marqueeBouncePauseLeftMs) + ",";
  json += "\"marqueeBouncePauseRightMs\":" + String(marqueeBouncePauseRightMs) + ",";
  json += "\"marqueeOneShotDelayMs\":" + String(marqueeOneShotDelayMs) + ",";
  json += "\"marqueeOneShotStopCenter\":" + String(marqueeOneShotStopCenter ? 1:0) + ",";
  json += "\"marqueeOneShotRestartSec\":" + String(marqueeOneShotRestartSec) + ",";
  json += "\"marqueeAccelEnabled\":" + String(marqueeAccelEnabled ? 1:0) + ",";
  json += "\"marqueeAccelStartIntervalMs\":" + String(marqueeAccelStartIntervalMs) + ",";
  json += "\"marqueeAccelEndIntervalMs\":" + String(marqueeAccelEndIntervalMs) + ",";
  json += "\"marqueeAccelDurationMs\":" + String(marqueeAccelDurationMs);
  json += ",\"brightness\":" + String(displayBrightness);
  json += "}";
  
  server.send(200, "application/json", json);
}

// Synchronisation de l'heure depuis le navigateur (client envoie son epoch ms + offset minutes)
void handleSyncTime() {
  if (!server.hasArg("epoch")) {
    server.send(400, "application/json", "{\"status\":\"ERR\",\"msg\":\"missing epoch\"}");
    return;
  }
  String epochStr = server.arg("epoch");
  String tzStr = server.hasArg("tz") ? server.arg("tz") : String("0");
  unsigned long long epochMs = strtoull(epochStr.c_str(), nullptr, 10);
  long tzMinutes = tzStr.toInt(); // JS getTimezoneOffset(): UTC = local + offset
  // epochMs est en UTC. Nous voulons régler le RTC sur l'heure locale perçue par l'utilisateur.
  time_t epochSec = (time_t)(epochMs / 1000ULL);
  // local = UTC - offsetMinutes*60 (car offset = UTC - local)
  time_t localSec = epochSec - (tzMinutes * 60L);
  DateTime localDT(localSec);
  // Protection simple : vérifier année raisonnable
  if (localDT.year() < 2020 || localDT.year() > 2099) {
    server.send(400, "application/json", "{\"status\":\"ERR\",\"msg\":\"invalid time\"}");
    return;
  }
  // Ajuster le RTC (mutex countdown pas nécessaire pour simple set, mais on peut briefer)
  rtc.adjust(localDT);
  // Recalculer le countdownTarget à partir des paramètres sauvegardés (utc inchangé) — laisser tel quel.
  // Réponse
  String resp = "{\"status\":\"OK\",\"set\":\"";
  resp += String(localDT.year()) + "-" + String(localDT.month()) + "-" + String(localDT.day()) + "T" + String(localDT.hour()) + ":" + String(localDT.minute()) + ":" + String(localDT.second()) + "\"}";
  server.send(200, "application/json", resp);
  Serial.printf("RTC synchronized to client local time: %04d-%02d-%02d %02d:%02d:%02d (tz offset %ld mn)\n", localDT.year(), localDT.month(), localDT.day(), localDT.hour(), localDT.minute(), localDT.second(), tzMinutes);
}

// Gestionnaire des paramètres
void handleSettings() {
  Serial.println("\n-------------Settings");

  // Désactiver temporairement le timer d'affichage pour éviter conflits ISR pendant mise à jour
  bool displayTimerWasOn = (timer != nullptr);
  if (displayTimerWasOn) {
    Serial.println("[handleSettings] Pause display timer");
    display_update_enable(false);
  }

  // Récupérer les valeurs de la requête
  String title = server.hasArg("title") ? server.arg("title") : String("");
  String dateStr = server.hasArg("date") ? server.arg("date") : String("");
  String timeStr = server.hasArg("time") ? server.arg("time") : String("");

  if (dateStr.length() < 10 || timeStr.length() < 5) {
    server.send(400, "text/plain", "Parametres invalides");
    return;
  }
  
  // Parser la date (format YYYY-MM-DD)
  // Protéger modifications
  if (countdownMutex != NULL && xSemaphoreTake(countdownMutex, pdMS_TO_TICKS(200))) {
    countdownYear = dateStr.substring(0, 4).toInt();
    countdownMonth = dateStr.substring(5, 7).toInt();
    countdownDay = dateStr.substring(8, 10).toInt();
  
  // Parser l'heure (format HH:MM:SS)
  countdownHour = timeStr.substring(0, 2).toInt();
  countdownMinute = timeStr.substring(3, 5).toInt();
  countdownSecond = timeStr.length() > 5 ? timeStr.substring(6, 8).toInt() : 0;
  
  // Récupérer le style de police et la couleur
  fontStyle = server.hasArg("fontStyle") ? server.arg("fontStyle").toInt() : fontStyle;
  countdownColorR = server.hasArg("colorR") ? server.arg("colorR").toInt() : countdownColorR;
  countdownColorG = server.hasArg("colorG") ? server.arg("colorG").toInt() : countdownColorG;
  countdownColorB = server.hasArg("colorB") ? server.arg("colorB").toInt() : countdownColorB;
  
  // Récupérer les paramètres du message de fin
  if (server.hasArg("endColorR")) {
    endMessageColorR = server.arg("endColorR").toInt();
  }
  if (server.hasArg("endColorG")) {
    endMessageColorG = server.arg("endColorG").toInt();
  }
  if (server.hasArg("endColorB")) {
    endMessageColorB = server.arg("endColorB").toInt();
  }
  if (server.hasArg("endEffect")) {
    String effect = server.arg("endEffect");
    if (effect == "static") endMessageEffect = 0;
    else if (effect == "blink") endMessageEffect = 1;
    else if (effect == "fade") endMessageEffect = 2;
    else if (effect == "rainbow") endMessageEffect = 3;
  }
  if (server.hasArg("blinkEnabled")) {
    blinkEnabled = server.arg("blinkEnabled").toInt() != 0;
  }
  if (server.hasArg("blinkInterval")) {
    int bi = server.arg("blinkInterval").toInt();
    if (bi < 50) bi = 50; if (bi > 5000) bi = 5000; // bornes logiques
    blinkIntervalMs = bi;
  }
  if (server.hasArg("blinkWindow")) {
    int bw = server.arg("blinkWindow").toInt();
    if (bw < 1) bw = 1; if (bw > 3600) bw = 3600;
    blinkWindowSeconds = bw;
  }
  if (server.hasArg("marqueeEnabled")) {
    marqueeEnabled = server.arg("marqueeEnabled").toInt() != 0;
  forceLayout = true;
  }
  if (server.hasArg("marqueeInterval")) {
    int mi = server.arg("marqueeInterval").toInt();
    if (mi < 5) mi = 5; if (mi > 500) mi = 500;
    marqueeIntervalMs = mi;
  forceLayout = true;
  }
  if (server.hasArg("marqueeGap")) {
    int mg = server.arg("marqueeGap").toInt();
    if (mg < 4) mg = 4; if (mg > 256) mg = 256;
    marqueeGap = mg;
  forceLayout = true;
  }
  if (server.hasArg("marqueeMode")) {
    int mm = server.arg("marqueeMode").toInt();
    if (mm < 0) mm = 0; if (mm > 3) mm = 3;
    marqueeMode = mm;
    // réinitialiser états spécifiques
    marqueeOneShotDone = false;
  forceLayout = true;
  }
  if (server.hasArg("marqueeReturnInterval")) {
    int ri = server.arg("marqueeReturnInterval").toInt();
    if (ri < 5) ri = 5; if (ri > 500) ri = 500;
    marqueeReturnIntervalMs = ri;
  forceLayout = true;
  }
  if (server.hasArg("marqueeBouncePauseLeft")) {
    int bpL = server.arg("marqueeBouncePauseLeft").toInt();
    if (bpL < 0) bpL = 0; if (bpL > 5000) bpL = 5000;
    marqueeBouncePauseLeftMs = bpL;
  forceLayout = true;
  }
  if (server.hasArg("marqueeBouncePauseRight")) {
    int bpR = server.arg("marqueeBouncePauseRight").toInt();
    if (bpR < 0) bpR = 0; if (bpR > 5000) bpR = 5000;
    marqueeBouncePauseRightMs = bpR;
  forceLayout = true;
  }
  if (server.hasArg("marqueeOneShotDelay")) {
    int od = server.arg("marqueeOneShotDelay").toInt();
    if (od < 0) od = 0; if (od > 10000) od = 10000;
    marqueeOneShotDelayMs = od;
  forceLayout = true;
  }
  if (server.hasArg("marqueeOneShotStopCenter")) {
    marqueeOneShotStopCenter = server.arg("marqueeOneShotStopCenter") == "1";
  forceLayout = true;
  }
  if (server.hasArg("marqueeOneShotRestart")) {
    int rs = server.arg("marqueeOneShotRestart").toInt();
    if (rs < 0) rs = 0; if (rs > 86400) rs = 86400;
    marqueeOneShotRestartSec = rs;
  forceLayout = true;
  }
  if (server.hasArg("marqueeAccelEnabled")) {
    marqueeAccelEnabled = server.arg("marqueeAccelEnabled") == "1";
  forceLayout = true;
  }
  if (server.hasArg("marqueeAccelStart")) {
    int as = server.arg("marqueeAccelStart").toInt();
    if (as < 5) as = 5; if (as > 500) as = 500; marqueeAccelStartIntervalMs = as;
  forceLayout = true;
  }
  if (server.hasArg("marqueeAccelEnd")) {
    int ae = server.arg("marqueeAccelEnd").toInt();
    if (ae < 5) ae = 5; if (ae > 500) ae = 500; marqueeAccelEndIntervalMs = ae;
  forceLayout = true;
  }
  if (server.hasArg("marqueeAccelDuration")) {
    int ad = server.arg("marqueeAccelDuration").toInt();
    if (ad < 50) ad = 50; if (ad > 600000) ad = 600000; marqueeAccelDurationMs = ad;
  forceLayout = true;
  }
  if (server.hasArg("brightness")) {
    int b = server.arg("brightness").toInt();
    if (b < -1) b = -1; if (b > 255) b = 255;
    displayBrightness = b;
  }
  
  // Valider le style de police (0=Normal, 1=Gras, 2=Italique)
  if (fontStyle < 0) fontStyle = 0;
  if (fontStyle > 2) fontStyle = 2;
  
  // Mettre à jour le titre
    if (title.length() > 0) {
      utf8SafeCopyTruncate(title, countdownTitle, sizeof(countdownTitle), 50);
    }
  
  // Valider la date
  if (countdownMonth < 1) countdownMonth = 1;
  if (countdownMonth > 12) countdownMonth = 12;
  if (countdownDay < 1) countdownDay = 1;
  if (countdownDay > 31) countdownDay = 31;
  
  // Valider l'heure
    if (countdownHour > 23) countdownHour = 23;
    if (countdownMinute > 59) countdownMinute = 59;
    if (countdownSecond > 59) countdownSecond = 59;

    // Recalculer couleur immédiate (affichage plus réactif)
    countdownColor = display.color565(countdownColorR, countdownColorG, countdownColorB);
    // Mettre à jour cible localement pour affichage avant sauvegarde
    countdownTarget = DateTime(countdownYear, countdownMonth, countdownDay, 
                               countdownHour, countdownMinute, countdownSecond);
    xSemaphoreGive(countdownMutex);
  } else {
    Serial.println("Could not get countdown mutex in handleSettings");
  }
  
  // Sauvegarder les paramètres (demander la sauvegarde plutôt que de la faire directement)
  saveRequested = true;
  saveRequestTime = millis();

  // Ajuster luminosité immédiatement
  int effectiveBrightness;
  if (displayBrightness < 0) {
    effectiveBrightness = 150;
    if (MATRIX_PANELS_X > 2) effectiveBrightness = 100;
    if (MATRIX_PANELS_X > 4) effectiveBrightness = 80;
    if (MATRIX_PANELS_X > 6) effectiveBrightness = 60;
  } else {
    effectiveBrightness = displayBrightness;
  }
  display.setBrightness(effectiveBrightness);
  
  Serial.println("Settings updated:");
  Serial.printf("Target: %d-%02d-%02d %02d:%02d:%02d\n", 
                countdownYear, countdownMonth, countdownDay,
                countdownHour, countdownMinute, countdownSecond);
  Serial.printf("Title: %s\n", countdownTitle);
  const char* fontStyleNames[] = {"Normal", "Gras", "Italique"};
  Serial.printf("Font: DejaVu %s (taille automatique)\n", fontStyleNames[fontStyle]);
  Serial.printf("Color (RGB): %d,%d,%d\n", countdownColorR, countdownColorG, countdownColorB);
  Serial.printf("Blink: enabled=%d interval=%dms window=%ds\n", blinkEnabled, blinkIntervalMs, blinkWindowSeconds);
  Serial.printf("Marquee: en=%d mode=%d fwdInt=%d retInt=%d gap=%d LPause=%d RPause=%d accel=%d start=%d end=%d dur=%d oneDelay=%d oneStopC=%d oneRst=%d\n",
    marqueeEnabled, marqueeMode, marqueeIntervalMs, marqueeReturnIntervalMs, marqueeGap,
    marqueeBouncePauseLeftMs, marqueeBouncePauseRightMs, marqueeAccelEnabled?1:0,
    marqueeAccelStartIntervalMs, marqueeAccelEndIntervalMs, marqueeAccelDurationMs,
    marqueeOneShotDelayMs, marqueeOneShotStopCenter?1:0, marqueeOneShotRestartSec);
  Serial.printf("Brightness setting: %d ( -1 = auto )\n", displayBrightness);
  
  // Répondre avec une redirection vers la page principale
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");

  // Réactiver timer si nécessaire
  if (displayTimerWasOn) {
    Serial.println("[handleSettings] Resume display timer");
    display_update_enable(true);
    // Forcer un redraw complet
    forceLayout = true;
  }
}

// Gestionnaire de reset
void handleReset() {
  // Vérification clé supprimée

  // Réinitialiser aux valeurs par défaut
  if (countdownMutex != NULL && xSemaphoreTake(countdownMutex, pdMS_TO_TICKS(200))) {
    countdownYear = 2025;
    countdownMonth = 12;
    countdownDay = 31;
    countdownHour = 23;
    countdownMinute = 59;
    countdownSecond = 0;
    strcpy(countdownTitle, "COUNTDOWN");
    fontStyle = 0; // Normal par défaut
    countdownColorR = 0;
    countdownColorG = 255;
    countdownColorB = 0;
    countdownColor = display.color565(countdownColorR, countdownColorG, countdownColorB);
    countdownTarget = DateTime(countdownYear, countdownMonth, countdownDay, 
                               countdownHour, countdownMinute, countdownSecond);
    xSemaphoreGive(countdownMutex);
  }
  
  // Sauvegarder les paramètres (demander la sauvegarde plutôt que de la faire directement)
  saveRequested = true;
  saveRequestTime = millis();
  
  Serial.println("Settings reset to defaults");
  
  // Répondre avec une redirection vers la page principale
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

// Configuration et démarrage du serveur
void prepare_and_start_The_Server() {
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  server.on("/", handleRoot);
  server.on("/settings", HTTP_POST, handleSettings);
  server.on("/getSettings", handleGetSettings);
  server.on("/syncTime", HTTP_GET, handleSyncTime);
  server.on("/reset", HTTP_POST, handleReset);
  server.onNotFound([]() { server.sendHeader("Location", "/", true); server.send(302, "text/plain", ""); });
  server.begin();
  Serial.println("HTTP server started (fast)");
  if (useStationMode) {
    Serial.print("URL: http://"); Serial.println(WiFi.localIP());
  } else {
    Serial.print("AP URL: http://"); Serial.println(WiFi.softAPIP());
  }
}

// Fonction de callback pour le timer d'affichage
void IRAM_ATTR display_updater() {
  // Vérification rapide de la validité avant d'entrer en section critique
  if (timer == nullptr) {
    return; // Timer désactivé
  }
  
  portENTER_CRITICAL_ISR(&timerMux);
  // Double vérification dans la section critique
  if (timer != nullptr) {
    display.display(display_draw_time);
  }
  portEXIT_CRITICAL_ISR(&timerMux);
}

// Activation/désactivation du timer d'affichage
void display_update_enable(bool is_enable) {
  if (is_enable) {
    if (timer == nullptr) {
      timer = timerBegin(0, 80, true);
      if (timer != nullptr) {
        timerAttachInterrupt(timer, &display_updater, true);
        timerAlarmWrite(timer, 4000, true);
        timerAlarmEnable(timer);
        Serial.println("Display timer enabled");
      } else {
        Serial.println("Failed to initialize display timer");
      }
    } else {
      Serial.println("Display timer already enabled");
    }
  } else {
    if (timer != nullptr) {
      timerAlarmDisable(timer);
      timerDetachInterrupt(timer);
      timerEnd(timer);
      timer = nullptr;
      Serial.println("Display timer disabled");
    }
  }
}

void setup() {
  // Éviter grosse pause initiale; laisser hardware se stabiliser rapidement
  BOOT_DELAY(100);
  Serial.begin(115200);
  Serial.println("\n=== ESP32 P10 RGB FULLSCREEN COUNTDOWN ===");
  Serial.printf("Configuration: %dx%d panels (%dx%d total resolution)\n", 
                MATRIX_PANELS_X, MATRIX_PANELS_Y, TOTAL_WIDTH, TOTAL_HEIGHT);
  
  // Création des mutex
  displayMutex = xSemaphoreCreateMutex();
  countdownMutex = xSemaphoreCreateMutex();
  preferencesMutex = xSemaphoreCreateMutex();
  
  if (displayMutex == NULL || countdownMutex == NULL || preferencesMutex == NULL) {
    Serial.println("Error creating mutexes");
    while(1);
  }
  
  // NE PAS activer le timer ici - attendre après le WiFi et les tâches
  
  // Initialisation du RTC
  Serial.println("Initializing RTC...");
  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC");
    while (1) delay(10);
  }
  Serial.println("RTC initialized successfully");
  
  // Initialisation de l'affichage avec configuration P10 optimisée
  display.begin(4); // 1/8 scan pour P10
  display.setScanPattern(ZAGZIG);
  display.setMuxPattern(BINARY); 
  const int muxdelay = 10; // Délai de multiplexage
  display.setMuxDelay(muxdelay, muxdelay, muxdelay, muxdelay, muxdelay);
  BOOT_DELAY(20);
  
  // Luminosité adaptée au nombre de panneaux
  int brightness = 150;
  if (MATRIX_PANELS_X > 2) brightness = 100;
  if (MATRIX_PANELS_X > 4) brightness = 80;
  if (MATRIX_PANELS_X > 6) brightness = 60;
  
  display.setBrightness(brightness);
  Serial.printf("Brightness set to: %d\n", brightness);
  
  display.setTextWrap(false);
  display.setRotation(0);
  
  // Chargement des paramètres
  loadSettings();
  // Affichage initial (y compris si déjà expiré au démarrage)
  {
    int d=0,h=0,m=0,s=0;
    if (countdownMutex && xSemaphoreTake(countdownMutex, pdMS_TO_TICKS(50))) {
      updateCountdown(d,h,m,s);
      updateDisplayFormat(d,h,m,s);
      xSemaphoreGive(countdownMutex);
    }
    if (displayMutex && xSemaphoreTake(displayMutex, pdMS_TO_TICKS(50))) {
      displayFullscreenCountdown(d,h,m,s);
      xSemaphoreGive(displayMutex);
    }
  }
  
  // Splash écran réduit
  display.clearDisplay();
  display.setTextColor(countdownColor);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("BOOT");
  display.setCursor(0, 8);
  display.print("COUNTDOWN");
  BOOT_DELAY(BOOT_SPLASH_MS);
  display.clearDisplay();
  
  // Configuration WiFi initiale
  if (useStationMode) {
    connecting_To_WiFi();
    if (!useStationMode) {
      set_ESP32_Access_Point();
    }
  } else {
    set_ESP32_Access_Point();
  }

  // Démarrage du serveur web
  prepare_and_start_The_Server();
  
  Serial.println("Creating FreeRTOS tasks (fast boot)...");
  
  // Création des tâches FreeRTOS avec une meilleure distribution
  BaseType_t result;
  
  // Tâche d'affichage sur le Core 1 (isolé du WiFi)
  result = xTaskCreatePinnedToCore(
    DisplayTask,
    "DisplayTask",
    TASK_DISPLAY_STACK,
    NULL,
    TASK_DISPLAY_PRIORITY,
    &displayTaskHandle,
    1  // Core 1
  );
  if (result != pdPASS) Serial.println("Failed to create Display task");
  
  // Tâche combinée réseau + serveur web Core 0
  result = xTaskCreatePinnedToCore(
    NetWebTask,
    "NetWebTask",
    TASK_NETWORK_STACK,
    NULL,
    TASK_NETWORK_PRIORITY,
    &networkTaskHandle,
    0
  );
  if (result != pdPASS) Serial.println("Failed to create NetWeb task");
  
  // Tâche de calcul sur le Core 0
  result = xTaskCreatePinnedToCore(
    CountdownTask,
    "CountdownTask",
    TASK_COUNTDOWN_STACK,
    NULL,
    TASK_COUNTDOWN_PRIORITY,
    &countdownTaskHandle,
    0  // Core 0
  );
  if (result != pdPASS) Serial.println("Failed to create Countdown task");
  
  Serial.println("FreeRTOS tasks created successfully");
  Serial.println("Starting fullscreen countdown...");
  
  // ACTIVATION DU TIMER D'AFFICHAGE EN DERNIÈRE ÉTAPE
  Serial.println("Activating display timer earlier...");
  display_update_enable(true);
  BOOT_DELAY(50); // Stabilisation rapide
}

// Tâche d'affichage
void DisplayTask(void * parameter) {
  // Attendre que le système soit prêt
  vTaskDelay(pdMS_TO_TICKS(1000));
  
  Serial.println("Display task started on core " + String(xPortGetCoreID()));
  
  int prevSeconds = -1, prevMinutes = -1, prevHours = -1, prevDays = -1;
  bool prevExpired = false;
  for(;;) {
    // Utiliser MUTEX_GUARD avec timeout optimisé pour l'affichage
    {
      MUTEX_GUARD(displayMutex, MUTEX_TIMEOUT_FAST);
      if (guard_displayMutex.isLocked()) {
        int days=0, hours=0, minutes=0, seconds=0;
        bool expired = false;
        
        // Utiliser la fonction helper optimisée
        if (safeTakeCountdownData(days, hours, minutes, seconds, expired)) {
          bool secondChanged = (seconds != prevSeconds) || (minutes != prevMinutes) || (hours != prevHours) || (days != prevDays);
          bool needDraw = secondChanged || blinkLastSeconds || expired != prevExpired || marqueeActive;
          if (needDraw) {
            displayFullscreenCountdown(days, hours, minutes, seconds);
            prevSeconds = seconds; prevMinutes = minutes; prevHours = hours; prevDays = days; prevExpired = expired;
          }
        }
      }
    }
    
    // Fréquence un peu plus élevée si marquee actif pour fluidité, sinon économe
    uint32_t baseDelay = marqueeActive ? 40 : 150;
    if (blinkLastSeconds && baseDelay > 50) baseDelay = 50;
    vTaskDelay(pdMS_TO_TICKS(baseDelay));
  }
}

// Tâche combinée réseau + web
void NetWebTask(void * parameter) {
  vTaskDelay(pdMS_TO_TICKS(1200));
  Serial.println("NetWeb task started on core " + String(xPortGetCoreID()));
  uint32_t lastReconnectCheck = 0;
  for(;;) {
    dnsServer.processNextRequest();
    server.handleClient();
    if (useStationMode) {
      uint32_t now = millis();
      if (WiFi.status() != WL_CONNECTED && now - lastReconnectCheck > 5000) {
        lastReconnectCheck = now;
        Serial.println("[NetWeb] WiFi lost -> reconnect");
        WiFi.reconnect();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// Tâche de gestion du compte à rebours
void CountdownTask(void * parameter) {
  // Attendre que le RTC soit prêt
  vTaskDelay(pdMS_TO_TICKS(1500));
  
  Serial.println("Countdown task started on core " + String(xPortGetCoreID()));
  
  for(;;) {
    // Vérifier s'il faut sauvegarder les paramètres (avec délai de sécurité)
    if (saveRequested && (millis() - saveRequestTime) > 1500) {  // Débounce 1.5s après dernière modif
      saveRequested = false;
      
      // Vérifications de sécurité avant sauvegarde
      if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING && !xPortInIsrContext()) {
        // Attendre un peu plus pour s'assurer que le contexte web est terminé
        vTaskDelay(pdMS_TO_TICKS(100));
        saveSettings();
        Serial.println("Settings saved from CountdownTask");
      } else {
        Serial.println("Unsafe context - delaying save");
        saveRequested = true; // Re-programmer la sauvegarde
        saveRequestTime = millis() + 500; // Dans 500ms
      }
    }
    
    // Utiliser MUTEX_GUARD avec timeout optimisé
    {
      MUTEX_GUARD(countdownMutex, MUTEX_TIMEOUT_NORMAL);
      if (guard_countdownMutex.isLocked()) {
        DateTime now = rtc.now();
        if (now.isValid()) {  // Vérification de la validité de la date/heure
          if (now >= countdownTarget) {
            countdownExpired = true;
          }
        } else {
          Serial.println("RTC read error!");
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// Ancienne NetworkTask fusionnée dans NetWebTask

void loop() {
  // Surveillance du système et pause pour éviter les problèmes de watchdog
  systemWatchdog();
  
  // Les tâches FreeRTOS gèrent tout le travail
  vTaskDelay(pdMS_TO_TICKS(1000));
}
  
