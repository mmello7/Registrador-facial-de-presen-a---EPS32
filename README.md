# 📚 Sistema de Reconhecimento Facial para Registro de Presença (ESP32-CAM)

Este projeto implementa um sistema de **registro automático de presença** utilizando o módulo **ESP32-CAM**, capaz de capturar imagens, reconhecer rostos e enviar informações de presença para um servidor.  
O objetivo é tornar o processo de chamada em sala de aula mais rápido, automático e sem intervenção manual.

---

## 🎯 Objetivo

Automatizar o registro de presença de alunos por **reconhecimento facial**, utilizando hardware de baixo custo e comunicação via Wi-Fi.

---

## 🧠 Como Funciona

1. A câmera do **ESP32-CAM** captura uma imagem.
2. O sistema converte a imagem para **Base64**.
3. Os dados são enviados para um **servidor** (quando online) ou armazenados **offline criptografados**.
4. Quando houver internet, os registros offline são enviados automaticamente.
5. (Opcional) Um dispositivo BLE pode autorizar a validação de presença.

---

## 🏗️ Hardware Utilizado

| Componente | Função |
|-----------|--------|
| **ESP32-CAM** | Captura de imagem e reconhecimento |
| **ESP32-CAM-MB (programador)** | Facilita a gravação do código via USB |
| Protoboard (opcional) | Suporte para testes |
| Cabos jumper | Conexões |

> **Atenção:** Não é necessário Arduino Uno. A gravação é feita diretamente pelo ESP32-CAM-MB via USB.

---

## 🔧 Software e Ferramentas

| Ferramenta | Uso |
|-----------|-----|
| **VSCode + PlatformIO** | Ambiente de desenvolvimento |
| Biblioteca `esp_camera` | Controle da OV2640 |
| `WiFiClientSecure` | Comunicação HTTPS |
| `SPIFFS` | Armazenamento local offline |
| `mbedtls` | Criptografia AES-128 |

---

## ⚙️ Configurações Importantes

No código, altere:

```cpp
static const char* WIFI_SSID = "SUA_REDE";
static const char* WIFI_PASS = "SUA_SENHA";
static const char* SERVER_HOST = "http://IP_DO_SEU_SERVIDOR";
