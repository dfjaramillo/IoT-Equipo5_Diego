#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <PubSubClient.h>
#include <DHT.h>


// Definiciones

// Ancho de la pantalla (en pixeles)
#define SCREEN_WIDTH 128
// Alto de la pantalla (en pixeles)
#define SCREEN_HEIGHT 64
// Pin del sensor de temperatura y humedad
#define DHTPIN 5 // D1 (GPIO5)
// Tipo de sensor de temperatura y humedad
#define DHTTYPE DHT11
// Intervalo en segundos de las mediciones
#define MEASURE_INTERVAL 2
// Duración aproximada en la pantalla de las alertas que se reciban
#define ALERT_DURATION 60
// Pin de control del circuito oscilador NE555 (activa/desactiva LEDs alternados)
#define NE555_CTRL_PIN 4 // D2 (GPIO4) - Conectado al pin RESET (pin 4) del NE555
// Pin analógico para la fotocelda (LDR)
#define LDR_PIN A0 // A0 - único pin analógico del ESP8266

// Pines del display SPI
#define OLED_MOSI  13  // D7 - SDA del display
#define OLED_CLK   14  // D5 - SCL del display
#define OLED_DC    12  // D6
#define OLED_CS    15  // D8
#define OLED_RESET  0  // D3


// Declaraciones

// Sensor DHT
DHT dht(DHTPIN, DHTTYPE);
// Pantalla OLED (SPI)
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, OLED_MOSI, OLED_CLK, OLED_DC, OLED_RESET, OLED_CS);
// Cliente WiFi
WiFiClient net;
// Cliente MQTT
PubSubClient client(net);


// Variables a editar TODO

// WiFi
const char ssid[] = "Netzwerk";
const char pass[] = "Isaac2023";

// Conexión a Mosquitto
#define USER "user1"
const char MQTT_HOST[] = "34.238.235.55";
const int MQTT_PORT = 8082;
const char MQTT_CLIENT_ID[] = "MCU_user1";  // Client ID único para este dispositivo
const char MQTT_USER[] = "user1";
const char MQTT_PASS[] = "123456";

// Tópicos MQTT
const char MQTT_TOPIC_PUB[] = "colombia/cundinamarca/bogota/" USER "/out";
const char MQTT_TOPIC_SUB[] = "colombia/cundinamarca/bogota/" USER "/in";

// Variables globales
time_t now;
long long int measureTime = millis();
long long int alertTime = millis();
String alert = "";
float temp;
float humi;
float lumi; // Valor de luminosidad (0-1024)

// Variables para alerta de luminosidad y control del NE555
bool luminosityAlertActive = false;
String luminosityAlertMsg = "";

/**
 * Conecta el dispositivo con el bróker MQTT.
 */
void mqtt_connect()
{
  while (!client.connected()) {
    Serial.print("MQTT connecting ... ");
    if (client.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)) {
      Serial.println("connected.");
      client.subscribe(MQTT_TOPIC_SUB);
    } else {
      Serial.println("Problema con la conexión, revise los valores de las constantes MQTT");
      int state = client.state();
      Serial.print("Código de error = ");
      alert = "MQTT error: " + String(state);
      Serial.println(state);
      if ( client.state() == MQTT_CONNECT_UNAUTHORIZED ) {
        ESP.deepSleep(0);
      }
      delay(5000);
    }
  }
}

/**
 * Publica la temperatura, humedad y luminosidad al tópico configurado.
 */
void sendSensorData(float temperatura, float humedad, float luminosidad) {
  String data = "{";
  data += "\"temperatura\": "+ String(temperatura, 1) +", ";
  data += "\"humedad\": "+ String(humedad, 1) +", ";
  data += "\"luminosidad\": "+ String(luminosidad, 1);
  data += "}";
  char payload[data.length()+1];
  data.toCharArray(payload,data.length()+1);
  client.publish(MQTT_TOPIC_PUB, payload);
  Serial.println("datos enviados");
}

/**
 * Lee la temperatura del sensor DHT, la imprime en consola y la devuelve.
 */
float readTemperatura() {
  
  // Se lee la temperatura en grados centígrados (por defecto)
  float t = dht.readTemperature();
  
  Serial.print("Temperatura: ");
  Serial.print(t);
  Serial.println(" *C ");
  
  return t;
}

/**
 * Lee la humedad del sensor DHT, la imprime en consola y la devuelve.
 */
float readHumedad() {
  // Se lee la humedad relativa
  float h = dht.readHumidity();
  
  Serial.print("Humedad: ");
  Serial.print(h);
  Serial.println(" %\t");

  return h;
}

/**
 * Lee la luminosidad desde la fotocelda (LDR) conectada al pin A0.
 * Retorna un valor entre 0 (baja luminosidad) y 100 (alta luminosidad).
 */
float readLuminosidad() {
  int raw = analogRead(LDR_PIN);
  float l = (raw / 1024.0) * 100.0;

  Serial.print("Luminosidad: ");
  Serial.print(l, 1);
  Serial.println(" %");

  return l;
}

/**
 * Verifica si las variables son números válidos.
 */
bool checkMeasures(float t, float h) {
  if (isnan(t) || isnan(h)) {
    Serial.println("Error obteniendo los datos del sensor DHT11");
    return false;
  }
  return true;
}

/**
 * Inicia la pantalla OLED por SPI.
 */
void startDisplay() {
  if(!display.begin(SSD1306_SWITCHCAPVCC)) {
    Serial.println(F("SSD1306 allocation failed"));
    for(;;);
  }
  display.setTextColor(SSD1306_WHITE);
}

/**
 * Imprime en la pantalla "No hay señal".
 */
void displayNoSignal() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(10, 10);
  display.println("No hay señal");
  display.display();
}

/**
 * Agrega el header "IOT Sensors" y la hora actual.
 */
void displayHeader() {
  display.setTextSize(1);
  long long int milli = now + millis() / 1000;
  struct tm* tinfo;
  tinfo = localtime(&milli);
  String hour = String(asctime(tinfo)).substring(11, 19);
  String title = "IOT Sensors  " + hour;
  display.println(title);
}

/**
 * Agrega los valores de temperatura y humedad a la pantalla.
 */
void displayMeasures() {
  display.println("");
  display.print("T:");
  display.print(temp, 1);
  display.print(" H:");
  display.print(humi, 1);
  display.println("");
  display.print("L: ");
  display.print(lumi, 1);
  display.println(" %");
}

/**
 * Agrega el mensaje indicado a la pantalla.
 */
void displayMessage(String message) {
  display.setTextSize(1);
  display.println("\nMsg:");
  display.setTextSize(2);
  if (message.equals("OK")) {
    display.println("    " + message);
  } else {
    display.setTextSize(1);
    display.println(message);
  }
}

/**
 * Muestra "Connecting to:" y el nombre de la red.
 */
void displayConnecting(String ssid) {
  display.clearDisplay();
  display.setTextSize(1);
  display.println("Connecting to:\n");
  display.println(ssid);
  display.display();
}

/**
 * Verifica si ha llegado alguna alerta.
 */
String checkAlert() {
  String message = "OK";
  if (alert.length() != 0) {
    message = alert;
    if ((millis() - alertTime) >= ALERT_DURATION * 1000 ) {
      alert = "";
      alertTime = millis();
    }
  }
  return message;
}

/**
 * Función callback cuando llega un mensaje MQTT.
 */
void receivedCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Received [");
  Serial.print(topic);
  Serial.print("]: ");
  String data = "";
  for (int i = 0; i < length; i++) {
    data += String((char)payload[i]);
  }
  Serial.println(data);

  // Procesar alerta específica de luminosidad (controla el oscilador NE555)
  if (data.indexOf("LUMINOSITY_ALERT") >= 0) {
    if (data.indexOf("LUMINOSITY_ALERT ON") >= 0) {
      luminosityAlertActive = true;
      luminosityAlertMsg = data.substring(20); // Extraer datos después de "LUMINOSITY_ALERT ON "
      digitalWrite(NE555_CTRL_PIN, HIGH);       // Activar oscilador NE555 -> LEDs parpadean alternados
      Serial.println("Alerta de luminosidad");
    } else if (data.indexOf("LUMINOSITY_ALERT OFF") >= 0) {
      luminosityAlertActive = false;
      luminosityAlertMsg = "";
      digitalWrite(NE555_CTRL_PIN, LOW);        // Desactivar oscilador NE555 -> LEDs apagados
      Serial.println("Luminosidad normal");
    }
    alert = data;
    alertTime = millis();
  }
  // Procesar alertas genéricas (temperatura, etc.)
  else if (data.indexOf("ALERT") >= 0) {
    alert = data;
    alertTime = millis();
  }
}

/**
 * Verifica conexión WiFi y MQTT.
 */
void checkWiFi() {
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.print("Checking wifi");
    while (WiFi.waitForConnectResult() != WL_CONNECTED)
    {
      WiFi.begin(ssid, pass);
      Serial.print(".");
      displayNoSignal();
      delay(10);
    }
    Serial.println("connected");
  }
  else
  {
    if (!client.connected())
    {
      mqtt_connect();
    }
    else
    {
      client.loop();
    }
  }
}

/**
 * Lista las redes WiFi disponibles.
 */
void listWiFiNetworks() {
  int numberOfNetworks = WiFi.scanNetworks();
  Serial.println("\nNumber of networks: ");
  Serial.print(numberOfNetworks);
  for(int i =0; i<numberOfNetworks; i++){
    Serial.println(WiFi.SSID(i));
  }
}

/**
 * Inicia la conexión WiFi.
 */
void startWiFi() {
  WiFi.hostname(USER);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  Serial.println("(\n\nAttempting to connect to SSID: ");
  Serial.print(ssid);
  while (WiFi.status() != WL_CONNECTED)
  {
    if ( WiFi.status() == WL_NO_SSID_AVAIL ) {
      Serial.println("\nNo se encuentra la red WiFi ");
      Serial.print(ssid);
      WiFi.begin(ssid, pass);
    } else if ( WiFi.status() == WL_WRONG_PASSWORD ) {
      Serial.println("\nLa contraseña de la red WiFi no es válida.");
    } else if ( WiFi.status() == WL_CONNECT_FAILED ) {
      Serial.println("\nNo se ha logrado conectar con la red, resetee el node y vuelva a intentar");
      WiFi.begin(ssid, pass);
    }
    Serial.print(".");
    delay(1000);
  }
  Serial.println("connected!");
}

/**
 * Sincroniza la hora con servidores SNTP.
 */
void setTime() {
  Serial.print("Setting time using SNTP");
  configTime(-5 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  now = time(nullptr);
  while (now < 1510592825) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println("done!");
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  Serial.print("Current time: ");
  Serial.print(asctime(&timeinfo));
}

/**
 * Configura el servidor MQTT y la función callback.
 */
void configureMQTT() {
  client.setServer(MQTT_HOST, MQTT_PORT);
  client.setCallback(receivedCallback);
  mqtt_connect();
}

/**
 * Mide y envía datos si ya pasó el intervalo.
 */
void measure() {
  if ((millis() - measureTime) >= MEASURE_INTERVAL * 1000 ) {
    Serial.println("\nMidiendo variables...");
    measureTime = millis();
    temp = readTemperatura();
    humi = readHumedad();
    lumi = readLuminosidad();
    // Siempre envía (los datos simulados ya son válidos)
    sendSensorData(temp, humi, lumi);
  }
}

/////////////////////////////////////
//         FUNCIONES ARDUINO       //
/////////////////////////////////////

void setup() {
  Serial.begin(115200);
  randomSeed(analogRead(0));  // Semilla para datos simulados

  // Configurar pin de control del NE555 como salida (inicia apagado)
  pinMode(NE555_CTRL_PIN, OUTPUT);
  digitalWrite(NE555_CTRL_PIN, LOW);

  listWiFiNetworks();
  startDisplay();
  displayConnecting(ssid);
  startWiFi();
  dht.begin();
  setTime();
  configureMQTT();
}

void loop() {
  checkWiFi();
  String message = checkAlert();
  measure();

  display.clearDisplay();
  display.setCursor(0,0);
  displayHeader();
  displayMeasures();
  displayMessage(message);

  

  display.display();
}