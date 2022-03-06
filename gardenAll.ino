#include <EasyPCF8574.h>
#include <WiFi.h>
#include <SPI.h>
#include <LoRa.h>
#include <time.h>

WiFiServer *sockServ;

struct tntp {
    const char *ntpServer        = "pool.ntp.org";
    const long gmtOffset_sec     = 0;
    const int daylightOffset_sec = -3600 * 3;

} ntp_server;

//estados
struct sm {
    bool sm_lora_is_on           = false;
    bool sm_water                = false;
    bool sm_ap_mode              = true;
    bool sm_sta_mode             = false;
    bool sm_is_on                = false;
    bool sm_turn_water_on        = false;

    const char* sm_water_stat[2] = {"ON","OFF"};
    
    uint8_t ap_rf_mqtt_ntp_sock[5]      = {0};
    uint8_t sm_time_to_water            = 0;
    uint8_t sm_turnOn                   = 0;
    
} state_machine;

enum {
    pos_ap, pos_rf, pos_mqtt, pos_ntp, pos_sock
};

struct lora {
    byte msgCount = 0;            // count of outgoing messages
    byte syncw    = 0xBB;
    int interval = 2000;          // interval between sends
    long lastSendTime = 0;
} sx1276rf;

// Replace with your network credentials
const char* ssid          = "Garden";
const char* password      = "djamessuhanko";
const char* sta_ssid      = "SuhankoFamily";
const char* sta_pwd       = "fsjmr112";

bool sta_is_connected     = false; 

long int water_time_secs     = 90*1000; //segundos ligados
bool water_flowing           = false;
long int water_started_at    = 0;
uint8_t last_water_execution = 0;

char *rf_msg_arrived;

EasyPCF8574 pcf_A(0x27,255); //pcf addr, initial value

void taskSocket(void *pvParameters){
    while (true){
        String payload_from_socket = "";
        WiFiClient client2 = sockServ->available();
        if (client2) {
            while (client2.connected()) {
                while (client2.available()>0) {
                    char c = client2.read();
                    //client.write(c);
                    payload_from_socket += c;
                }
                //delay(10);
            }
            client2.stop();
            Serial.println("Fim da conexao");
            state_machine.ap_rf_mqtt_ntp_sock[pos_sock] = payload_from_socket.indexOf("^1$") < 0 ? 0 : 1;
            Serial.printf("Payload from socket: %s",payload_from_socket);
        }   
    }
}

uint8_t getHourFromNTPserver(){
    struct tm timeinfo;
    
    if (!getLocalTime(&timeinfo)) {
        Serial.println("no NTP for now");
        return 0;
    }
    
    Serial.println(timeinfo.tm_hour);
    // Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
    //TODO: definir a hora prioritária por MQTT
    
    if ((timeinfo.tm_hour-3 == 8 || timeinfo.tm_hour-3 == 20) && last_water_execution != timeinfo.tm_hour-3){   
        water_flowing        = true;
        water_started_at     = millis();
        last_water_execution = timeinfo.tm_hour-3;
        return 1;
    }
    
    if (water_flowing){
        if ((millis()-water_started_at) > water_time_secs){
            water_flowing = false;
            return 0;
        }
        return 1;
    }
    return 0;
    
}

uint8_t onReceive(int packetSize) {
    if (packetSize == 0) return 0;          // if there's no packet, return

    // read packet header bytes:
    String incoming = "";
 
    while (LoRa.available()) {
        char pipe = (char)LoRa.read();
        //TODO: incrementar se atender o formato da mensagem.
        incoming += pipe;
    }
 
    //TODO: remover esses prints após constatar o handshake
    Serial.println("Message: " + incoming);
    //Serial.println("RSSI: " + String(LoRa.packetRssi()));
    //Serial.println("Snr: " + String(LoRa.packetSnr()));
    //Serial.println();
    
    //Sem susto. Se estiver ativo por AP, só AP pode parar a rega. TODO: condicionar também MQTT
    //state_machine.sm_time_to_water = WiFi.softAPgetStationNum() > 0 ? 1 : incoming[1]-48;
    
    return incoming[1]-48;
}

void setup() {
    configTime(ntp_server.gmtOffset_sec, ntp_server.daylightOffset_sec, ntp_server.ntpServer);
    Serial.begin(115200);
    WiFi.mode(WIFI_MODE_APSTA);
    WiFi.softAP(ssid, password);
    WiFi.begin(sta_ssid,sta_pwd); //mesmo se o modo STA falhar, pode seguir com os demais modos
    
    /*Aguarda que a conexão seja completada, por 10 segundos. Se não conectar, segue adiante*/
    for (uint8_t i=0; i<20;i++){
        if (WiFi.status() == WL_CONNECTED){
            state_machine.sm_sta_mode = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    } 
    
    LoRa.setPins(32, 18, 2);
    
    //A única interface mandatória é a conexão AP. O resto é desejável, mas funciona sem.
    for (uint8_t i=0;i<20;i++){
        if (LoRa.begin(915E6)){
            state_machine.sm_lora_is_on = true;
            Serial.printf("\nRadio is on!\n");
            break;
        }
        delay(500);
    }
    
    if (state_machine.sm_lora_is_on){
        // Sync word needs to match with the receiver
        // The sync word assures you don't get LoRa messages from other LoRa transceivers
        // ranges from 0-0xFF
        LoRa.setSyncWord(sx1276rf.syncw);
    }
    else{
        Serial.println("LoRa its not initialized :(");
        delay(3000);
    }
    
    //Se não inicializar o endereço, não adianta seguir porque não haverá controle do relé.
    if (!pcf_A.startI2C(21,22)){
        Serial.println("Not started. Check pin and address.");
        while (true);
    }
    
    Serial.printf("\n\nAP IP: ");
    Serial.println(WiFi.softAPIP());
    
    Serial.print("STA IP: ");
    Serial.println(WiFi.localIP());
    
    sockServ = new WiFiServer(123);
    sockServ->begin();
    
    xTaskCreatePinnedToCore(taskSocket,"taskSocket",10000,NULL,0,NULL,0);
}

void loop() {
    
    //---------------- check AP mode ------------------------------
    state_machine.sm_time_to_water = WiFi.softAPgetStationNum();
    
    //state_machine.sm_turn_water_on = true;
    state_machine.ap_rf_mqtt_ntp_sock[pos_ap] = state_machine.sm_time_to_water == 1 ? 1 : 0;

    //--------------------------------------------------------------
    
    //TODO: Na mudança pode não ser mais útil. Validar:
    //------------------- check LoRa mode --------------------------
    state_machine.ap_rf_mqtt_ntp_sock[pos_rf] = onReceive(LoRa.parsePacket());
    
    //--------------------- check NTP mode -------------------------
    state_machine.ap_rf_mqtt_ntp_sock[pos_ntp] = getHourFromNTPserver();
    
    //TODO: implementar MQTT antes da linha a seguir
    state_machine.sm_turnOn = 0;
    for (uint8_t i=0;i<sizeof(state_machine.ap_rf_mqtt_ntp_sock);i++){
        state_machine.sm_turnOn += state_machine.ap_rf_mqtt_ntp_sock[i];
        if (state_machine.ap_rf_mqtt_ntp_sock[i] == 1){
            Serial.printf("Bit up: %d\n", i);
        }
    }
    
    //Agora verifica se houve requisição por AP, RF ou MQTT. Atua conforme o estado:
    if (state_machine.sm_turnOn > 0){
        state_machine.sm_turn_water_on = true;
    }
    else{
        state_machine.sm_turn_water_on = false;
    }
    /* Poderia ser resolvido diretamente acima, mas se houver adição à condicional,
    /talvez seja melhor fazê-lo modificando abaixo. Por exemplo, é para ligar, mas
    deve-se avaliar umidade do ar ou do solo:
    if (state_machine.sm_turn_water_on && gnd_humidity > 50){...}
    */
    if (state_machine.sm_turn_water_on){
        pcf_A.setDownBit(0);
        Serial.printf("Ligar: %d\n",state_machine.sm_turn_water_on);
    }
    else{
        pcf_A.setUpBit(0);
        state_machine.sm_turnOn = 0;
        Serial.printf("Desligar: %d\n",state_machine.sm_turn_water_on);
    }
    
    Serial.printf("ACUMULADOR: %d\n",state_machine.sm_turnOn);
    
    Serial.printf("A agua esta %s\n", state_machine.sm_water_stat[!state_machine.sm_turn_water_on]);
    vTaskDelay(pdMS_TO_TICKS(1000));
}
