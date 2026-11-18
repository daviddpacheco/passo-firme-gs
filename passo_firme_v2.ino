#include "BluetoothSerial.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth nao esta habilitado! Verifique as configuracoes da placa no IDE.
#endif

BluetoothSerial ESP_BT;

// Teste de diagnostico: se o robo travar com 1 e nao travar com 0 (ou o contrario),
// o problema e a alimentacao. 1 = brownout desativado (como no seu codigo original).
#define DESATIVAR_BROWNOUT 1

// -------- MAPEAMENTO DE PINOS DO ESP32 --------
// Ponte H (L298N)
const int IN1 = 13;
const int IN2 = 12;
const int IN3 = 14;
const int IN4 = 27;

// Sensor Ultrassonico (HC-SR04)
const int TRIG = 5;
const int ECHO = 18;

// Sensor de Luminosidade (LDR sem resistor externo: Pino 32 + GND)
const int LDR_PIN = 32;

// -------- PARAMETROS DE CALIBRACAO --------
const int DISTANCIA_MINIMA = 25;    // cm
const int LIMITE_ESCURIDAO = 1400;  // > 1400 = escuro / risco de queda

// -------- VARIAVEIS DE ESTADO --------
char ultimoComando = 'S';
long distancia = -1;
int luminosidade = 0;
int leiturasPerto = 0;
unsigned long ultimaLeitura = 0;
unsigned long ultimoEnvioIA = 0;

// Flags alteradas pelo callback do Bluetooth (tarefa diferente do loop)
volatile bool clienteConectado = false;
volatile bool desconectouAgora = false;

// -------- CALLBACK DO BLUETOOTH --------
void eventoBT(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
  if (event == ESP_SPP_SRV_OPEN_EVT) {
    clienteConectado = true;
  } else if (event == ESP_SPP_CLOSE_EVT) {
    clienteConectado = false;
    desconectouAgora = true;
  }
}

// -------- FUNCOES DE MOVIMENTO --------
void forward() {
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
}

void backward() {
  digitalWrite(IN1, LOW); digitalWrite(IN2, HIGH);
  digitalWrite(IN3, LOW); digitalWrite(IN4, HIGH);
}

void turnLeft() {
  digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
}

void turnRight() {
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);
}

void stopCar() {
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
}

void mover(char direcao) {
  switch (direcao) {
    case 'F': case 'A': forward();   break;
    case 'B': case 'C': backward();  break;
    case 'L': case 'E': turnLeft();  break;
    case 'R': case 'D': turnRight(); break;
    case 'S': case 'G': stopCar();   break;
    default:            stopCar();   break;
  }
}

// -------- ENVIO SEGURO --------
// So escreve no Bluetooth se houver cliente conectado.
// Escrever sem cliente (ou com o buffer cheio) pode bloquear o loop e "travar" os comandos.
void enviar(const char *msg) {
  Serial.println(msg);
  if (clienteConectado) {
    ESP_BT.println(msg);
  }
}

// -------- LEITURA DO SENSOR ULTRASSONICO --------
long lerDistanciaCm() {
  digitalWrite(TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG, LOW);

  long duracao = pulseIn(ECHO, HIGH, 15000);  // timeout menor = menos tempo bloqueado
  if (duracao == 0) return 200;
  return duracao * 0.034 / 2;
}

// -------- LEITURA DO LDR (media de 4 amostras para estabilizar) --------
int lerLuminosidade() {
  pinMode(LDR_PIN, INPUT_PULLUP);
  delayMicroseconds(50);
  long soma = 0;
  for (int i = 0; i < 4; i++) {
    soma += analogRead(LDR_PIN);
  }
  return soma / 4;
}

// -------- CONFIGURACAO INICIAL --------
void setup() {
#if DESATIVAR_BROWNOUT
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
#endif
  Serial.begin(115200);

  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
  pinMode(TRIG, OUTPUT); pinMode(ECHO, INPUT);
  pinMode(LDR_PIN, INPUT_PULLUP);

  stopCar();

  ESP_BT.register_callback(eventoBT);  // registrar ANTES do begin()
  ESP_BT.begin("PassoFirme_Bot");
  Serial.println("Passo Firme v2 iniciado! Emparelhe com 'PassoFirme_Bot'.");
}

// -------- LOOP PRINCIPAL --------
void loop() {
  // 0. Failsafe: se o Bluetooth desconectar, o carrinho para
  if (desconectouAgora) {
    desconectouAgora = false;
    stopCar();
    ultimoComando = 'S';
    Serial.println("Bluetooth desconectado: carrinho parado por seguranca.");
  }

  // 1. Le TODOS os bytes pendentes (antes lia 1 por volta e o buffer enchia)
  while (ESP_BT.available() > 0) {
    char comando = toupper(ESP_BT.read());
    if (comando == '\n' || comando == '\r' || comando == ' ') continue;
    ultimoComando = comando;
    mover(ultimoComando);
  }

  // 2. Sensores e seguranca ativa (a cada 100 ms)
  if (millis() - ultimaLeitura >= 100) {
    ultimaLeitura = millis();
    distancia = lerDistanciaCm();
    luminosidade = lerLuminosidade();

    if (distancia > 0 && distancia < DISTANCIA_MINIMA) {
      leiturasPerto++;
    } else {
      leiturasPerto = 0;
    }

    if ((ultimoComando == 'F' || ultimoComando == 'A') && leiturasPerto >= 2) {
      stopCar();
      ultimoComando = 'S';
      enviar("{\"alerta\":\"Barreira fisica detectada! Paragem de emergencia.\"}");
    }
  }

  // 3. Telemetria JSON (a cada 1 s) com buffer fixo, sem String dinamica
  if (millis() - ultimoEnvioIA >= 1000) {
    ultimoEnvioIA = millis();

    const char *statusLuz = (luminosidade > LIMITE_ESCURIDAO) ? "BAIXA_ALTO_RISCO_QUEDA" : "ADEQUADA_SEGURO";
    const char *statusVia = (distancia > 0 && distancia < DISTANCIA_MINIMA) ? "OBSTACULO_PROXIMO" : "LIVRE";

    char payload[200];
    snprintf(payload, sizeof(payload),
             "{\"distancia_cm\":%ld,\"luminosidade\":%d,\"status_luz\":\"%s\",\"status_via\":\"%s\",\"heap\":%lu,\"up_s\":%lu}",
             distancia, luminosidade, statusLuz, statusVia,
             (unsigned long)ESP.getFreeHeap(), millis() / 1000UL);

    enviar(payload);
  }

  delay(2);  // cede tempo para a pilha Bluetooth
}
