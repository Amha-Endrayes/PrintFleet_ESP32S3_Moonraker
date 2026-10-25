/* PrintFleet v0.1.0 — ESP32-S3, Arduino-ESP32, ArduinoJson 7.3.1.
   Read-only Moonraker collector. See README for provisioning and limits. */
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <atomic>
#include <new>
#include <time.h>
#include "web_ui.h"

constexpr int MAX_PRINTERS=8, QUEUE_DEPTH=12, RESPONSE_LIMIT=16384, SAMPLE_LIMIT=24576;
WebServer web(80);
Preferences prefs;
SemaphoreHandle_t guard;
QueueHandle_t outbox;
String configText, collectorId, bootId, apName;
String live[MAX_PRINTERS], liveId[MAX_PRINTERS], discoveries="[]", uploadError="Not configured";
std::atomic<uint32_t> dropped{0}, uploaded{0}, uploadFailures{0};
std::atomic<bool> scanRequested{false}, scanRunning{false};
std::atomic<uint32_t> scanDone{0}, scanTotal{0};
uint64_t seq=0;
uint32_t restartAt=0;

struct Lock { Lock(){xSemaphoreTake(guard,portMAX_DELAY);} ~Lock(){xSemaphoreGive(guard);} };
String randomId(){ char b[33]; snprintf(b,sizeof(b),"%08lx%08lx%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random()); return b; }
String copyConfig(){ Lock l; return configText; }
void configDoc(JsonDocument &d){ deserializeJson(d,copyConfig()); }
void jsonSend(int code,const String &s){ web.sendHeader("Cache-Control","no-store"); web.send(code,"application/json",s); }
void sendDoc(int code,const JsonDocument &d){String s;serializeJson(d,s);jsonSend(code,s);}
void errorSend(int code,const char *message){JsonDocument d;d["error"]=message;sendDoc(code,d);}
bool authorize(bool mutation=false){
  JsonDocument c;configDoc(c);
  if(!web.authenticate("admin",c["admin_password"].as<const char*>())) {web.requestAuthentication();return false;}
  if(mutation && web.header("X-Requested-With")!="PrintFleet"){errorSend(403,"Missing request header");return false;}
  return true;
}
bool saveConfig(JsonDocument &d){
  String s;serializeJson(d,s);
  if(s.length()>14000)return false;
  {Lock l;if(s==configText)return true;}
  // A single NVS blob is replaced only on configuration changes.
  if(prefs.putBytes("config",s.c_str(),s.length())!=s.length())return false;
  {Lock l;configText=s;} return true;
}
bool safeText(const String &s,size_t n){return s.length()<=n && s.indexOf('\r')<0 && s.indexOf('\n')<0;}
bool validHost(const String &s){
  if(!s.length()||s.length()>253)return false;
  for(size_t i=0;i<s.length();i++)if(!isalnum((unsigned char)s[i])&&s[i]!='.'&&s[i]!='-')return false;
  return true;
}
bool validId(const String &s){
  if(!s.length()||s.length()>64)return false;
  for(size_t i=0;i<s.length();i++)if(!isalnum((unsigned char)s[i])&&s[i]!='-'&&s[i]!='_')return false;
  return true;
}

// HTTPClient decodes chunk framing. This sink limits bytes before JSON allocation.
class BoundedSink: public Stream {
 public:
  String body; size_t cap; uint32_t began; bool exceeded=false;
  BoundedSink(size_t limit):cap(limit),began(millis()){body.reserve(min(limit,(size_t)4096));}
  size_t write(uint8_t b) override {return write(&b,1);}
  size_t write(const uint8_t *b,size_t n) override {
    if(body.length()+n>cap || millis()-began>8000){exceeded=true;return 0;}
    size_t before=body.length();body.concat((const char*)b,n);return body.length()-before;
  }
  int available() override{return 0;} int read() override{return -1;} int peek() override{return -1;} void flush() override{}
};
int printerRequest(const String &host,int port,const String &key,const String &path,JsonDocument &result,const String &post="",int timeout=2000){
  WiFiClient client;HTTPClient http;
  http.setConnectTimeout(timeout);http.setTimeout(timeout);http.setReuse(false);
  if(!http.begin(client,"http://"+host+":"+String(port)+path))return -1;
  http.addHeader("Accept","application/json");
  if(key.length())http.addHeader("X-Api-Key",key);
  int code;
  if(post.length()){http.addHeader("Content-Type","application/json");code=http.POST(post);}else code=http.GET();
  if(code==200){
    BoundedSink sink(RESPONSE_LIMIT);
    int n=http.writeToStream(&sink);
    if(n<0||sink.exceeded)code=-20;
    else if(deserializeJson(result,sink.body)||!result["result"].is<JsonObject>())code=-21;
  }
  http.end();return code;
}
String urlEncode(const String &s){String o;const char *hex="0123456789ABCDEF";for(size_t i=0;i<s.length();i++){uint8_t c=s[i];if(isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~')o+=(char)c;else{o+='%';o+=hex[c>>4];o+=hex[c&15];}}return o;}
struct Runtime {String signature,objects,info,metadata,filename,proc;uint32_t next=0,refresh=0,failures=0,metaNext=0,procNext=0;};
Runtime runtime[MAX_PRINTERS];

void enqueue(String *s){
  if(!s){dropped++;return;}
  if(xQueueSend(outbox,&s,0)!=pdTRUE){delete s;dropped++;}
}
void pollPrinter(int slot,JsonObjectConst p,int interval){
  Runtime &r=runtime[slot];
  String signature=p["id"].as<String>()+"|"+p["host"].as<String>()+"|"+String(p["port"].as<int>())+"|"+p["api_key"].as<String>();
  if(r.signature!=signature){r=Runtime();r.signature=signature;}
  uint32_t now=millis();if((int32_t)(now-r.next)<0)return;
  String host=p["host"], key=p["api_key"];int port=p["port"];
  JsonDocument sample,reply;
  sample["schema_version"]=1;sample["collector_id"]=collectorId;sample["boot_id"]=bootId;
  sample["seq"]=++seq;sample["printer_id"]=p["id"];sample["name"]=p["name"];sample["model"]=p["model"];
  sample["host"]=host;sample["uptime_ms"]=(uint64_t)(esp_timer_get_time()/1000);
  time_t utc=time(nullptr);if(utc>1700000000)sample["captured_at"]=(int64_t)utc;else sample["captured_at"]=nullptr;
  JsonObject data=sample["data"].to<JsonObject>();
  int code=printerRequest(host,port,key,"/server/info",reply);
  bool online=code==200;String failure;
  if(online){
    data["server"]=reply["result"];
    if((int32_t)(now-r.procNext)>=0){
      r.procNext=now+60000;JsonDocument proc;
      if(printerRequest(host,port,key,"/machine/proc_stats",proc)==200){r.proc="";serializeJson(proc["result"],r.proc);}
    }
    if(r.proc.length()){JsonDocument proc;deserializeJson(proc,r.proc);data["process_stats"]=proc;}
    if(reply["result"]["klippy_state"]=="ready"){
      if(r.objects.isEmpty()||now-r.refresh>300000){
        JsonDocument list,info;
        int lc=printerRequest(host,port,key,"/printer/objects/list",list);
        if(lc==200 && list["result"]["objects"].is<JsonArray>()){
          r.objects="";serializeJson(list["result"]["objects"],r.objects);r.refresh=now;
        }else failure="Object discovery failed: "+String(lc);
        if(printerRequest(host,port,key,"/printer/info",info)==200){r.info="";serializeJson(info["result"],r.info);}
      }
      JsonDocument names,request;
      deserializeJson(names,r.objects);
      JsonObject objects=request["objects"].to<JsonObject>();
      int count=0;
      for(JsonVariantConst v:names.as<JsonArrayConst>()){
        String name=v.as<String>();
        // Configuration/macro variables can be very large or contain secrets.
        if(name=="configfile"||name=="bed_mesh"||name=="exclude_object"||name.startsWith("gcode_macro "))continue;
        if(count++>=128){failure="Object limit reached (128)";break;}
        objects[name]=nullptr;
      }
      if(objects.size()){
        String body;serializeJson(request,body);reply.clear();
        int qc=printerRequest(host,port,key,"/printer/objects/query",reply,body);
        if(qc==200 && reply["result"]["status"].is<JsonObject>()){
          data["objects"]=reply["result"]["status"];data["eventtime"]=reply["result"]["eventtime"];
        }else{
          failure="Telemetry query failed: "+String(qc)+"; using core objects";
          // Degrade gracefully for unusually large installations or vendor objects.
          String core="{\"objects\":{\"print_stats\":null,\"extruder\":null,\"heater_bed\":null,\"toolhead\":null,\"virtual_sdcard\":null,\"display_status\":null,\"webhooks\":null,\"fan\":null}}";
          reply.clear();if(printerRequest(host,port,key,"/printer/objects/query",reply,core)==200){data["objects"]=reply["result"]["status"];data["eventtime"]=reply["result"]["eventtime"];}
        }
      }
      if(r.info.length()){JsonDocument info;deserializeJson(info,r.info);data["printer_info"]=info;}
      String filename=data["objects"]["print_stats"]["filename"]|"";
      if(filename!=r.filename){r.filename=filename;r.metadata="";r.metaNext=0;}
      if(filename.length() && r.metadata.isEmpty() && (int32_t)(millis()-r.metaNext)>=0){
        r.metaNext=millis()+60000;
        JsonDocument meta;
        if(printerRequest(host,port,key,"/server/files/metadata?filename="+urlEncode(filename),meta)==200){
          // Omit thumbnail descriptors; images are never transported through ESP32.
          meta["result"].remove("thumbnails");serializeJson(meta["result"],r.metadata);
        }
      }
      if(r.metadata.length()){JsonDocument meta;deserializeJson(meta,r.metadata);data["file_metadata"]=meta;}
    }else{r.objects="";r.filename="";r.metadata="";}
  }else failure=(code==401||code==403)?"Moonraker API key required or rejected":"Moonraker unreachable / invalid response: "+String(code);
  sample["online"]=online;sample["error"]=failure;
  sample["diagnostics"]["dropped_samples"]=dropped.load();
  sample["diagnostics"]["free_heap"]=ESP.getFreeHeap();
  sample["diagnostics"]["rssi"]=WiFi.RSSI();
  String encoded;serializeJson(sample,encoded);
  if(encoded.length()>SAMPLE_LIMIT){
    data.remove("file_metadata");data.remove("process_stats");
    sample["error"]="Supplementary data omitted: sample size limit";
    encoded="";serializeJson(sample,encoded);
    if(encoded.length()>SAMPLE_LIMIT){sample["data"].clear();sample["error"]="Sample exceeded 24 KiB limit";encoded="";serializeJson(sample,encoded);}
  }
  {Lock l;live[slot]=encoded;liveId[slot]=p["id"].as<String>();}
  enqueue(new(std::nothrow) String(encoded));
  r.failures=online?0:min(r.failures+1,5U);
  uint32_t backoff=online?interval*1000U:min(60000U,interval*1000U*(1U<<r.failures));
  r.next=millis()+backoff+(esp_random()%300);
}

// Deliberately scan only the connected IPv4 subnet, at most 1022 addresses.
// Discovery is a separate worker so an absent host cannot stall sampling.
uint32_t toInt(IPAddress ip){return ((uint32_t)ip[0]<<24)|((uint32_t)ip[1]<<16)|((uint32_t)ip[2]<<8)|ip[3];}
IPAddress toIP(uint32_t n){return IPAddress(n>>24,n>>16,n>>8,n);}
void discoveryTask(void*){
  uint32_t last=0;bool first=true;
  for(;;){
    if(WiFi.status()==WL_CONNECTED && (scanRequested.exchange(false)||first||millis()-last>900000)){
      first=false;last=millis();scanRunning=true;scanDone=0;
      uint32_t ip=toInt(WiFi.localIP()), mask=toInt(WiFi.subnetMask()),network=ip&mask,broadcast=network|~mask;
      JsonDocument found;JsonArray entries=found.to<JsonArray>();
      uint32_t hosts=broadcast-network-1;scanTotal=hosts;
      if(hosts<=1022){
        for(uint32_t n=network+1;n<broadcast && WiFi.status()==WL_CONNECTED;n++){
          if(n!=ip){
            JsonDocument c;configDoc(c);int port=c["scan_port"]|7125;
            JsonDocument reply;int code=printerRequest(toIP(n).toString(),port,"","/server/info",reply,"",350);
            bool verified=code==200 && reply["result"]["moonraker_version"].is<const char*>() && reply["result"]["api_version"].is<JsonArray>();
            if((verified||code==401||code==403)&&entries.size()<32){
              JsonObject e=entries.add<JsonObject>();e["host"]=toIP(n).toString();e["port"]=port;e["verified"]=verified;
              e["state"]=verified?(reply["result"]["klippy_state"]|"unknown"):"Authentication required; verify manually";
              if(verified){JsonDocument info;if(printerRequest(toIP(n).toString(),port,"","/printer/info",info,"",700)==200)e["hostname"]=info["result"]["hostname"];}
              String out;serializeJson(found,out);{Lock l;discoveries=out;}
            }
          }
          scanDone++;vTaskDelay(pdMS_TO_TICKS(25));
        }
      }
      String out;serializeJson(found,out);{Lock l;discoveries=out;}scanRunning=false;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}
void collectorTask(void*){
  for(;;){
    if(WiFi.status()==WL_CONNECTED){
      JsonDocument c;configDoc(c);int i=0;
      for(JsonObjectConst p:c["printers"].as<JsonArrayConst>()){
        if(i>=MAX_PRINTERS)break;
        if(p["enabled"]|true)pollPrinter(i,p,c["poll_seconds"]|5);
        i++;vTaskDelay(pdMS_TO_TICKS(20));
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
void uploaderTask(void*){
  String *pending=nullptr;uint32_t retry=1000;
  for(;;){
    if(!pending)xQueueReceive(outbox,&pending,pdMS_TO_TICKS(500));
    if(!pending)continue;
    JsonDocument c;configDoc(c);String url=c["vps_url"]|"",token=c["vps_token"]|"",ca=c["root_ca"]|"";
    if(WiFi.status()!=WL_CONNECTED || url.isEmpty()||token.isEmpty()||ca.isEmpty()||time(nullptr)<1700000000){
      {Lock l;uploadError=WiFi.status()!=WL_CONNECTED?"Wi-Fi disconnected":url.isEmpty()?"VPS URL not set":token.isEmpty()?"VPS ingest token not set":ca.isEmpty()?"TLS root CA not set":"Waiting for NTP time";}
      vTaskDelay(pdMS_TO_TICKS(1000));continue;
    }
    WiFiClientSecure client;client.setCACert(ca.c_str());client.setHandshakeTimeout(8);
    HTTPClient http;http.setConnectTimeout(5000);http.setTimeout(5000);http.setReuse(false);
    int code=-1;bool ack=false;
    if(http.begin(client,url)){
      http.addHeader("Content-Type","application/json");http.addHeader("Authorization","Bearer "+token);
      code=http.POST(*pending);
      if(code==200){
        BoundedSink sink(1024);JsonDocument a,s;
        if(http.writeToStream(&sink)>=0 && !deserializeJson(a,sink.body) && !deserializeJson(s,*pending))
          ack=(a["accepted"]==true && a["seq"].as<uint64_t>()==s["seq"].as<uint64_t>() && a["boot_id"]==s["boot_id"]);
      }
      http.end();
    }
    if(ack){delete pending;pending=nullptr;uploaded++;retry=1000;{Lock l;uploadError="";}}
    else{
      uploadFailures++;{Lock l;uploadError="Upload failed: "+String(code)+(code==200?" (invalid acknowledgement)":"");}
      // A permanent schema/body rejection is visible and cannot block every later sample.
      if(code==400||code==413||code==422){delete pending;pending=nullptr;dropped++;}
      vTaskDelay(pdMS_TO_TICKS(retry+esp_random()%500));retry=min(retry*2,60000U);
    }
  }
}

void routes(){
  const char *headers[]={"X-Requested-With"};web.collectHeaders(headers,1);
  web.on("/",HTTP_GET,[]{if(authorize()){web.sendHeader("Cache-Control","no-store");web.send_P(200,"text/html",INDEX_HTML);}});
  web.on("/api/status",HTTP_GET,[]{
    if(!authorize())return;JsonDocument d,c;configDoc(c);
    d["collector_id"]=collectorId;d["ip"]=WiFi.localIP().toString();d["wifi"]=WiFi.status()==WL_CONNECTED;
    d["heap"]=ESP.getFreeHeap();d["psram"]=ESP.getPsramSize();d["uptime_s"]=(uint64_t)(esp_timer_get_time()/1000000);
    d["queued"]=uxQueueMessagesWaiting(outbox);d["uploaded"]=uploaded.load();d["dropped"]=dropped.load();d["upload_failures"]=uploadFailures.load();
    d["clock_synced"]=time(nullptr)>1700000000;d["scan_running"]=scanRunning.load();d["scan_done"]=scanDone.load();d["scan_total"]=scanTotal.load();
    d["vps_configured"]=String(c["vps_url"]|"").length()>0;
    JsonArray a=d["printers"].to<JsonArray>();
    {Lock l;d["upload_error"]=uploadError;JsonDocument found;deserializeJson(found,discoveries);d["discovered"]=found;
      for(JsonObjectConst p:c["printers"].as<JsonArrayConst>()){
        JsonObject item=a.add<JsonObject>();item["id"]=p["id"];item["name"]=p["name"];item["model"]=p["model"];item["host"]=p["host"];item["port"]=p["port"];item["enabled"]=p["enabled"];
        for(int i=0;i<MAX_PRINTERS;i++)if(liveId[i]==p["id"].as<String>() && live[i].length()){
          JsonDocument s;deserializeJson(s,live[i]);item["sample"]=s;item["age_s"]=((uint64_t)(esp_timer_get_time()/1000)-s["uptime_ms"].as<uint64_t>())/1000;break;
        }
      }
    }sendDoc(200,d);
  });
  web.on("/api/settings",HTTP_GET,[]{
    if(!authorize())return;JsonDocument c;configDoc(c);
    c.remove("admin_password");c["wifi_password_set"]=String(c["wifi_password"]|"").length()>0;c.remove("wifi_password");
    c["vps_token_set"]=String(c["vps_token"]|"").length()>0;c.remove("vps_token");
    for(JsonObject p:c["printers"].as<JsonArray>()){p["api_key_set"]=String(p["api_key"]|"").length()>0;p.remove("api_key");}
    sendDoc(200,c);
  });
  web.on("/api/settings",HTTP_POST,[]{
    if(!authorize(true))return;
    if(web.arg("plain").length()>14000){errorSend(413,"Settings too large");return;}
    JsonDocument input,c;if(deserializeJson(input,web.arg("plain"))){errorSend(400,"Invalid JSON");return;}configDoc(c);
    for(const char *k:{"ssid","vps_url","root_ca","ntp_host"})if(input[k].is<String>())c[k]=input[k];
    for(const char *k:{"wifi_password","vps_token","admin_password"})if(input[k].is<String>() && input[k].as<String>().length())c[k]=input[k];
    if(input["poll_seconds"].is<int>())c["poll_seconds"]=input["poll_seconds"];
    if(input["scan_port"].is<int>())c["scan_port"]=input["scan_port"];
    String url=c["vps_url"]|"",ca=c["root_ca"]|"";
    bool urlOK=url.isEmpty()||(url.startsWith("https://") && url.indexOf('@')<0 && url.indexOf('?')<0 && url.indexOf('#')<0 && safeText(url,512));
    if(!urlOK || !safeText(c["ssid"].as<String>(),32) || String(c["wifi_password"]|"").length()>63 || !safeText(c["vps_token"].as<String>(),256) || String(c["admin_password"]|"").length()<12 || String(c["admin_password"]|"").length()>63 || !validHost(c["ntp_host"].as<String>()) || (int)c["poll_seconds"]<2 || (int)c["poll_seconds"]>300 || (int)c["scan_port"]<1 || (int)c["scan_port"]>65535 || ca.length()>8000 || (ca.length() && (ca.indexOf("-----BEGIN CERTIFICATE-----")<0 || ca.indexOf("-----END CERTIFICATE-----")<0))){errorSend(400,"Invalid settings: check field limits and HTTPS/CA");return;}
    if(!saveConfig(c)){errorSend(500,"Could not save configuration");return;}jsonSend(200,"{\"saved\":true,\"restart_required\":true}");
  });
  web.on("/api/printers",HTTP_POST,[]{
    if(!authorize(true))return;if(web.arg("plain").length()>2048){errorSend(413,"Too large");return;}
    JsonDocument p,c;if(deserializeJson(p,web.arg("plain"))){errorSend(400,"Invalid JSON");return;}configDoc(c);
    String host=p["host"]|"",name=p["name"]|"",model=p["model"]|"",id=p["id"]|"",key=p["api_key"]|"";
    int port=p["port"]|7125;
    if(!validHost(host)||!name.length()||!safeText(name,96)||!safeText(model,96)||!safeText(key,256)||port<1||port>65535||(!id.isEmpty()&&!validId(id))){errorSend(400,"Invalid printer fields");return;}
    JsonArray a=c["printers"].as<JsonArray>();JsonObject target;
    for(JsonObject old:a){
      if(old["id"]==id)target=old;
      if(old["host"]==host && old["port"]==port && old["id"].as<String>()!=id){errorSend(409,"This endpoint is already saved");return;}
    }
    if(target.isNull()){
      if(!id.isEmpty()){errorSend(404,"Unknown printer ID");return;}
      if(a.size()>=MAX_PRINTERS){errorSend(409,"Maximum eight printers per collector");return;}
      id=randomId();target=a.add<JsonObject>();target["api_key"]="";
    }
    target["id"]=id;target["host"]=host;target["port"]=port;target["name"]=name;target["model"]=model;target["enabled"]=p["enabled"]|true;
    if(key.length() || (p["clear_api_key"]|false))target["api_key"]=key;
    if(!saveConfig(c)){errorSend(500,"NVS save failed");return;}jsonSend(200,"{\"saved\":true}");
  });
  web.on("/api/remove",HTTP_POST,[]{
    if(!authorize(true))return;JsonDocument p,c;if(deserializeJson(p,web.arg("plain"))){errorSend(400,"Invalid JSON");return;}configDoc(c);
    JsonArray a=c["printers"].as<JsonArray>();for(size_t i=0;i<a.size();i++)if(a[i]["id"]==p["id"]){a.remove(i);if(saveConfig(c))jsonSend(200,"{\"removed\":true}");else errorSend(500,"NVS save failed");return;}errorSend(404,"Unknown printer");
  });
  web.on("/api/scan",HTTP_POST,[]{if(authorize(true)){scanRequested=true;jsonSend(202,"{\"started\":true}");}});
  web.on("/api/restart",HTTP_POST,[]{if(authorize(true)){jsonSend(200,"{\"restarting\":true}");restartAt=millis()+1000;}});
  web.onNotFound([]{errorSend(404,"Not found");});
}
void setup(){
  Serial.begin(115200);delay(500);
  // Arduino ESP32 routes large malloc allocations to PSRAM when enabled in board settings.
  if(!psramFound()){Serial.println("PSRAM required. Enable OPI/QSPI PSRAM for your S3 board.");while(true)delay(1000);}
  guard=xSemaphoreCreateMutex();outbox=xQueueCreate(QUEUE_DEPTH,sizeof(String*));
  if(!guard||!outbox){Serial.println("Allocation failed");while(true)delay(1000);}
  if(!prefs.begin("printfleet",false)){Serial.println("NVS open failed");while(true)delay(1000);}
  collectorId=prefs.getString("collector","");if(collectorId.isEmpty()){collectorId=randomId();prefs.putString("collector",collectorId);}
  bootId=randomId();
  size_t bytes=prefs.getBytesLength("config");
  if(bytes && bytes<=14000){char *buf=(char*)malloc(bytes+1);if(buf){if(prefs.getBytes("config",buf,bytes)==bytes){buf[bytes]=0;configText=buf;}free(buf);}}
  JsonDocument c;
  if(deserializeJson(c,configText)||!c["admin_password"].is<String>()){
    c.clear();c["admin_password"]=randomId().substring(0,16);c["ssid"]="";c["wifi_password"]="";
    c["vps_url"]="";c["vps_token"]="";c["root_ca"]="";c["ntp_host"]="pool.ntp.org";c["poll_seconds"]=5;c["scan_port"]=7125;c["printers"].to<JsonArray>();
    if(!saveConfig(c)){Serial.println("NVS write failed");while(true)delay(1000);}
  }
  Serial.printf("\nPrintFleet admin / %s\n",c["admin_password"].as<const char*>());
  WiFi.persistent(false);WiFi.mode(WIFI_AP_STA);WiFi.setHostname("printfleet");WiFi.setAutoReconnect(true);WiFi.setSleep(false);
  if(String(c["ssid"]|"").length())WiFi.begin(c["ssid"].as<const char*>(),c["wifi_password"].as<const char*>());
  apName="PrintFleet-"+collectorId.substring(0,6);
  // Setup AP stays available until STA connects; reopens after a 30-second outage.
  WiFi.softAP(apName.c_str(),c["admin_password"].as<const char*>());
  Serial.printf("Setup Wi-Fi %s; password is admin password; open http://192.168.4.1\n",apName.c_str());
  configTime(0,0,c["ntp_host"].as<const char*>());
  routes();web.begin();
  if(xTaskCreate(collectorTask,"collector",16384,nullptr,1,nullptr)!=pdPASS ||
     xTaskCreate(uploaderTask,"uploader",12288,nullptr,1,nullptr)!=pdPASS ||
     xTaskCreate(discoveryTask,"discovery",8192,nullptr,1,nullptr)!=pdPASS){Serial.println("Task allocation failed");delay(1000);ESP.restart();}
}
void loop(){
  web.handleClient();
  static uint32_t disconnectedAt=0,lastReconnect=0;static bool ap=true,wasConnected=false;
  bool connected=WiFi.status()==WL_CONNECTED;
  if(connected){
    if(!wasConnected){Serial.printf("LAN dashboard: http://%s\n",WiFi.localIP().toString().c_str());scanRequested=true;}
    disconnectedAt=0;if(ap){WiFi.softAPdisconnect(true);ap=false;}
  }else{
    if(!disconnectedAt)disconnectedAt=millis();
    if(millis()-disconnectedAt>30000 && !ap){JsonDocument c;configDoc(c);WiFi.softAP(apName.c_str(),c["admin_password"].as<const char*>());ap=true;}
    if(millis()-lastReconnect>15000){lastReconnect=millis();WiFi.reconnect();}
  }
  wasConnected=connected;
  if(restartAt && (int32_t)(millis()-restartAt)>=0)ESP.restart();
  delay(2);
}
