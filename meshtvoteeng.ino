/*
 * MeshtVote v1.0, decentralized decisions in a dystopian future
 * Board: Any Meshtastic hardware + ESP32 through serial/UART
 * Developed by Roni Bandini 
 * Date 12/2025 - MIT License
 * 
 * - Admin via Access point, 192.168.4.1
 * - Also via DMs: admin 1234 open subject="Run or fight?" options="Run/Fight" duration=4
 * -               admin 1234 close
 *                 admin 1234 vote user !testUser 1 (testing purposes)
 */

#include <Arduino.h>
#include <Meshtastic.h>
#include <WiFi.h>
#include <WebServer.h>
#include <map>
#include <vector>

// ==========================================
// CONFIGURATION
// ==========================================

String ADMIN_PIN = "0000"; // please change this

// --- AP, connect and load 192.168.4.1 ---
const char* AP_SSID = "Meshtvote";
const char* AP_PASS = "00000000"; // please change this also
IPAddress local_IP(192,168,4,1);
IPAddress gateway(192,168,4,1);
IPAddress subnet(255,255,255,0);

// --- Meshtastic Pins  ---
#define SERIAL_RX_PIN 19 // to Hetltec 5
#define SERIAL_TX_PIN 20  // to Heltec  4
#define BAUD_RATE 115200 

// --- Voting Variables ---
std::vector<String> options; 
std::map<String, int> userVotes;

// State
String votingSubject = "Run or fight?"; 
bool votingActive = false;
unsigned long votingStartTime = 0;
unsigned long votingDurationMs = 28800000; // 8 Hours
bool hideVotingUsers = false;
bool broadcastLiveUpdates = false; 
int broadcastChannel = 0; 

// --- QUEUE SYSTEM ---
struct IncomingMessage {
  uint32_t fromNode;
  String text;
  uint8_t channel;
};
std::vector<IncomingMessage> messageQueue;

// Web Server
WebServer server(80);

// ==========================================
// HELPERS
// ==========================================

void initOptions() {
  options.clear();
  options.push_back("INVALID"); // Index 0
  options.push_back("Run");   // Index 1
  options.push_back("Fight");   // Index 2
}

String formatNodeId(uint32_t id) {
  String hexId = String(id, HEX);
  hexId.toLowerCase();
  return "!" + hexId;
}

String getTimeRemaining() {
  if (!votingActive) return "00:00";
  unsigned long now = millis();
  unsigned long elapsed = now - votingStartTime;
  if (elapsed >= votingDurationMs) return "00:00";
  
  unsigned long remaining = votingDurationMs - elapsed;
  unsigned long hours = remaining / 3600000;
  unsigned long mins = (remaining % 3600000) / 60000;
  
  String h = (hours < 10) ? "0" + String(hours) : String(hours);
  String m = (mins < 10) ? "0" + String(mins) : String(mins);
  return h + ":" + m;
}

String extractParam(String msg, String key) {
  String keyPattern = key + "=\"";
  int start = msg.indexOf(keyPattern);
  if (start == -1) return ""; 
  start += keyPattern.length();
  int end = msg.indexOf("\"", start);
  if (end == -1) return "";
  return msg.substring(start, end);
}

int extractIntParam(String msg, String key, int defaultValue) {
  String keyPattern = key + "=";
  int start = msg.indexOf(keyPattern);
  if (start == -1) return defaultValue;
  start += keyPattern.length();
  int end = msg.indexOf(" ", start);
  if (end == -1) end = msg.length();
  String val = msg.substring(start, end);
  return val.toInt();
}

String getMenuString() {
  String msg = "Subject: " + votingSubject + "\n";
  for(size_t i=1; i<options.size(); i++) {
    msg += String(i) + "=" + options[i] + "\n";
  }
  return msg;
}

// ==========================================
// BROADCAST LOGIC
// ==========================================

void startVotingBroadcast(long durationHours) {
  String msg = "OPEN VOTE: " + votingSubject + "\n";
  msg += "(Duration: " + String(durationHours) + "h)\n";
  msg += "Reply via DM with number:\n";
  
  for(size_t i=1; i<options.size(); i++) {
    msg += String(i) + "=" + options[i] + "\n";
  }
  
  Serial.println("[SYSTEM] Broadcasting START on Ch " + String(broadcastChannel));
  mt_send_text(msg.c_str(), 0xFFFFFFFF, broadcastChannel);
}

void broadcastResults() {
  String msg = "FINAL RESULTS: " + votingSubject + "\n";
  
  int totalVotes = userVotes.size();
  std::vector<int> counts(options.size(), 0);

  for(auto const& [user, option] : userVotes) {
    if(option >= 1 && option < options.size()) counts[option]++;
  }

  for(size_t i=1; i<options.size(); i++) {
     float pct = (totalVotes > 0) ? ((float)counts[i] / totalVotes) * 100.0 : 0.0;
     msg += options[i] + ": " + String(counts[i]) + " (" + String(pct, 1) + "%)\n";
  }
  
  msg += "Total: " + String(totalVotes);

  if (!hideVotingUsers && totalVotes > 0) {
      msg += "\n-- Voters --\n";
      for(auto const& [user, option] : userVotes) {
        msg += user + ": " + String(option) + "\n";
      }
  }

  Serial.println("[BROADCAST] Sending Results to Ch " + String(broadcastChannel));
  mt_send_text(msg.c_str(), 0xFFFFFFFF, broadcastChannel);
}

// ==========================================
// ADMIN LOGIC
// ==========================================

void processAdminCommand(uint32_t fromNode, String cmdLineRaw, uint8_t channel) {
  // Format: admin <PIN> <CMD> <ARGS>
  String cmdLine = cmdLineRaw;
  cmdLine.trim();

  // 1. Verify PIN
  int pinEnd = cmdLine.indexOf(' ');
  String inputPin = (pinEnd == -1) ? cmdLine : cmdLine.substring(0, pinEnd);
  
  if (inputPin != ADMIN_PIN) {
    mt_send_text("Admin: Auth Failed.", fromNode, channel);
    return;
  }

  // 2. Parse Command
  String remaining = (pinEnd == -1) ? "" : cmdLine.substring(pinEnd + 1);
  remaining.trim();
  int cmdEnd = remaining.indexOf(' ');
  String cmd = (cmdEnd == -1) ? remaining : remaining.substring(0, cmdEnd);
  String args = (cmdEnd == -1) ? "" : remaining.substring(cmdEnd + 1);
  
  Serial.println("[ADMIN] Command: " + cmd);

  if (cmd == "close") {
    if (votingActive) {
      broadcastResults();
      votingActive = false;
      mt_send_text("Admin: Voting Closed & Results Sent.", fromNode, channel);
    } else {
      mt_send_text("Admin: Voting was not active.", fromNode, channel);
    }
  }
  else if (cmd == "open") {
    // Parse Subject
    String newSub = extractParam(args, "subject");
    if (newSub != "") votingSubject = newSub;

    // Parse Options (Separator /)
    String optStr = extractParam(args, "options");
    if (optStr != "") {
      options.clear();
      options.push_back("INVALID");
      int start = 0;
      int end = optStr.indexOf('/');
      while (end != -1) {
        options.push_back(optStr.substring(start, end));
        start = end + 1;
        end = optStr.indexOf('/', start);
      }
      options.push_back(optStr.substring(start));
    }

    int dur = extractIntParam(args, "duration", -1);
    if (dur > 0) votingDurationMs = dur * 3600 * 1000UL;
    
    // Start
    userVotes.clear();
    votingActive = true;
    votingStartTime = millis();
    
    long h = votingDurationMs / 3600000;
    startVotingBroadcast(h);
    mt_send_text("Admin: Voting Started.", fromNode, channel);
  }
  else if (cmd == "vote") {
    // Expected: admin 1234 vote user !testID 1
    // "args" currently contains: user !testID 1
    
    int userIdx = args.indexOf("user");
    if (userIdx != -1) {
      // Find start of name (after "user ")
      int nameStart = userIdx + 5; 
      
      // Find space after name (before option index)
      int nameEnd = args.lastIndexOf(' '); 
      
      if (nameEnd > nameStart) {
        String fakeUser = args.substring(nameStart, nameEnd);
        fakeUser.trim();
        
        String valStr = args.substring(nameEnd + 1);
        int val = valStr.toInt();
        
        if (val >= 1 && val < options.size()) {
          userVotes[fakeUser] = val;
          Serial.println("[ADMIN] Injected Vote: " + fakeUser + " -> " + String(val));
          mt_send_text(("Admin: Injected vote for " + fakeUser).c_str(), fromNode, channel);
        } else {
          mt_send_text("Admin: Invalid Option Index.", fromNode, channel);
        }
      } else {
        mt_send_text("Admin: Parse Error (Name/Option).", fromNode, channel);
      }
    } else {
      mt_send_text("Admin: Syntax Error (Missing 'user').", fromNode, channel);
    }
  }
}

// ==========================================
// VOTING LOGIC
// ==========================================

void processVoteLogic(uint32_t fromNode, String text, uint8_t channel) {
  String senderID = formatNodeId(fromNode);
  text.trim();

  // --- Check for Admin Command ---
  String lowerText = text;
  lowerText.toLowerCase();
  if (lowerText.startsWith("admin ")) {
    processAdminCommand(fromNode, text.substring(6), channel);
    return; 
  }

  Serial.print("[PROCESS] Msg from " + senderID + ": " + text);

  if (!votingActive) {
    Serial.println(" -> CLOSED");
    mt_send_text("Voting is currently closed.", fromNode, channel);
    return;
  }

  if (userVotes.count(senderID) > 0) {
    Serial.println(" -> DUPLICATE");
    mt_send_text("Invalid: You have already voted.", fromNode, channel);
    return;
  }

  int selection = text.toInt();

  // Valid Vote
  if (selection >= 1 && selection < options.size()) {
    userVotes[senderID] = selection;
    Serial.println(" -> OK: " + options[selection]);
    
    // 1. Private Reply
    String reply = "Vote accepted for: " + options[selection];
    mt_send_text(reply.c_str(), fromNode, channel);

    // 2. Optional: Public Live Update
    if (broadcastLiveUpdates) {
      String pubMsg = "Update: New vote received! Total: " + String(userVotes.size());
      mt_send_text(pubMsg.c_str(), 0xFFFFFFFF, broadcastChannel);
    }

  } else {
    // Invalid Vote
    Serial.println(" -> INVALID");
    
    // Reply with Subject and Options
    String reply = "Invalid Option.\n" + getMenuString();
    if (reply.length() > 200) reply = reply.substring(0, 200);
    
    mt_send_text(reply.c_str(), fromNode, channel);
  }
}

// ==========================================
// WEB SERVER
// ==========================================

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'></head>";
  html += "<style>body{font-family:sans-serif; text-align:center; padding:20px;} .btn{padding:10px 20px; font-size:1.2rem; cursor:pointer;} input{padding:5px; margin:2px;}</style><body>";
  
  html += "<h1>MeshtVote Manager</h1>";
  
  if (votingActive) {
    html += "<h2 style='color:green;'>VOTING IN PROGRESS</h2>";
    html += "<h3>" + votingSubject + "</h3>";
    html += "Time Remaining: " + getTimeRemaining() + "<br><br>";
    html += "<form action='/stop' method='GET'><button class='btn' style='background:#f44336; color:white;'>STOP & BROADCAST</button></form>";
  } else {
    html += "<h2 style='color:grey;'>VOTING CLOSED</h2>";
    html += "<form action='/start' method='GET'>";
    html += "Duration (Hours): <input type='number' name='hours' value='8' min='1' max='24' style='width:50px;'><br><br>";
    html += "<button class='btn' style='background:#4CAF50; color:white;'>START VOTING</button>";
    html += "</form>";
  }

  // Live Stats
  html += "<hr><h3>Live Tally</h3>";
  
  int total = userVotes.size();
  std::vector<int> counts(options.size(), 0);
  for(auto const& [user, option] : userVotes) {
    if(option >= 1 && option < options.size()) counts[option]++;
  }
  
  html += "<ul style='list-style:none; padding:0;'>";
  for(size_t i=1; i<options.size(); i++) {
    float pct = (total > 0) ? ((float)counts[i]/total)*100 : 0;
    html += "<li><b>" + options[i] + ":</b> " + String(counts[i]) + " (" + String(pct,1) + "%)</li>";
  }
  html += "</ul>";
  html += "<p>Total Votes: " + String(total) + "</p>";

  // Settings
  html += "<hr><form action='/settings' method='POST' style='border:1px solid #ccc; padding:10px;'>";
  html += "<h3>Configuration</h3>";
  
  html += "<b>Subject:</b><br><input type='text' name='subject' value='" + votingSubject + "' style='width:90%;'><br><br>";
  html += "<b>Admin PIN:</b> <input type='text' name='pin' value='" + ADMIN_PIN + "' style='width:60px;'><br><br>";

  html += "<b>Broadcast Channel (Index):</b> <input type='number' name='chan' value='" + String(broadcastChannel) + "' style='width:50px;'><br><br>";

  html += "<b>Voting Options (Leave empty to remove):</b><br>";
  for(int i=1; i<=10; i++) {
    String val = (i < options.size()) ? options[i] : "";
    html += String(i) + ": <input type='text' name='opt" + String(i) + "' value='" + val + "'><br>";
  }
  
  html += "<br><input type='checkbox' name='live' " + String(broadcastLiveUpdates ? "checked" : "") + "> Broadcast valid votes<br>";
  html += "<input type='checkbox' name='hide' " + String(hideVotingUsers ? "checked" : "") + "> Hide Voter IDs in Results<br><br>";
  
  html += "<input type='submit' value='Save Settings'>";
  html += "</form>";

  html += "<br><br><small>Developed by Roni Bandini 11/2025 - MIT License</small>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleStart() {
  long hours = 8;
  if (server.hasArg("hours")) {
    int h = server.arg("hours").toInt();
    if (h > 0) hours = h;
  }
  votingDurationMs = hours * 3600 * 1000UL;
  
  votingActive = true;
  votingStartTime = millis();
  userVotes.clear();
  
  startVotingBroadcast(hours);

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleStop() {
  if(votingActive) {
    broadcastResults();
    votingActive = false;
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSettings() {
  if (server.hasArg("subject")) votingSubject = server.arg("subject");
  if (server.hasArg("pin")) ADMIN_PIN = server.arg("pin");
  if (server.hasArg("chan")) broadcastChannel = server.arg("chan").toInt();

  options.clear();
  options.push_back("INVALID"); 

  for(int i=1; i<=10; i++) {
    String argName = "opt" + String(i);
    if(server.hasArg(argName)) {
      String val = server.arg(argName);
      val.trim();
      if(val.length() > 0) {
        options.push_back(val);
      }
    }
  }
  
  if(options.size() < 3) {
    options.clear();
    options.push_back("INVALID");
    options.push_back("Yes");
    options.push_back("No");
  }

  if (server.hasArg("hide")) hideVotingUsers = true; else hideVotingUsers = false;
  if (server.hasArg("live")) broadcastLiveUpdates = true; else broadcastLiveUpdates = false;
  
  server.sendHeader("Location", "/");
  server.send(303);
}

// ==========================================
// MESHTASTIC CALLBACKS
// ==========================================

void connected_callback(mt_node_t* node, mt_nr_progress_t progress) {
  Serial.println("[MESH] Node Connected.");
}

void text_message_callback(uint32_t from, uint32_t to, uint8_t channel, const char* text) {
  if (to == 0xFFFFFFFF) return; // Ignore bcast

  IncomingMessage newMsg;
  newMsg.fromNode = from;
  newMsg.text = String(text);
  newMsg.channel = channel;
  
  messageQueue.push_back(newMsg);
}

// ==========================================
// SETUP
// ==========================================

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH); 
  Serial.begin(115200);
  delay(1000);
  
  initOptions();

  Serial.println("\n--- MeshtVote v1.0 ---");

  // AP
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(local_IP, gateway, subnet);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("[AP] Started: ");
  Serial.println(AP_SSID);

  // Web
  server.on("/", handleRoot);
  server.on("/start", handleStart);
  server.on("/stop", handleStop);
  server.on("/settings", handleSettings);
  server.begin();

  // Mesh
  mt_serial_init(SERIAL_RX_PIN, SERIAL_TX_PIN, BAUD_RATE);
  mt_request_node_report(connected_callback);
  set_text_message_callback(text_message_callback);
  
  digitalWrite(LED_BUILTIN, LOW); 
  Serial.println("[SYSTEM] Ready.");
}

// ==========================================
// LOOPing
// ==========================================

void loop() {
  mt_loop(millis());
  server.handleClient();

  if (!messageQueue.empty()) {
    IncomingMessage msg = messageQueue.front();
    processVoteLogic(msg.fromNode, msg.text, msg.channel);
    messageQueue.erase(messageQueue.begin());
  }

  if (votingActive) {
    if (millis() - votingStartTime > votingDurationMs) {
      Serial.println("[SYSTEM] Time Expired.");
      broadcastResults();
      votingActive = false;
    }
  }
}