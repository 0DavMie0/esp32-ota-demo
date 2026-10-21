/*
 * ---------------------------------------------------------------------------
 *  Demo de actualización OTA desde GitHub para ESP32-2432S024R
 *  - Muestra la versión instalada
 *  - Botón para comprobar si hay una versión nueva publicada en GitHub
 *  - Pregunta antes de actualizar y muestra el progreso
 *  - Todo se puede manejar también desde la consola serie (escribe "ayuda")
 *
 *  La configuración de la pantalla está en platformio.ini (build_flags)
 *  La versión está en include/version.h
 *
 *  Nota: las fuentes integradas de TFT_eSPI no tienen tildes ni eñes,
 *  por eso los textos en pantalla van sin ellas.
 * ---------------------------------------------------------------------------
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiManager.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include "version.h"

// ---- CAMBIA ESTO por tu usuario de GitHub ----
const char* GITHUB_USER = "0DavMie0";
const char* GITHUB_REPO = "esp32-ota-demo";

// Nombre de la red que crea la placa para configurar el WiFi desde el móvil
const char* SETUP_AP_NAME = "ESP32-OTA-Setup";

// ---- Pines del táctil (los mismos que en tu sketch del radar) ----
#define XPT2046_IRQ  36
#define XPT2046_MOSI 13
#define XPT2046_MISO 12
#define XPT2046_CLK  14
#define XPT2046_CS   33

#define SCREEN_W 320
#define SCREEN_H 240

// ---- Objetos globales ----
TFT_eSPI tft = TFT_eSPI();
SPIClass touchSPI = SPIClass(HSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);
WiFiManager wm;

// ---- Botones ----
struct Button {
  int x, y, w, h;
  const char* label;
  uint16_t color;
};

Button btnCheck = { 40, 165, 240, 50, "Buscar actualizacion", TFT_BLUE };
Button btnYes   = { 30, 165, 120, 50, "Si", TFT_DARKGREEN };
Button btnNo    = { 170, 165, 120, 50, "No", TFT_MAROON };

// ---- Estado ----
enum Screen { SCREEN_WIFI_SETUP, SCREEN_MAIN, SCREEN_CONFIRM };
Screen currentScreen = SCREEN_WIFI_SETUP;
bool wifiReady = false;
String latestVersion = "";
int lastProgressPct = -1;

// ---- Consola serie ----
enum ConsoleMode { SERIAL_COMMAND, SERIAL_WAIT_SSID, SERIAL_WAIT_PASS };
ConsoleMode serialMode = SERIAL_COMMAND;
String serialBuffer = "";
String pendingSsid = "";

// =========================================================================
//  DIBUJO (cada pantalla escribe también su contenido en la consola serie)
// =========================================================================

void drawButton(const Button& b) {
  tft.fillRoundRect(b.x, b.y, b.w, b.h, 8, b.color);
  tft.drawRoundRect(b.x, b.y, b.w, b.h, 8, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, b.color);
  tft.drawString(b.label, b.x + b.w / 2, b.y + b.h / 2, 2);
}

// Mensaje en la zona central de la pantalla principal (y en la consola)
void showStatus(const String& msg, uint16_t color) {
  Serial.println("> " + msg);
  tft.fillRect(0, 105, SCREEN_W, 40, TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(msg, SCREEN_W / 2, 125, 2);
}

// Pantalla con un único mensaje centrado (y en la consola)
void showMessageScreen(const String& msg, uint16_t color) {
  Serial.println("> " + msg);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(msg, SCREEN_W / 2, SCREEN_H / 2, 2);
}

void drawMainScreen() {
  currentScreen = SCREEN_MAIN;
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("Actualizada desde Github", SCREEN_W / 2, 20, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Version " FW_VERSION, SCREEN_W / 2, 65, 4);
  drawButton(btnCheck);

  Serial.println();
  Serial.println("=== Pantalla principal: Version " FW_VERSION " ===");
  Serial.println("Escribe 'buscar' para comprobar actualizaciones ('ayuda' para ver todo)");
}

void drawConfirmScreen() {
  currentScreen = SCREEN_CONFIRM;
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("Hay version nueva", SCREEN_W / 2, 40, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(String("Instalada: " FW_VERSION "   ->   Nueva: ") + latestVersion,
                 SCREEN_W / 2, 85, 2);
  tft.drawString("Quieres actualizar ahora?", SCREEN_W / 2, 120, 2);
  drawButton(btnYes);
  drawButton(btnNo);

  Serial.println();
  Serial.println("=== Hay una versión nueva ===");
  Serial.println("Instalada: " FW_VERSION "  ->  Nueva: " + latestVersion);
  Serial.println("¿Actualizar ahora? Escribe 'si' o 'no'");
}

// =========================================================================
//  TÁCTIL
// =========================================================================

// Devuelve true si se ha pulsado la pantalla, con las coordenadas en píxeles.
// La calibración es la misma de tu sketch del radar.
bool readTouch(int& x, int& y) {
  if (!(touchscreen.tirqTouched() && touchscreen.touched())) return false;
  TS_Point p = touchscreen.getPoint();
  x = map(p.x, 240, 3700, 1, SCREEN_W);
  y = map(p.y, 200, 3800, 1, SCREEN_H);
  while (touchscreen.touched()) delay(10);  // Esperar a que se suelte el dedo
  return true;
}

bool isInside(const Button& b, int x, int y) {
  return x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h;
}

// =========================================================================
//  WIFI
// =========================================================================

// Se muestra cuando la placa no consigue conectarse a ninguna red guardada
void onConfigPortalStarted(WiFiManager*) {
  currentScreen = SCREEN_WIFI_SETUP;
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Configura el WiFi", SCREEN_W / 2, 35, 4);
  tft.drawString("1. Conecta el movil a la red:", SCREEN_W / 2, 85, 2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(SETUP_AP_NAME, SCREEN_W / 2, 115, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("2. Elige tu red y escribe la clave", SCREEN_W / 2, 155, 2);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("O configurala por el puerto serie", SCREEN_W / 2, 190, 2);

  Serial.println();
  Serial.println("=== No hay WiFi configurada (o no se puede conectar) ===");
  Serial.println("Escribe 'wifi' para configurarla desde aquí,");
  Serial.println(String("o conecta el móvil a la red ") + SETUP_AP_NAME);
}

void onWiFiConnected() {
  wifiReady = true;
  if (wm.getConfigPortalActive()) wm.stopConfigPortal();
  Serial.print("> WiFi conectada a \"");
  Serial.print(WiFi.SSID());
  Serial.print("\", IP: ");
  Serial.println(WiFi.localIP());
  drawMainScreen();
}

void startWiFi() {
  // Si se mantiene pulsada la pantalla al arrancar, se olvida la red guardada
  if (touchscreen.touched()) {
    wm.resetSettings();
    showMessageScreen("WiFi borrada", TFT_ORANGE);
    while (touchscreen.touched()) delay(10);
    delay(1500);
  }

  showMessageScreen("Conectando a WiFi...", TFT_YELLOW);

  wm.setAPCallback(onConfigPortalStarted);
  wm.setConnectTimeout(20);
  wm.setConfigPortalBlocking(false);  // Para poder atender la consola mientras tanto
  if (wm.autoConnect(SETUP_AP_NAME)) {
    onWiFiConnected();
  }
}

// Conecta con la red indicada por la consola serie. La red queda guardada.
void connectWithCredentials(const String& ssid, const String& pass) {
  if (wm.getConfigPortalActive()) wm.stopConfigPortal();
  currentScreen = SCREEN_WIFI_SETUP;
  showMessageScreen("Conectando a " + ssid + "...", TFT_YELLOW);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("> Conectado. La red queda guardada en la placa.");
    onWiFiConnected();
  } else {
    wifiReady = false;
    showMessageScreen("No se ha podido conectar a la WiFi", TFT_RED);
    Serial.println("  Revisa el nombre y la clave y vuelve a probar con 'wifi'.");
    Serial.println("  Usa 'redes' para ver los nombres exactos de las redes cercanas.");
  }
}

void scanNetworks() {
  Serial.println("> Buscando redes...");
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.println("> No se ha encontrado ninguna red");
    return;
  }
  for (int i = 0; i < n; i++) {
    Serial.printf("  %-32s %4d dBm  %s\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                  WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "(abierta)" : "");
  }
  WiFi.scanDelete();
}

// =========================================================================
//  COMPROBACIÓN DE VERSIONES
// =========================================================================

// Consulta en GitHub cuál es la última versión publicada.
// Devuelve el número (por ejemplo "1.1") o "" si hay error.
String fetchLatestVersion(int& httpCode) {
  WiFiClientSecure client;
  client.setInsecure();  // No verifica el certificado (lo mejoraremos más adelante)

  HTTPClient http;
  String url = String("https://api.github.com/repos/") + GITHUB_USER + "/" +
               GITHUB_REPO + "/releases/latest";
  if (!http.begin(client, url)) {
    httpCode = -1;
    return "";
  }
  http.setUserAgent("ESP32-OTA-demo");  // GitHub exige identificarse
  http.setTimeout(10000);

  httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("> GitHub ha respondido con el codigo %d al consultar:\n", httpCode);
    Serial.println("  " + url);
    http.end();
    return "";
  }
  String body = http.getString();
  http.end();

  // Buscar "tag_name": "v1.1" dentro de la respuesta
  int i = body.indexOf("\"tag_name\"");
  if (i < 0) return "";
  int start = body.indexOf('"', body.indexOf(':', i)) + 1;
  int end = body.indexOf('"', start);
  String tag = body.substring(start, end);
  if (tag.startsWith("v")) tag = tag.substring(1);
  return tag;
}

// Devuelve true si la versión "a" es más nueva que "b" (formato "1.2")
bool isNewer(const String& a, const String& b) {
  int aMajor = a.toInt();
  int bMajor = b.toInt();
  if (aMajor != bMajor) return aMajor > bMajor;
  int aMinor = a.substring(a.indexOf('.') + 1).toInt();
  int bMinor = b.substring(b.indexOf('.') + 1).toInt();
  return aMinor > bMinor;
}

void checkForUpdates() {
  if (WiFi.status() != WL_CONNECTED) {
    showStatus("Sin conexion WiFi", TFT_RED);
    return;
  }
  showStatus("Comprobando...", TFT_YELLOW);

  int httpCode = 0;
  String latest = fetchLatestVersion(httpCode);

  if (latest == "") {
    if (httpCode == 404) {
      showStatus("Repositorio o version no encontrados", TFT_ORANGE);
      Serial.println("  Comprueba que el usuario y el repositorio de la direccion son correctos,");
      Serial.println("  que el repositorio es publico y que tiene alguna version publicada.");
    } else {
      showStatus("Error al consultar GitHub (" + String(httpCode) + ")", TFT_RED);
    }
    return;
  }

  Serial.println("> Ultima version en GitHub: " + latest);
  if (isNewer(latest, FW_VERSION)) {
    latestVersion = latest;
    drawConfirmScreen();
  } else {
    showStatus("Ya tienes la ultima version", TFT_GREEN);
  }
}

// =========================================================================
//  ACTUALIZACIÓN
// =========================================================================

void drawProgress(int current, int total) {
  if (total <= 0) return;
  int pct = (int)((current * 100LL) / total);
  if (pct == lastProgressPct) return;

  // En la consola, una línea cada 10 %
  if (lastProgressPct < 0 || pct / 10 != lastProgressPct / 10) {
    Serial.printf("> Descargando... %d%%\n", pct);
  }
  lastProgressPct = pct;

  tft.fillRect(32, 152, (256 * pct) / 100, 20, TFT_GREEN);
  tft.fillRect(0, 185, SCREEN_W, 20, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(String(pct) + "%", SCREEN_W / 2, 195, 2);
}

void doUpdate() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(String("Actualizando a ") + latestVersion, SCREEN_W / 2, 60, 4);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("No apagues la placa", SCREEN_W / 2, 100, 2);
  tft.drawRect(30, 150, 260, 24, TFT_WHITE);
  lastProgressPct = -1;

  WiFiClientSecure client;
  client.setInsecure();

  String url = String("https://github.com/") + GITHUB_USER + "/" + GITHUB_REPO +
               "/releases/download/v" + latestVersion + "/firmware.bin";
  Serial.println("> Actualizando a la version " + latestVersion + ". No apagues la placa.");
  Serial.println("  " + url);

  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // GitHub redirige la descarga
  httpUpdate.rebootOnUpdate(false);  // Reiniciamos nosotros tras mostrar el mensaje
  httpUpdate.onProgress(drawProgress);

  t_httpUpdate_return result = httpUpdate.update(client, url);

  if (result == HTTP_UPDATE_OK) {
    Serial.println("> Actualizacion completada. Reiniciando...");
    tft.fillRect(0, 185, SCREEN_W, 20, TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("Listo. Reiniciando...", SCREEN_W / 2, 215, 2);
    delay(1500);
    ESP.restart();
  } else {
    Serial.printf("> Error en la actualizacion (%d): %s\n",
                  httpUpdate.getLastError(),
                  httpUpdate.getLastErrorString().c_str());
    tft.fillRect(0, 140, SCREEN_W, 100, TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("Error en la actualizacion (" + String(httpUpdate.getLastError()) + ")",
                   SCREEN_W / 2, 170, 2);
    delay(4000);
    drawMainScreen();
  }
}

// =========================================================================
//  CONSOLA SERIE
// =========================================================================

void printHelp() {
  Serial.println();
  Serial.println("Comandos disponibles:");
  Serial.println("  buscar     Comprobar si hay actualizacion (como pulsar el boton)");
  Serial.println("  si / no    Responder cuando pregunta si actualizar");
  Serial.println("  wifi       Configurar la red WiFi (pide nombre y clave)");
  Serial.println("  redes      Ver las redes WiFi cercanas");
  Serial.println("  estado     Ver version, WiFi, IP y memoria");
  Serial.println("  olvidar    Borrar la WiFi guardada y reiniciar");
  Serial.println("  reiniciar  Reiniciar la placa");
  Serial.println("  ayuda      Ver esta lista");
}

void printState() {
  Serial.println();
  Serial.println("Version:  " FW_VERSION);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi:     conectada a \"%s\" (%d dBm)\n", WiFi.SSID().c_str(), WiFi.RSSI());
    Serial.print("IP:       ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi:     sin conexion");
  }
  Serial.printf("Memoria:  %u bytes libres\n", ESP.getFreeHeap());
}

void handleCommand(String cmd) {
  cmd.trim();
  cmd.toLowerCase();

  if (cmd == "buscar") {
    if (currentScreen == SCREEN_MAIN) checkForUpdates();
    else if (currentScreen == SCREEN_CONFIRM) Serial.println("> Hay una actualizacion pendiente: escribe 'si' o 'no'");
    else Serial.println("> Primero configura la WiFi con 'wifi'");
  } else if (cmd == "si" || cmd == "sí") {
    if (currentScreen == SCREEN_CONFIRM) doUpdate();
    else Serial.println("> No hay ninguna actualizacion pendiente de confirmar");
  } else if (cmd == "no") {
    if (currentScreen == SCREEN_CONFIRM) {
      Serial.println("> Actualizacion cancelada");
      drawMainScreen();
    } else {
      Serial.println("> No hay ninguna actualizacion pendiente de confirmar");
    }
  } else if (cmd == "wifi") {
    Serial.println("> Escribe el nombre de la red (o 'cancelar'):");
    serialMode = SERIAL_WAIT_SSID;
  } else if (cmd == "redes") {
    scanNetworks();
  } else if (cmd == "estado") {
    printState();
  } else if (cmd == "olvidar") {
    Serial.println("> Borrando la WiFi guardada y reiniciando...");
    wm.resetSettings();
    delay(500);
    ESP.restart();
  } else if (cmd == "reiniciar") {
    Serial.println("> Reiniciando...");
    delay(500);
    ESP.restart();
  } else if (cmd == "ayuda" || cmd == "?") {
    printHelp();
  } else {
    Serial.println("> Comando desconocido: '" + cmd + "'. Escribe 'ayuda'.");
  }
}

void processLine(const String& line) {
  switch (serialMode) {
    case SERIAL_WAIT_SSID:
      if (line == "cancelar") {
        Serial.println("> Cancelado");
        serialMode = SERIAL_COMMAND;
      } else {
        pendingSsid = line;
        Serial.println("> Escribe la clave:");
        serialMode = SERIAL_WAIT_PASS;
      }
      break;
    case SERIAL_WAIT_PASS:
      serialMode = SERIAL_COMMAND;
      connectWithCredentials(pendingSsid, line);
      break;
    default:
      handleCommand(line);
      break;
  }
}

// Lee la consola sin bloquear y procesa cada línea al recibir Enter
void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      if (serialBuffer.length() > 0) {  // Ignorar líneas vacías
        processLine(serialBuffer);
        serialBuffer = "";
      }
    } else if (serialBuffer.length() < 100) {
      serialBuffer += c;
    }
  }
}

// =========================================================================
//  SETUP Y LOOP
// =========================================================================

void handleTouch(int x, int y) {
  if (currentScreen == SCREEN_MAIN) {
    if (isInside(btnCheck, x, y)) checkForUpdates();
  } else if (currentScreen == SCREEN_CONFIRM) {
    if (isInside(btnYes, x, y)) {
      doUpdate();
    } else if (isInside(btnNo, x, y)) {
      Serial.println("> Actualizacion cancelada");
      drawMainScreen();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("===================================");
  Serial.println(" ESP32 OTA demo - version " FW_VERSION);
  Serial.println(" Escribe 'ayuda' para ver comandos");
  Serial.println("===================================");

  tft.begin();
  tft.setRotation(1);
  tft.setTextDatum(MC_DATUM);  // Los textos se centran en el punto indicado

  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin(touchSPI);
  touchscreen.setRotation(1);

  startWiFi();
}

void loop() {
  // Mientras no haya WiFi, atender el portal del móvil y vigilar si se conecta
  if (!wifiReady) {
    wm.process();
    if (WiFi.status() == WL_CONNECTED) onWiFiConnected();
  }

  handleSerial();

  int x, y;
  if (readTouch(x, y)) handleTouch(x, y);
}
