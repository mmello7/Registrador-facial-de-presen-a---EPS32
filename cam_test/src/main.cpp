#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <SPIFFS.h>
#include <time.h>
#include "esp_camera.h"

// Para a criptografia AES
#include "mbedtls/aes.h"
#include <vector>
#include <memory>
#include <cstring> // Adicionado para operações de string/memória

// Ativa ou desativa a funcionalidade de Validação via Bluetooth Low Energy (BLE)
#define ENABLE_BLE 1
#if ENABLE_BLE
// Biblioteca instalada via platformio.ini
#include <NimBLEDevice.h>
#endif

// ===================== PINOUT DA CÂMERA (ESP32-CAM) =====================
#define PWDN_GPIO_NUM       32
#define RESET_GPIO_NUM      -1
#define XCLK_GPIO_NUM       0
#define SIOD_GPIO_NUM       26
#define SIOC_GPIO_NUM       27
#define Y9_GPIO_NUM         35
#define Y8_GPIO_NUM         34
#define Y7_GPIO_NUM         39
#define Y6_GPIO_NUM         36
#define Y5_GPIO_NUM         21
#define Y4_GPIO_NUM         19
#define Y3_GPIO_NUM         18
#define Y2_GPIO_NUM         5
#define VSYNC_GPIO_NUM      25
#define HREF_GPIO_NUM       23
#define PCLK_GPIO_NUM       22

// ===================== CONFIGURAÇÃO GERAL =====================

// Configurações Wi-Fi (Substitua pelos seus dados)
// NOTA DE SEGURANÇA: Dados sensíveis (como credenciais) devem ser armazenados
// em NVS (Non-Volatile Storage) ou em um arquivo de configuração seguro, e não hardcoded.
static const char* WIFI_SSID = "SEMPRE_COLABORADOR";
static const char* WIFI_PASS = "Sempre@2024#";

// Configurações do Servidor de Reconhecimento Facial (API)
static const char* SERVER_ADDRESS = "172.24.153.47"; 
static const uint16_t SERVER_PORT = 3000;
static const char* SERVER_ATTENDANCE_PATH = "/api/v1/attendance"; 

// Autenticação
static const char* AUTH_BEARER_TOKEN = "e83z9XyJk4mFpA6hD7qWc2sT1uVb0R3g";

// Identificação do Contexto da Chamada
static const char* SCHOOL_ROOM_ID = "SALA_101";
static const char* COURSE_ID = "DISCIPLINA_ABC123";

// Segurança TLS (HTTPS)
// 1 = Ignorar validação de certificado (Inseguro, APENAS para testes com servidores sem CA)
// Mude para 0 e defina SERVER_CA_CERT para uso em produção.
#define TLS_INSECURE 1

// Certificado Root CA do servidor (NECESSÁRIO para TLS_INSECURE=0 e para comunicação segura).
// Substitua pelo certificado real, caso use o servidor com domínio/certificado válido.
const char* SERVER_CA_CERT = \
"-----BEGIN CERTIFICATE-----\n" \
"MIIFazCCA1OgAwIBAgIRAIIQz7ZN0pLwYwAwDQYJKoZIhvcNAQELBQAwUjELMAkG\n" \
// ... Seu certificado CA ROOT aqui ...
"-----END CERTIFICATE-----\n";

// Configurações NTP (Network Time Protocol)
static const char* NTP_SERVER = "pool.ntp.org";
static const long GMT_OFFSET_SEC = -3 * 3600; // Fuso horário (Ex: Brasil -3h)
static const int DAYLIGHT_OFFSET_SEC = 0;

// Configurações de Desempenho da Câmera
#define FRAME_SIZE_DEFAULT FRAMESIZE_QVGA // QVGA (320x240) é um bom equilíbrio
#define JPEG_QUALITY_DEFAULT 12 // 0 (melhor qualidade) a 63 (pior qualidade)
#define FB_COUNT_DEFAULT 1 // Número de Frame Buffers

// Buffer Offline (SPIFFS)
static const char* OFFLINE_FILE = "/attendance_offline.ndjson";
// Chave e IV AES-128 para criptografia offline
static const uint8_t AES_KEY_128[16] = { 0x7a,0x21,0x93,0x44,0x55,0xC2,0x3E,0x19,0xA8,0x04,0x6B,0xDE,0xF0,0x17,0x52,0x9C };
static const uint8_t AES_IV_128[16]  = { 0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,0x10 };

// Janela de retenção offline (2 horas)
static const uint32_t OFFLINE_RETENTION_SEC = 2 * 3600;

// Variáveis de estado para a Ativação via BLE (Professor)
static volatile bool g_bleValidationRequired = true; // Começa exigindo ativação
static volatile uint32_t g_bleLastSeenMillis = 0;
static const uint32_t BLE_VALID_WINDOW_MS = 2 * 60 * 1000; // Janela de validação de 2 minutos

// ===================== PROTÓTIPOS =====================
static bool timeIsSet();
static String iso8601Now();
// Função de log otimizada para Flash Strings (uso de F("texto"))
static void logInfo(const __FlashStringHelper* msg);
static void logError(const __FlashStringHelper* msg);
static void logError(const char* msg); // Sobrecarga para strings dinâmicas
static void connectWiFi();
static bool initCamera();
static String base64Encode(const uint8_t* data, size_t len);
static std::vector<uint8_t> aesCbcEncrypt(const uint8_t* plaintext, size_t len);
static std::vector<uint8_t> aesCbcDecrypt(const uint8_t* ciphertext, size_t len);
static bool appendEncryptedOfflineLine(const String& jsonLine);
static bool tryDrainOfflineQueue(WiFiClientSecure& client);
static bool captureJpeg(std::unique_ptr<camera_fb_t, void(*)(camera_fb_t*)>& fbPtr);
static String makeAttendanceJson(const String& imageBase64, const String& timestampIso, const String& roomId, const String& courseId, const String& mode, const String& identity);
static bool postAttendance(WiFiClientSecure& client, const String& jsonPayload);
#if ENABLE_BLE
static void initBLE();
#endif

// ===================== UTILITÁRIOS E LOGS =====================

static bool timeIsSet() {
    time_t now;
    time(&now);
    // Verifica se o tempo está minimamente ajustado (após 1º de Janeiro de 2024)
    return now > 1704067200; 
}

static String iso8601Now() {
    time_t now;
    time(&now);
    struct tm timeinfo;
    // Usa gmtime_r (UTC) para o formato ISO 8601 com 'Z'
    gmtime_r(&now, &timeinfo); 
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo); 
    return String(buf);
}

// Implementações de Log (usando F() para economizar RAM)
static void logInfo(const __FlashStringHelper* msg) {
    Serial.println(msg);
}

static void logError(const __FlashStringHelper* msg) {
    Serial.print(F("[ERRO] "));
    Serial.println(msg);
}

static void logError(const char* msg) {
    Serial.print(F("[ERRO] "));
    Serial.println(msg);
}

// ===================== WI-FI E NTP =====================

static void connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;
    logInfo(F("Conectando Wi-Fi..."));
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        logInfo(F("Wi-Fi conectado."));
        // Configura o NTP apenas se conectar
        configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    } else {
        logError(F("Falha ao conectar Wi-Fi."));
    }
}

// ===================== CÂMERA =====================

static bool initCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;

    config.frame_size = FRAME_SIZE_DEFAULT;
    config.jpeg_quality = JPEG_QUALITY_DEFAULT;
    config.fb_count = FB_COUNT_DEFAULT;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        logError(F("Falha ao iniciar camera"));
        return false;
    }
    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        // Redefine o tamanho do frame para garantir que a configuração seja aplicada
        s->set_framesize(s, FRAME_SIZE_DEFAULT);
    }
    logInfo(F("Camera inicializada."));
    return true;
}

// ===================== CRIPTOGRAFIA (AES-128 CBC) =====================

// Função Base64 Encode mantida
static String base64Encode(const uint8_t* data, size_t len) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    String out;
    out.reserve((len * 4) / 3 + 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (uint32_t)data[i] << 16;
        if (i + 1 < len) n |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) n |= (uint32_t)data[i + 2];
        out += tbl[(n >> 18) & 63];
        out += tbl[(n >> 12) & 63];
        out += (i + 1 < len) ? tbl[(n >> 6) & 63] : '=';
        out += (i + 2 < len) ? tbl[n & 63] : '=';
    }
    return out;
}

// Função AES Encrypt mantida (PKCS#7 padding interno)
static std::vector<uint8_t> aesCbcEncrypt(const uint8_t* plaintext, size_t len) {
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_enc(&ctx, AES_KEY_128, 128) != 0) {
        logError(F("Erro setkey_enc"));
        mbedtls_aes_free(&ctx);
        return {};
    }

    size_t pad = 16 - (len % 16);
    size_t total = len + pad;
    std::vector<uint8_t> input(total);
    // Usando std::memcpy para compatibilidade C++
    std::memcpy(input.data(), plaintext, len); 
    for (size_t i = 0; i < pad; ++i) input[len + i] = (uint8_t)pad;

    std::vector<uint8_t> iv(16);
    std::memcpy(iv.data(), AES_IV_128, 16);

    std::vector<uint8_t> out(total);
    if (mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, total, iv.data(), input.data(), out.data()) != 0) {
        logError(F("Erro crypt_cbc (enc)"));
        mbedtls_aes_free(&ctx);
        return {};
    }
    mbedtls_aes_free(&ctx);
    return out;
}

// Função AES Decrypt mantida (Remoção de PKCS#7 padding interno)
static std::vector<uint8_t> aesCbcDecrypt(const uint8_t* ciphertext, size_t len) {
    if (len == 0 || len % 16 != 0) return {}; // Verifica o tamanho do bloco

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_dec(&ctx, AES_KEY_128, 128) != 0) { 
        logError(F("Erro setkey_dec"));
        mbedtls_aes_free(&ctx);
        return {};
    }
    
    std::vector<uint8_t> iv(16);
    std::memcpy(iv.data(), AES_IV_128, 16);
    std::vector<uint8_t> out(len);

    if (mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, len, iv.data(), ciphertext, out.data()) != 0) {
        logError(F("Erro crypt_cbc (dec)"));
        mbedtls_aes_free(&ctx);
        return {};
    }
    mbedtls_aes_free(&ctx);
    
    // Remover padding PKCS#7
    uint8_t pad = out.back();
    if (pad > 0 && pad <= 16 && pad <= out.size()) {
        out.resize(out.size() - pad);
    } else {
        logError(F("Padding inválido na decodificação."));
        return {}; 
    }
    return out;
}

// ===================== ARMAZENAMENTO OFFLINE (SPIFFS) =====================

static bool appendEncryptedOfflineLine(const String& jsonLine) {
    auto enc = aesCbcEncrypt((const uint8_t*)jsonLine.c_str(), jsonLine.length());
    if (enc.empty()) {
        logError(F("Erro na criptografia. Não salvo."));
        return false;
    }
    String b64 = base64Encode(enc.data(), enc.size());
    File f = SPIFFS.open(OFFLINE_FILE, FILE_APPEND);
    if (!f) { logError(F("Falha ao abrir SPIFFS para APPEND.")); return false; }
    f.println(b64);
    f.close();
    return true;
}

// Refatoração de Base64 Decode em uma função separada para clareza
static std::vector<uint8_t> b64ToBin(const String& s) {
    static const int8_t map[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    std::vector<uint8_t> out;
    int val = 0, valb = -8;
    for (size_t i = 0; i < s.length(); i++) {
        unsigned char c = s[i];
        if (c == '=') break;
        int8_t d = map[c];
        if (d == -1) continue;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back((uint8_t)((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

// Implementação da drenagem da fila offline (TRY DRAIN)
static bool tryDrainOfflineQueue(WiFiClientSecure& client) {
    if (!SPIFFS.exists(OFFLINE_FILE)) return true;
    File f = SPIFFS.open(OFFLINE_FILE, FILE_READ);
    if (!f) { logError(F("Falha ao abrir fila offline.")); return false; }

    String remaining;
    bool allSent = true;
    logInfo(F("Tentando drenar fila offline..."));

    while (f.available()) {
        String b64 = f.readStringUntil('\n');
        b64.trim();
        if (b64.isEmpty()) continue;

        std::vector<uint8_t> enc = b64ToBin(b64);
        if (enc.empty() || enc.size() % 16 != 0) { 
            logError(F("Linha offline (B64/AES) inválida, descartando."));
            allSent = false; 
            continue; 
        }
        
        auto plain = aesCbcDecrypt(enc.data(), enc.size());
        if (plain.empty()) {
            logError(F("Falha na descriptografia da linha offline, descartando."));
            allSent = false;
            continue;
        }

        String jsonLine = String((const char*)plain.data());
        jsonLine.trim();

        // Envia linha ao servidor
        if (!WiFi.isConnected()) { 
            allSent = false; 
            remaining += b64 + "\n"; 
            continue; 
        }

        bool ok = false;
        {
            if (!client.connected()) {
                if (!client.connect(SERVER_ADDRESS, SERVER_PORT)) {
                    logError(F("Falha ao conectar servidor para drenagem."));
                    allSent = false;
                    remaining += b64 + "\n"; // Salva para a próxima tentativa
                    continue;
                }
            }
            // HTTP POST Request
            String req;
            req += F("POST ");
            req += SERVER_ATTENDANCE_PATH;
            req += F(" HTTP/1.1\r\nHost: ");
            req += SERVER_ADDRESS;
            req += F("\r\nUser-Agent: ESP32-CAM\r\nConnection: keep-alive\r\nAccept: application/json\r\nContent-Type: application/json\r\nAuthorization: Bearer ");
            req += AUTH_BEARER_TOKEN;
            req += F("\r\nContent-Length: ");
            req += String(jsonLine.length());
            req += F("\r\n\r\n");
            req += jsonLine;

            client.print(req);

            uint32_t t0 = millis();
            // Espera pela resposta com timeout
            while (client.available() == 0 && millis() - t0 < 2000) delay(10); 
            if (client.available()) {
                String status = client.readStringUntil('\n'); 
                // Considera sucesso se o status for 2xx
                ok = status.indexOf(F("200")) > 0 || status.indexOf(F("201")) > 0 || status.indexOf(F("204")) > 0;
            }
        }
        if (!ok) {
            logError(F("Linha offline não enviada, re-adicionando."));
            allSent = false;
            remaining += b64 + "\n"; // Salva para a próxima tentativa
        }
    }
    f.close();

    // Reescreve arquivo com pendências (ou o remove se estiver vazio)
    if (remaining.isEmpty()) {
        SPIFFS.remove(OFFLINE_FILE);
    } else {
        File fw = SPIFFS.open(OFFLINE_FILE, FILE_WRITE);
        if (!fw) return false;
        fw.print(remaining);
        fw.close();
    }
    
    if (allSent) logInfo(F("Fila offline drenada com sucesso."));
    return allSent;
}

// ===================== BLE (ATIVAÇÃO DO PROFESSOR) =====================
#if ENABLE_BLE
static NimBLEServer* g_bleServer = nullptr;
static NimBLECharacteristic* g_bleCtrlChar = nullptr;

class CtrCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        std::string v = c->getValue();
        if (v == "OK" || v == "VALID") {
            g_bleValidationRequired = false; 
            g_bleLastSeenMillis = millis(); 
            logInfo(F("BLE: Validação do professor recebida. Chamada ATIVA."));
            g_bleCtrlChar->setValue(F("ACTIVE"));
            g_bleCtrlChar->notify();
        } else if (v == "STOP") {
             g_bleValidationRequired = true;
             logInfo(F("BLE: Comando STOP recebido. Chamada INATIVA."));
             g_bleCtrlChar->setValue(F("STOPPED"));
             g_bleCtrlChar->notify();
        }
    }
};

static void initBLE() {
    NimBLEDevice::init("ESP32-CAM-CHAMADA"); 
    NimBLEDevice::setPower(ESP_PWR_LVL_P3); 
    g_bleServer = NimBLEDevice::createServer();

    // Serviço e Característica UUIDs Customizados
    NimBLEService* svc = g_bleServer->createService("7e570001-0002-0003-0004-000000000001");
    g_bleCtrlChar = svc->createCharacteristic("7e570001-0002-0003-0004-000000000002", NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    g_bleCtrlChar->setCallbacks(new CtrCallbacks());
    g_bleCtrlChar->setValue("READY"); 
    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(svc->getUUID());
    adv->start();
    logInfo(F("BLE iniciado. Aguardando ativação do professor."));
}
#endif

// ===================== CAPTURA E ENVIO =====================

static bool captureJpeg(std::unique_ptr<camera_fb_t, void(*)(camera_fb_t*)>& fbPtr) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { 
        logError(F("esp_camera_fb_get falhou."));
        return false; 
    }
    if (fb->format != PIXFORMAT_JPEG) {
        logError(F("Formato inesperado."));
        esp_camera_fb_return(fb);
        return false;
    }
    // O unique_ptr assume a responsabilidade de liberar o buffer (fb) no final do escopo
    fbPtr.reset(fb); 
    return true;
}

static String makeAttendanceJson(const String& imageBase64, const String& timestampIso, const String& roomId, const String& courseId, const String& mode, const String& identity) {
    String j = F("{");
    j += F("\"ts\":\"") + timestampIso + F("\",");
    j += F("\"room\":\"") + roomId + F("\",");
    j += F("\"course\":\"") + courseId + F("\",");
    j += F("\"mode\":\"") + mode + F("\","); 
    j += F("\"identity\":\"") + identity + F("\","); 
    j += F("\"image_b64\":\"") + imageBase64 + F("\"");
    j += F("}");
    return j;
}

static bool postAttendance(WiFiClientSecure& client, const String& jsonPayload) {
    if (!WiFi.isConnected()) return false;

    // Conexão TLS
    if (!client.connected()) {
        if (SERVER_PORT == 443 && TLS_INSECURE == 0) {
            // Se estiver usando HTTPS padrão (443) e modo seguro, defina o CA
            client.setCACert(SERVER_CA_CERT);
        }
        
        if (!client.connect(SERVER_ADDRESS, SERVER_PORT)) {
            logError(F("Falha ao conectar ao servidor."));
            return false;
        }
    }

    String req;
    req += F("POST ");
    req += SERVER_ATTENDANCE_PATH;
    req += F(" HTTP/1.1\r\nHost: ");
    req += SERVER_ADDRESS;
    req += F("\r\nUser-Agent: ESP32-CAM\r\nConnection: keep-alive\r\nAccept: application/json\r\nContent-Type: application/json\r\nAuthorization: Bearer ");
    req += AUTH_BEARER_TOKEN;
    req += F("\r\nContent-Length: ");
    req += String(jsonPayload.length());
    req += F("\r\n\r\n");
    req += jsonPayload;

    client.print(req);

    uint32_t t0 = millis();
    while (client.available() == 0 && millis() - t0 < 2000) delay(10);
    
    if (client.available()) {
        String status = client.readStringUntil('\n'); 
        if (status.indexOf(F("200")) > 0 || status.indexOf(F("201")) > 0 || status.indexOf(F("204")) > 0) {
            return true;
        } else {
            // Loga o erro de status HTTP para diagnóstico
            Serial.print(F("[ERRO] Status HTTP: "));
            Serial.println(status);
            return false;
        }
    }
    logError(F("Timeout na resposta do servidor."));
    return false;
}

// ===================== SETUP E LOOP (Funções padrão Arduino) =====================
static uint32_t lastSyncAttemptMs = 0;
static const uint32_t SYNC_INTERVAL_MS = 10000;

void setup() {
    Serial.begin(115200);
    delay(500);

    logInfo(F("Inicializando SPIFFS (Sistema de Arquivos)..."));
    if (!SPIFFS.begin(true)) {
        logError(F("Falha ao montar SPIFFS. Dados offline não serão armazenados."));
    }

    connectWiFi();

    if (!timeIsSet()) {
        logInfo(F("Aguardando sincronização NTP (máx 10s)..."));
        for (int i = 0; i < 20 && !timeIsSet(); i++) {
            delay(500);
        }
    }

    if (!initCamera()) {
        logError(F("Camera indisponível. Reinicie o dispositivo."));
        delay(5000);
        ESP.restart();
    }

#if ENABLE_BLE
    initBLE();
#endif

    logInfo(F("Setup concluído. Loop de captura em execução."));
}

void loop() {
    // 1. Manutenção do Wi-Fi
    if (!WiFi.isConnected()) {
        connectWiFi();
    }

    // 2. Cliente de Comunicação (TLS)
    static WiFiClientSecure client;
#if TLS_INSECURE
    client.setInsecure(); 
#endif

    // 3. Drenagem da fila offline
    // Tenta drenar a fila apenas se houver Wi-Fi e tempo ajustado (para logs e timestamps corretos)
    if (WiFi.isConnected() && timeIsSet() && millis() - lastSyncAttemptMs > SYNC_INTERVAL_MS) {
        lastSyncAttemptMs = millis();
        tryDrainOfflineQueue(client);
    }

    // 4. Checagem de Ativação do Professor (BLE)
#if ENABLE_BLE
    if (g_bleValidationRequired) {
        logInfo(F("Aguardando ativação do professor via BLE..."));
        g_bleCtrlChar->setValue(F("WAITING")); // Atualiza status BLE
        g_bleCtrlChar->notify();
        delay(1000);
        return; 
    }
    
    if (millis() - g_bleLastSeenMillis > BLE_VALID_WINDOW_MS) {
        g_bleValidationRequired = true;
        logInfo(F("Janela de chamada expirou. Aguardando nova ativação do professor via BLE."));
        g_bleCtrlChar->setValue(F("EXPIRED")); // Atualiza status BLE
        g_bleCtrlChar->notify();
        delay(1000);
        return; 
    }
#endif

    // 5. Captura da Imagem
    // Função customizada para gerenciamento seguro de memória com esp_camera_fb_return
    std::unique_ptr<camera_fb_t, void(*)(camera_fb_t*)> fb(nullptr, [](camera_fb_t* p){ if (p) esp_camera_fb_return(p); });
    if (!captureJpeg(fb)) {
        logError(F("Falha na captura."));
        delay(500);
        return;
    }

    // 6. Codificação e Montagem do Payload
    String imgB64 = base64Encode(fb->buf, fb->len);
    // Usa o timestamp real se estiver ajustado, caso contrário usa o fallback
    String ts = timeIsSet() ? iso8601Now() : String(F("1970-01-01T00:00:00Z")); 
    
    // O modo de envio é determinado pela conexão Wi-Fi
    String mode = WiFi.isConnected() ? F("realtime") : F("offline");
    
    String payload = makeAttendanceJson(imgB64, ts, SCHOOL_ROOM_ID, COURSE_ID, mode, F(""));

    // 7. Envio ou Armazenamento
    bool ok = false;
    uint32_t tStart = millis();

    if (WiFi.isConnected()) {
        ok = postAttendance(client, payload);
    }
    
    if (ok) {
        logInfo(F("Ponto enviado com sucesso."));
    } else {
        if (appendEncryptedOfflineLine(payload)) {
            logInfo(F("Falha no envio. Ponto salvo OFFLINE."));
        } else {
            logError(F("Falha no envio e no salvamento offline."));
        }
    }

    uint32_t elapsed = millis() - tStart;

    // 8. Controle de Tempo (Garante uma pausa mínima entre capturas)
    // Se o processamento for rápido, espera mais. Mínimo de 1.5s entre capturas.
    uint32_t rest = (elapsed < 1500) ? (1500 - elapsed) : (100); 
    delay(rest);
}