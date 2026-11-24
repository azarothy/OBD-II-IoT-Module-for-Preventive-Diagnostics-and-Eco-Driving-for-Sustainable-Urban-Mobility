/*
  OBD22 - Painel OBD-II com ESP32 + ELM327 (Bluetooth Classico) + OLED 128x64

  - Conecta no ELM327 pelo MAC, sem pareamento (o modulo nao aceita PIN)
  - Mostra RPM, velocidade, temperatura do motor e tensao da bateria
  - Sem carro: mostra "SEM ECU", tracos nos dados do motor e a tensao real
  - Procura o carro sozinho a cada 5 s e reconecta se o Bluetooth cair

  Bibliotecas: ELMduino, Adafruit SSD1306, Adafruit GFX
  Monitor Serial: 115200 baud
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "BluetoothSerial.h"
#include "ELMduino.h"           // A classe continua se chamando ELM327
#include <WiFi.h>
#include <HTTPClient.h>
#include "secrets.h"            // credenciais (copie secrets.example.h)

#if !defined(CONFIG_BT_SPP_ENABLED)
#error "Bluetooth Classico (SPP) indisponivel. Precisa ser ESP32 comum (WROOM/WROVER), nao C3/S3/C6."
#endif

// ========= CONFIGURAÇÕES DO DISPLAY =========
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1        // Sem pino de reset
#define OLED_ADDRESS  0x3C      // Se não funcionar, tente 0x3D
#define PINO_SDA      21
#define PINO_SCL      22

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ========= CONFIGURAÇÕES DO ELM327 =========
// MAC do modulo ELM327 vem de secrets.h
uint8_t MAC_ELM[6] = ELM327_MAC;

#define CANAL_SPP        1       // Canal serial do ELM327 (funcionou no teste)
#define DEBUG_ELM        false   // true = mostra no Serial todos os comandos da ELMduino
#define TIMEOUT_ECU      2000    // ms: tempo máximo de resposta com o carro ligado
#define TIMEOUT_BUSCA    10000   // ms: busca de protocolo quando não há carro
#define INTERVALO_SONDA  5000    // ms entre tentativas de achar o carro (modo SEM ECU)
#define INTERVALO_TELA   250     // ms entre atualizações do display

// ========= ENVIO PARA O SERVIDOR (credenciais ficam em secrets.h) =========
#define INTERVALO_ENVIO       3000    // ms entre envios
#define ENVIAR_MESMO_SEM_ECU  false   // true = envia zeros na bancada, so para testar o fluxo

BluetoothSerial SerialBT;
ELM327 myELM327;

// ========= DADOS =========
uint32_t rpm = 0;
int32_t  speedKmh = 0;
float    coolantTemp = 0;
float    tensaoBateria = 0;
float    tps = 0;
int      score = 100;
unsigned long ultimoEnvio = 0;

// ========= ESTADO =========
bool btConectado   = false;
bool elmIniciado   = false;     // begin() só pode ser chamado uma vez (aloca memória)
bool ecuOk         = false;     // O carro está respondendo?
uint8_t errosSeguidos = 0;

unsigned long ultimaTentativaBT = 0;
unsigned long ultimaSonda       = 0;
unsigned long fimUltimaConsulta = 0;
unsigned long pausaEntreConsultas = 0;
unsigned long ultimaTela        = 0;
unsigned long ultimoLog         = 0;

// Ordem das leituras com o carro ligado: RPM e velocidade com prioridade
enum Leitura { L_RPM, L_VELOCIDADE, L_TEMPERATURA, L_TENSAO, L_TPS };

const Leitura SEQUENCIA[] = {
    L_RPM, L_VELOCIDADE,
    L_RPM, L_VELOCIDADE,
    L_RPM, L_VELOCIDADE,
    L_TPS, L_TEMPERATURA, L_TENSAO
};
const uint8_t TAM_SEQUENCIA = sizeof(SEQUENCIA) / sizeof(SEQUENCIA[0]);

uint8_t posSequencia = 0;
Leitura leituraAtual = L_RPM;
bool consultando     = false;   // Tem uma pergunta esperando resposta?

// ========= PROTÓTIPOS =========
bool conectarOBD();
void entrarModoSemECU();
void lerOBD();
Leitura escolherLeitura();
void logSerial();
void showBootScreen();
void displayMessage(const char *line1, const char *line2);
void drawDashboard();

// =====================================================================

void setup() {
    Serial.begin(115200);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_SENHA);

    // ----- Inicializa o Display OLED -----
    Wire.begin(PINO_SDA, PINO_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS, true, false)) {
        Serial.println(F("Falha ao iniciar o OLED! Verifique a ligacao I2C."));
        while (true) {
            delay(1000); // Trava aqui se o display não for encontrado
        }
    }

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    showBootScreen();
    delay(2000);

    // ----- Inicializa o Bluetooth como mestre e conecta -----
    SerialBT.begin("ESP32_OBD", true);
    btConectado = conectarOBD();
    ultimaTentativaBT = millis();
}

void loop() {
    manterWifi();
    enviarDados();
    // ----- Sem conexão: tenta de novo a cada 5 s -----
    if (!btConectado) {
        if (millis() - ultimaTentativaBT > 5000) {
            btConectado = conectarOBD();
            ultimaTentativaBT = millis();
        }
        return;
    }

    // ----- Bluetooth caiu? -----
    if (!SerialBT.connected()) {
        Serial.println("[BT] Conexao perdida!");
        displayMessage("Conexao perdida", "Reconectando...");
        btConectado = false;
        ecuOk = false;
        ultimaTentativaBT = millis();
        return;
    }

    // ----- Leitura não bloqueante: chamar sempre, sem delay -----
    lerOBD();

    // ----- Atualiza a tela -----
    if (millis() - ultimaTela >= INTERVALO_TELA) {
        ultimaTela = millis();
        drawDashboard();
    }

    // ----- Resumo no Monitor Serial a cada 1 s -----
    if (millis() - ultimoLog >= 1000) {
        ultimoLog = millis();
        logSerial();
    }
}

// ========= CONEXÃO =========

bool conectarOBD() {
    displayMessage("Conectando ao", "OBD-II...");
    Serial.println("[BT] Conectando ao ELM327 (ate 10 s)...");

    // Conexão direta pelo MAC, canal 1, sem pareamento
    if (!SerialBT.connect(MAC_ELM, CANAL_SPP, ESP_SPP_SEC_NONE)) {
        Serial.println("[BT] Falha. O modulo esta ligado e perto?");
        displayMessage("ERRO:", "Bluetooth falhou");
        return false;
    }

    Serial.println("[BT] Conectado. Iniciando ELM327 (alguns segundos)...");
    displayMessage("Bluetooth OK", "Iniciando ELM327...");

    bool ok;
    if (!elmIniciado) {
        ok = myELM327.begin(SerialBT, DEBUG_ELM, TIMEOUT_ECU);
        elmIniciado = true;
    } else {
        // Reconexão: só reinicializa, sem alocar memória de novo
        myELM327.timeout_ms = TIMEOUT_ECU;
        ok = myELM327.initializeELM();
    }

    if (!ok) {
        Serial.println("[ELM] Nao respondeu aos comandos AT.");
        displayMessage("ERRO:", "ELM nao responde");
        SerialBT.disconnect();
        return false;
    }

    Serial.println("[ELM] Pronto! Procurando o carro...");
    entrarModoSemECU();
    consultando = false;
    ultimaSonda = millis() - INTERVALO_SONDA;   // Faz a primeira busca já
    return true;
}

// Sem carro: zera os dados do motor e usa timeout longo para a busca de protocolo
void entrarModoSemECU() {
    ecuOk = false;
    errosSeguidos = 0;
    rpm = 0;
    speedKmh = 0;
    coolantTemp = 0;
    tps = 0;
    myELM327.timeout_ms = TIMEOUT_BUSCA;
}

// ========= LEITURA DOS DADOS =========

void lerOBD() {
    // ----- Escolhe a próxima pergunta -----
    if (!consultando) {
        if (millis() - fimUltimaConsulta < pausaEntreConsultas) return;
        leituraAtual = escolherLeitura();
        consultando = true;
    }

    // ----- Envia / continua esperando a resposta -----
    float valor = 0;
    switch (leituraAtual) {
        case L_RPM:         valor = myELM327.rpm();               break;
        case L_VELOCIDADE:  valor = myELM327.kph();               break;
        case L_TEMPERATURA: valor = myELM327.engineCoolantTemp(); break;
        case L_TPS:         valor = myELM327.throttle();          break;
        case L_TENSAO:      valor = myELM327.batteryVoltage();    break;
    }

    if (myELM327.nb_rx_state == ELM_GETTING_MSG) {
        return;   // Ainda esperando: volta no próximo loop()
    }

    // ----- Resposta chegou (ou deu erro) -----
    consultando = false;
    bool ok = (myELM327.nb_rx_state == ELM_SUCCESS);

    if (leituraAtual == L_TENSAO) {
        // A tensão vem do próprio ELM327, funciona mesmo sem carro
        if (ok) tensaoBateria = valor;
    } else if (ok) {
        switch (leituraAtual) {
            case L_RPM:         rpm = (uint32_t)valor;      break;
            case L_VELOCIDADE:  speedKmh = (int32_t)valor;  break;
            case L_TEMPERATURA: coolantTemp = valor;        break;
            case L_TPS:         tps = valor;                break;
            default:                                        break;
        }
        errosSeguidos = 0;

        if (!ecuOk) {
            ecuOk = true;
            myELM327.timeout_ms = TIMEOUT_ECU;
            posSequencia = 0;
            Serial.println("[ECU] Carro respondendo!");
        }
    } else {
        // Sem carro cai aqui (UNABLE TO CONNECT / NO DATA): é esperado
        if (DEBUG_ELM) myELM327.printError();

        if (ecuOk && ++errosSeguidos >= 3) {
            Serial.println("[ECU] Carro parou de responder.");
            entrarModoSemECU();
        }
    }

    // A próxima busca pelo carro conta a partir do fim desta
    if (!ecuOk && leituraAtual == L_RPM) {
        ultimaSonda = millis();
    }

    // Sem carro não precisa ler a tensão tão rápido
    pausaEntreConsultas = ecuOk ? 0 : 500;
    fimUltimaConsulta = millis();
}

Leitura escolherLeitura() {
    if (ecuOk) {
        Leitura l = SEQUENCIA[posSequencia];
        posSequencia = (posSequencia + 1) % TAM_SEQUENCIA;
        return l;
    }

    // Sem carro: lê a tensão e, a cada 5 s, tenta achar a ECU pedindo o RPM
    if (millis() - ultimaSonda >= INTERVALO_SONDA) {
        return L_RPM;
    }
    return L_TENSAO;
}

void logSerial() {
    if (ecuOk) {
        Serial.printf("[DADOS] RPM: %lu | KM/H: %ld | Temp: %.0f C | Bateria: %.1f V\n",
                      (unsigned long)rpm, (long)speedKmh, coolantTemp, tensaoBateria);
    } else {
        Serial.printf("[DADOS] Sem ECU | Bateria: %.1f V\n", tensaoBateria);
    }
}

// ========= FUNÇÕES DE DESENHO =========

void showBootScreen() {
    display.clearDisplay();
    display.setTextSize(2);
    display.setCursor(15, 10);
    display.println("ESP32");
    display.setTextSize(1);
    display.setCursor(25, 35);
    display.println("OBD-II DASHBOARD");
    display.setCursor(35, 50);
    display.println("v2.0");
    display.display();
}

void displayMessage(const char *line1, const char *line2) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 15);
    display.println(line1);
    display.setCursor(0, 30);
    display.println(line2);
    display.display();
}

void drawDashboard() {
    display.clearDisplay();

    // ----- Cabeçalho com status -----
    display.fillRect(0, 0, 128, 10, SSD1306_WHITE);
    display.setTextSize(1);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(2, 1);
    display.print("OBD-II");

    const char *status;
    if (ecuOk)                                   status = "ECU OK";
    else if (consultando && leituraAtual == L_RPM) status = "BUSCANDO";
    else                                         status = "SEM ECU";
    display.setCursor(126 - strlen(status) * 6, 1);
    display.print(status);
    display.setTextColor(SSD1306_WHITE);

    // ----- RPM (lado esquerdo) -----
    display.setTextSize(1);
    display.setCursor(4, 14);
    display.print("RPM");

    display.setTextSize(2);
    display.setCursor(4, 25);
    if (ecuOk) display.print(rpm);
    else       display.print("--");

    // Barrinha de RPM (0 a 7000)
    long rpmLimitado = (rpm > 7000) ? 7000 : (long)rpm;
    int barWidth = ecuOk ? (int)(rpmLimitado * 56 / 7000) : 0;
    display.drawRect(4, 44, 56, 6, SSD1306_WHITE);
    display.fillRect(4, 44, barWidth, 6, SSD1306_WHITE);

    // ----- VELOCIDADE (lado direito) -----
    display.setTextSize(1);
    display.setCursor(72, 14);
    display.print("KM/H");

    display.setTextSize(2);
    display.setCursor(72, 25);
    if (ecuOk) display.print(speedKmh);
    else       display.print("--");

    // ----- Rodapé: temperatura e bateria -----
    display.setTextSize(1);
    display.setCursor(4, 55);
    display.print("Temp:");
    if (ecuOk) {
        display.print((int)coolantTemp);
        display.print((char)247);   // Símbolo de grau
        display.print("C");
    } else {
        display.print("--");
    }

    char txtTensao[8];
    if (tensaoBateria > 0) snprintf(txtTensao, sizeof(txtTensao), "%.1fV", tensaoBateria);
    else                   strcpy(txtTensao, "--V");
    display.setCursor(126 - strlen(txtTensao) * 6, 55);
    display.print(txtTensao);

    display.display();
}


// ========= WI-FI E ENVIO AO SERVIDOR =========

void manterWifi() {
    static unsigned long ultimaTentativaWifi = 0;
    if (WiFi.status() == WL_CONNECTED) {
        static bool avisou = false;
        if (!avisou) { avisou = true; Serial.print("[WIFI] Conectado. IP: "); Serial.println(WiFi.localIP()); }
        return;
    }
    if (millis() - ultimaTentativaWifi < 10000) return;
    ultimaTentativaWifi = millis();
    Serial.println("[WIFI] Reconectando...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_SENHA);
}

// Score provisorio: perde pontos por RPM alto e acelerador forte
int calcularScore() {
    float penRpm = (rpm > 3000) ? (rpm - 3000) / 50.0f : 0;
    if (penRpm > 40) penRpm = 40;
    float penTps = (tps > 60) ? (tps - 60) * 0.75f : 0;
    if (penTps > 30) penTps = 30;
    float s = 100 - penRpm - penTps;
    return (s < 0) ? 0 : (int)s;
}

void enviarDados() {
    if (!ecuOk && !ENVIAR_MESMO_SEM_ECU) return;
    if (consultando) return;                       // so envia entre duas consultas ao carro
    if (WiFi.status() != WL_CONNECTED) return;
    if (millis() - ultimoEnvio < INTERVALO_ENVIO) return;
    ultimoEnvio = millis();
    score = calcularScore();
    char body[200];
    snprintf(body, sizeof(body),
             "{\"dev\":\"esp32-01\",\"rpm\":%lu,\"vel\":%ld,\"temp\":%d,\"tps\":%d,\"score\":%d,\"tensao\":%.1f}",
             (unsigned long)rpm, (long)speedKmh, (int)coolantTemp, (int)tps, score, (double)tensaoBateria);
    HTTPClient http;
    http.setConnectTimeout(800);                   // nao trava a leitura do carro
    http.setTimeout(1500);
    http.begin(API_URL);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("x-api-key", API_KEY);
    int codigo = http.POST(body);
    Serial.printf("[API] POST -> %d\n", codigo);
    http.end();
}
