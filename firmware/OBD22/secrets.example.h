// Copie este arquivo para "secrets.h" (na mesma pasta do OBD22.ino) e preencha.
// O arquivo secrets.h NAO deve ser enviado ao GitHub (ja esta no .gitignore).
#pragma once
#define WIFI_SSID   "NOME_DA_SUA_REDE"
#define WIFI_SENHA  "SENHA_DA_SUA_REDE"
#define API_URL     "http://IP_DO_COMPUTADOR:3000/api/amostras"
#define API_KEY     "a-mesma-chave-do-arquivo-.env"
#define ELM327_MAC  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}   // MAC do adaptador ELM327
