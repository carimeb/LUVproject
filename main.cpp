#include <Arduino.h>                                    //Default Arduino lib, using AXTLS interface for HTTPS
#include <ESP8266WiFi.h>                                //Wifi connection library
#include <ESP8266mDNS.h>                                //DNS library (to device get a name in the router)
#include <ESP8266HTTPClient.h>                          //Communicate to Telegram and ThingSpeak    
#include <WiFiClientSecure.h>                           //SSL usage (HTTPS)
#include <WiFiUdp.h>                                    //UDP messages library (to use NTP client)
#include <Wire.h>                                       //I2C protocol library
#include <Adafruit_VEML6070.h>                          //UV light sensor
#include <NTPClient.h>                                  //Get timezones in the internet
#include <ArduinoJson.h>                                //To create and manipulate json files and Telegram lib
#include <LittleFS.h>                                   //To control board memory
#include <UniversalTelegramBot.h>                       //Telegram client library

#define DEBUG true                                      //Flag to debug via serial communication, when the board is connected to notebook

#define SERIAL_VELOCITY 115200                          //Set serial communication velocity between board and notebook, for debug
#define BOTtoken ""                                     //Secret Telegram bot ID, the bot to be manipulate
#define INTERVAL 1000                                   //Message checking interval (in milliseconds)
#define MAXLEN 100                                      //Max num of characters in a message
#define MAXSUB 10                                       //Max num of subscribers
#define UVWARNING 30                                    //UV index limit to send an warning

String WELCOME = "Welcome to <b>UV_bot</b>, send <b>/help</b> to help";
String AVAILABLE_CMD = "<b>UV</b>: Get UV index\n<b>SUBSCRIBE</b>: Subscribe to notification of high UV warning index\n<b>UNSUBSCRIBE</b>: Unsubscribe to notification of high UV warning";
String SUN = "\xE2\x98\x80";                            //Sun emoji

const size_t json_size = 512;

const char* ssid = "";                                  //Put your wifi network name here
const char* password = "";                              //Put your wifi network password here

char addr_api_thingspeak[] = "api.thingspeak.com";
String thingspeakKey = "";                              //Put your ThingSpeak key here
unsigned long last_connection_time;

String subscribed[MAXSUB];

const String FILENAME = "/config.json";

/*Default Telegram struct used in processing messages*/
typedef struct msg_ {
  char buffer[20];
  int size;
} msg;

/*Object instantiations*/
X509List cert(TELEGRAM_CERTIFICATE_ROOT);               //SSL certificate to identify my bot
WiFiClientSecure client_bot;                            //HTTPS connection client
UniversalTelegramBot bot(BOTtoken, client_bot);         //Telegram bot interface
Adafruit_VEML6070 uv = Adafruit_VEML6070();             //UV sensor
WiFiUDP ntpUDP;                                         //NTP server UDP connection
NTPClient timeClient(ntpUDP, "a.st1.ntp.br", -3 * 3600);//NTP time set to Brazil timezone
WiFiClient client;                                      //Secure wi-fi communication client  

unsigned long dt;
msg message;

int lastToday = -1;
int last_hour_get = -1;

/*Communication to send data to ThingSpeak database using HTTP API*/
void write_thingspeak(int uvIndex) {
    if (client.connect(addr_api_thingspeak, 80)) {
        char buffer[100] = {0};
        sprintf(buffer,"field1=%d", uvIndex);
        client.print("POST /update HTTP/1.1\n");
        client.print("Host: api.thingspeak.com\n");
        client.print("Connection: close\n");
        client.print("X-THINGSPEAKAPIKEY: "+thingspeakKey+"\n");
        client.print("Content-Type: application/x-www-form-urlencoded\n");
        client.print("Content-Length: ");
        client.print(strlen(buffer));
        client.print("\n\n");
        client.print(buffer);
        last_connection_time = millis();                 //Time control, to know when send the next info to database 
        Serial.println("- Infos sent to ThingSpeak!");
    }
}


/*Function to try WiFI connection*/
void connect() {
  if(DEBUG) Serial.println("Try to connect in WiFi network...");
  while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      if(DEBUG) Serial.print(".");
  }
}

/*Get UTC time via NTP server*/
void startTimeNclients() {
  timeClient.begin();
  configTime(0, 0, "pool.ntp.org");
  time_t now = time(nullptr);
  Serial.println(" - Sync...");
  while (now < 24 * 3600) {
    if(DEBUG) Serial.print(".");
    delay(100);
    now = time(nullptr);
  }
  timeClient.update();
}

/*Create a configuration file in the board memory, with subscribed users data and when the warnings will be/were sent*/
void createFile() {
  File rFile = LittleFS.open(FILENAME, "w");
  if (!rFile) {
    if(DEBUG) Serial.println("Error opening file for writing");
    return;
  }
  if(DEBUG) Serial.println(" - Save File");
  DynamicJsonDocument root(json_size);
  root["today"] = lastToday;                             //Keep the number of the day of the week the last message was sent - avoid flooding
  JsonArray arr = root.createNestedArray("users");
  for(int i = 0; i < MAXSUB; i++) arr.add(subscribed[i]); //Limit the number of subscribers
  serializeJson(root,rFile);
  if(DEBUG) serializeJson(root,Serial);
  rFile.close();
}

/*Read the .json memory file*/
void readFile() {
  if(DEBUG) Serial.print(" - Read File");
  for(int i = 0; i < MAXSUB; i++) subscribed[i] = "";
  DynamicJsonDocument obj(json_size);
  if(DEBUG)Serial.println(LittleFS.exists(FILENAME));
  if(LittleFS.exists(FILENAME)){
    File rFile = LittleFS.open(FILENAME, "r+");
    if(DEBUG) Serial.println(" - Open File");
    deserializeJson(obj,rFile);
    lastToday = obj["today"];
    for(int i = 0; i < MAXSUB; i++) {
      String u = obj["users"][i];
      subscribed[i] = u;
    }
    rFile.close();
  }
  else createFile();
}

/*Routine to add subscribers*/
void subscribe(String chatID) {
  for(int i = 0; i < MAXSUB; i++) {
    if(subscribed[i] == chatID) {
      bot.sendMessage(chatID, "You are already subscribed.", "HTML");
      return;
    }
  }
  for(int i = 0; i < MAXSUB; i++) {
    if(subscribed[i] == "") {
      subscribed[i] = chatID;
      createFile();
      bot.sendMessage(chatID, "You are subscribed to warnings.", "HTML");
      return;
    }
  }
  bot.sendMessage(chatID, "Sorry, the system has already reached the limit of subscribers.", "HTML");
}

/*Routine to remove a subscribers*/
void unsubscribe(String chatID) {
  for(int i = 0; i < MAXSUB; i++) {
    if(subscribed[i] == chatID) {
      subscribed[i] = "";
      createFile();
      bot.sendMessage(chatID, "You are unsubscribed.", "HTML");
      return;
    }
  }
  bot.sendMessage(chatID, "you are unsubscribed already.", "HTML");
}


/*Function to read the UV sensor*/
int read_UV_sensor() {
  int UV_index = uv.readUV();
  if(DEBUG) Serial.print("UV light level: ");
  if(DEBUG) Serial.println(UV_index);
  return UV_index;
}

/*Check the UV index and eventually send a warning*/
void sendWarning() {
  timeClient.update();
  int uvIndex = read_UV_sensor();
  if(DEBUG) Serial.println(String(lastToday) + " " + String(timeClient.getDay()) + " " + String(uvIndex));

  /*Send data to ThingSpeak hourly*/
  if(timeClient.getHours() != last_hour_get) {
    last_hour_get = timeClient.getHours();
    write_thingspeak(uvIndex);
  }
  /*If the UV index reaches a high level, it sends a warning once a day*/
  if(timeClient.getDay() != lastToday && uvIndex >= UVWARNING) {
    lastToday = timeClient.getDay();
    if(DEBUG) Serial.println("Send Warning");
    createFile();
    for(int i = 0; i < MAXSUB; i++) {
      if(subscribed[i].length() > 0) bot.sendMessage(subscribed[i], "UV IS TOO HIGH TODAY!", "HTML");
    }
    write_thingspeak(uvIndex);                   //Special bulletin to ThingSpeak database, independently of the hour
  }
  /*Correction for cycling count of weekend days*/
  if((timeClient.getDay()+1)%6 == lastToday) lastToday = timeClient.getDay();
}

/*Function to interpret message content*/
void handler(String chat_id, String name, String user_id, String msg) {
  bot.sendChatAction(chat_id, "typing");         //Show "typing" in Telegram chat
  msg.toUpperCase();                             //Set message to case insensitive
  if (msg.indexOf("UV") > -1) {
    String m = SUN + "<b>UV index</b>: " + String(read_UV_sensor()) + "\n";
    Serial.println(m);
    bot.sendMessage(chat_id, m, "HTML");
  }
  else if (msg == "SUBSCRIBE") subscribe(chat_id);
  else if (msg == "UNSUBSCRIBE") unsubscribe(chat_id);
  else if (msg == "/START") bot.sendMessage(chat_id, WELCOME, "HTML");
  else if (msg == "/HELP") bot.sendMessage(chat_id, "<b>Commands Available:</b>\n" + AVAILABLE_CMD, "HTML");
  else bot.sendMessage(chat_id, "<b>Command not Available :(</b>\n", "HTML");
}

/*Function to handle Telegram API requests*/
void readTel() {
   if(DEBUG) Serial.println("Getting update");
   int newmsg = bot.getUpdates(bot.last_message_received + 1);
   if(DEBUG) Serial.println("Done");

   for (int i = 0; i < newmsg; i++) {                            //Read all queued messages
      String chat_id = bot.messages[i].chat_id;                  //Get the chat ID from message was sent
      String sender_id = bot.messages[i].from_id;                //Get user ID who send the message
      String name = bot.messages[i].from_name;                   //Get user Telegram name
      if(DEBUG) Serial.println("We got a message");
      if(bot.messages[i].text.length() < MAXLEN){
        String msg = bot.messages[i].text;
        handler(chat_id, name, sender_id, msg);
      }

   }
}

void setup() {
  if(DEBUG) Serial.begin(SERIAL_VELOCITY);                     //Start serial communication
  uv.begin(VEML6070_1_T);                                      //Start the UV sampling
  WiFi.mode(WIFI_STA);                                         //Set wifi to station mode, not an access point
  WiFi.setAutoReconnect(true);                                 //Reconnect to wifi automatically, if interrupted
  WiFi.begin(ssid, password);
  client_bot.setTrustAnchors(&cert);                           //Add root certificate for api.telegram.org
  connect();                                                   //Connect to wifi
  LittleFS.begin();                                            //Start Filesys
  if(DEBUG)Serial.println("Connected");
  if(DEBUG)Serial.print("IP Addr: ");
  if(DEBUG)Serial.println(WiFi.localIP());
  startTimeNclients();                                         //Get time from the set timezone
  readFile();
}

void loop() {
  if (millis() - dt > INTERVAL) {
      timeClient.update();
      sendWarning();
      if(WiFi.status() != WL_CONNECTED) connect();
      client_bot.setInsecure();                               //Avoid compatibility issues between the board ESP8266 and Telegram API. 100% secure!
      readTel();
      dt = millis();
  }
}
