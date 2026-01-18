/*
 * M5StickC Plus Voice Assistant - v2.7
 * 
 * Hardware: M5StickC Plus (ESP32-PICO-V3-02)
 * 
 * Features:
 * ✨ 4-second recording (12kHz - balanced quality & time)
 * ✨ Conversation mode - Button A continues topic!
 * ✨ Kid-friendly responses (30 words max)
 * ✨ Friendly face animation
 * ✨ Persistent UI with visible button prompts
 * 
 * Button Controls:
 * - Button A (click): CONTINUE same topic (remembers context)
 * - Button B (click): Ask a NEW question (starts fresh)
 * - Button B (hold 3 sec): Reset WiFi credentials
 */

#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>

// ============================================================================
// 🔐 CONFIGURATION - PASTE YOUR CREDENTIALS HERE
// ============================================================================


// ===================== AZURE OPENAI CONFIGURATION =====================

const char* AZURE_OPENAI_KEY = "YOUR_AZURE_OPENAI_KEY_HERE";
const char* AZURE_OPENAI_ENDPOINT = "https://YOUR_RESOURCE_NAME.openai.azure.com"; // No trailing slash
const char* AZURE_OPENAI_DEPLOYMENT = "YOUR_CHAT_DEPLOYMENT_NAME"; // e.g. "gpt-4o-mini"
const char* AZURE_OPENAI_WHISPER_DEPLOYMENT = "YOUR_WHISPER_DEPLOYMENT_NAME"; // e.g. "whisper-1"
const char* AZURE_OPENAI_API_VERSION = "2024-02-15-preview"; // Or latest supported

// Optional: Pre-fill WiFi (or leave blank to use web setup at 192.168.4.1)
const char* WIFI_SSID = "";
const char* WIFI_PASSWORD = "";

// ============================================================================
// END CONFIGURATION - Don't change anything below this line!
// ============================================================================

// ============================================================================
// DISPLAY SETTINGS
// ============================================================================

static const int WIDTH  = 240;
static const int HEIGHT = 135;

// ============================================================================
// AUDIO RECORDING SETTINGS
// ============================================================================

static const int SAMPLE_RATE = 12000;      // 12kHz - balanced quality for 4 sec recording
static const int RECORD_SECONDS = 4;       // 4 seconds at 12kHz
static const int RECORD_SAMPLES = SAMPLE_RATE * RECORD_SECONDS;
static int16_t* audioBuffer = nullptr;     // Allocated dynamically to save memory

// ============================================================================
// CONVERSATION HISTORY
// ============================================================================

// Store only LAST 2 messages (1 Q&A pair) to conserve memory
const int MAX_HISTORY = 2;  // Reduced from 6 to prevent memory issues
String conversationHistory[MAX_HISTORY];
int historyCount = 0;
bool conversationMode = false;

// ============================================================================
// WIFI & WEB SERVER
// ============================================================================

Preferences prefs;           // Persistent storage for WiFi credentials
WebServer server(80);        // Web server for WiFi setup
String savedSSID = "";       // Saved WiFi network name
String savedPass = "";       // Saved WiFi password
bool apMode = false;         // True when in Access Point mode

String response = "Press A for NEW topic\nPress C to CONTINUE";

// ============================================================================
// HTML PAGES FOR WIFI SETUP
// ============================================================================

const char* setupPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial; background: #1a1a2e; color: #fff; padding: 20px; }
    .container { max-width: 300px; margin: 0 auto; }
    h1 { color: #0f0; font-size: 24px; }
    input { width: 100%; padding: 12px; margin: 8px 0; box-sizing: border-box; 
            border-radius: 8px; border: none; font-size: 16px; }
    input[type=submit] { background: #0f0; color: #000; cursor: pointer; 
                         font-weight: bold; }
    input[type=submit]:hover { background: #0c0; }
  </style>
</head>
<body>
  <div class="container">
    <h1>M5 Voice Setup</h1>
    <form action="/save" method="POST">
      <input type="text" name="ssid" placeholder="WiFi Name" required>
      <input type="password" name="pass" placeholder="WiFi Password" required>
      <input type="submit" value="Connect">
    </form>
  </div>
</body>
</html>
)rawliteral";

const char* successPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial; background: #1a1a2e; color: #fff; padding: 20px; 
           text-align: center; }
    h1 { color: #0f0; }
  </style>
</head>
<body>
  <h1>Saved!</h1>
  <p>Device is restarting...</p>
</body>
</html>
)rawliteral";

// ============================================================================
// CONVERSATION MANAGEMENT FUNCTIONS (NEW!)
// ============================================================================

// Add a message to conversation history
void addToHistory(const String &role, const String &content) {
  // Memory safety check - if running low, clear history
  if (ESP.getFreeHeap() < 50000) {
    Serial.println("⚠️ Low memory detected! Clearing history to prevent crash.");
    clearHistory();
    return;
  }
  
  if (historyCount < MAX_HISTORY) {
    conversationHistory[historyCount] = "\"role\":\"" + role + "\",\"content\":\"" + content + "\"";
    historyCount++;
  } else {
    // History full - shift everything left and add new message at end
    for (int i = 0; i < MAX_HISTORY - 1; i++) {
      conversationHistory[i] = conversationHistory[i + 1];
    }
    conversationHistory[MAX_HISTORY - 1] = "\"role\":\"" + role + "\",\"content\":\"" + content + "\"";
  }
  
  Serial.println("\n=== Conversation History ===");
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
  for (int i = 0; i < historyCount; i++) {
    Serial.println(conversationHistory[i]);
  }
  Serial.println("============================\n");
}

// Clear conversation history (start fresh topic)
void clearHistory() {
  historyCount = 0;
  conversationMode = false;
  Serial.println("🆕 Conversation history cleared - starting fresh topic!");
}

// Get conversation history as JSON array
String getHistoryJSON() {
  String history = "";
  for (int i = 0; i < historyCount; i++) {
    if (i > 0) history += ",";
    history += "{" + conversationHistory[i] + "}";
  }
  return history;
}

// ============================================================================
// DISPLAY FUNCTIONS
// ============================================================================

// Display text on screen (supports multi-line with \n)
void drawScreen(const String &text) {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextDatum(TL_DATUM);
  M5.Display.setTextSize(1);
  
  int y = 10;
  int startPos = 0;
  
  // Draw each line separately
  for (unsigned int i = 0; i <= text.length(); i++) {
    if (i == text.length() || text[i] == '\n') {
      String line = text.substring(startPos, i);
      M5.Display.drawString(line, 10, y);
      y += 18;
      startPos = i + 1;
    }
  }
}

// NEW: Display answer with persistent button hints at bottom
void drawAnswerWithButtons(const String &answer) {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextDatum(TL_DATUM);
  M5.Display.setTextSize(1);
  
  // Draw answer text (leave room at bottom for buttons)
  int y = 3;
  int startPos = 0;
  int maxY = 105;  // Stop before button area
  
  for (unsigned int i = 0; i <= answer.length(); i++) {
    if (i == answer.length() || answer[i] == '\n') {
      String line = answer.substring(startPos, i);
      if (y < maxY) {
        M5.Display.drawString(line, 3, y);  // Reduced margin from 5 to 3
        y += 15;
      }
      startPos = i + 1;
    }
  }
  
  // Draw separator line
  M5.Display.drawLine(0, 110, WIDTH, 110, TFT_DARKGREY);
  
  // Draw buttons SIDE BY SIDE in landscape
  // Left button (A) - Continue
  M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
  M5.Display.drawString("A=More", 5, 118);
  
  // Right button (B) - New
  M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Display.drawString("B=New Q", 165, 118);
}

// Show friendly face animation while listening
void drawListening(int seconds) {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(2);
  
  // Centered text
  M5.Display.drawString("Listening...", 60, 10);
  
  M5.Display.setTextSize(1);
  M5.Display.drawString(String(seconds) + " sec", 100, 35);
  
  // Simple friendly face - ears get bigger as it listens
  int earSize = 3 + (seconds % 2) * 2;  // Pulse between sizes
  
  // Eyes (two dots)
  M5.Display.fillCircle(90, 70, 4, TFT_WHITE);
  M5.Display.fillCircle(130, 70, 4, TFT_WHITE);
  
  // Smile
  M5.Display.drawCircle(110, 80, 15, TFT_WHITE);
  M5.Display.fillRect(95, 65, 30, 15, TFT_BLACK);  // Hide top half
  
  // Ears (get bigger while listening - shows active listening!)
  M5.Display.fillCircle(70, 70, earSize, TFT_WHITE);
  M5.Display.fillCircle(150, 70, earSize, TFT_WHITE);
}

// Show friendly face animation while thinking
void drawThinking() {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(2);
  
  M5.Display.drawString("Thinking...", 60, 10);
  
  // Thinking face - eyes closed, slight smile
  // Eyes (lines instead of dots - closed)
  M5.Display.drawLine(85, 70, 95, 70, TFT_WHITE);
  M5.Display.drawLine(125, 70, 135, 70, TFT_WHITE);
  
  // Gentle smile
  M5.Display.drawCircle(110, 85, 12, TFT_WHITE);
  M5.Display.fillRect(98, 70, 24, 12, TFT_BLACK);  // Hide top half
  
  // Thought bubble dots
  int offset = (millis() / 300) % 3;  // Animate dots
  if (offset >= 0) M5.Display.fillCircle(150, 50, 2, TFT_WHITE);
  if (offset >= 1) M5.Display.fillCircle(160, 45, 3, TFT_WHITE);
  if (offset >= 2) M5.Display.fillCircle(172, 42, 4, TFT_WHITE);
}

// Show which mode we're in
void showReadyScreen() {
  if (conversationMode) {
    drawScreen("🔗 SAME TOPIC\n\nA = Keep going\nB = New topic");
  } else {
    drawScreen("Press A\nto ask a question");
  }
}

// ============================================================================
// WIFI SETUP FUNCTIONS
// ============================================================================

void handleRoot() {
  Serial.println("Serving setup page");
  server.send(200, "text/html", setupPage);
}

void handleSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  
  Serial.println("Received credentials:");
  Serial.println("  SSID: " + ssid);
  Serial.println("  Pass: " + String(pass.length()) + " chars");
  
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  
  Serial.println("Credentials saved to Preferences");
  
  server.send(200, "text/html", successPage);
  
  delay(2000);
  ESP.restart();
}

void startAPMode() {
  Serial.println("\n========== STARTING AP MODE ==========");
  
  apMode = true;
  
  WiFi.mode(WIFI_AP);
  WiFi.softAP("M5Voice-Setup");
  
  IPAddress ip = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(ip);
  
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
  
  Serial.println("Web server started");
  Serial.println("=======================================\n");
  
  drawScreen("WiFi Setup\n\nConnect to:\nM5Voice-Setup\n\nThen go to:\n192.168.4.1");
}

bool connectToWiFi() {
  prefs.begin("wifi", true);
  savedSSID = prefs.getString("ssid", WIFI_SSID);  // Use config default
  savedPass = prefs.getString("pass", WIFI_PASSWORD);
  prefs.end();
  
  Serial.println("\n========== WIFI CONNECTION ==========");
  Serial.println("Stored SSID: " + savedSSID);
  Serial.println("Stored pass: " + String(savedPass.length()) + " chars");
  
  if (savedSSID.length() == 0) {
    Serial.println("No credentials stored");
    Serial.println("======================================\n");
    return false;
  }
  
  drawScreen("Connecting to\n" + savedSSID + "...");
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSSID.c_str(), savedPass.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    Serial.println("======================================\n");
    return true;
  }
  
  Serial.println("\nFailed to connect");
  Serial.println("======================================\n");
  return false;
}

void resetCredentials() {
  Serial.println("\n========== RESETTING CREDENTIALS ==========");
  
  drawScreen("Resetting...");
  
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();
  
  Serial.println("Credentials cleared, restarting...");
  Serial.println("============================================\n");
  
  delay(1000);
  ESP.restart();
}

// ============================================================================
// AUDIO RECORDING FUNCTIONS
// ============================================================================

bool recordAudio() {
  Serial.println("\n========== RECORDING ==========");
  
  // Buffer was allocated in setup()
  if (!audioBuffer) {
    Serial.println("ERROR: Audio buffer not allocated!");
    return false;
  }

  // Mic is already initialized in setup()
  Serial.println("Starting recording...");
  
  int samplesPerSecond = SAMPLE_RATE;
  unsigned long recordStartTime = millis();
  
  for (int sec = 0; sec < RECORD_SECONDS; sec++) {
    Serial.printf("Recording second %d/%d...\n", sec + 1, RECORD_SECONDS);
    
    // Start recording this second
    M5.Mic.record(&audioBuffer[sec * samplesPerSecond], samplesPerSecond, SAMPLE_RATE);
    
    // Update listening face while recording
    while (M5.Mic.isRecording()) {
      drawListening(RECORD_SECONDS - sec);
      delay(50);
    }
  }
  
  Serial.println("Recording complete");

  // Check audio quality
  int16_t minVal = 32767, maxVal = -32768;
  int64_t sum = 0;
  for (int i = 0; i < RECORD_SAMPLES; i++) {
    if (audioBuffer[i] < minVal) minVal = audioBuffer[i];
    if (audioBuffer[i] > maxVal) maxVal = audioBuffer[i];
    sum += abs(audioBuffer[i]);
  }
  Serial.printf("Audio stats: min=%d, max=%d, avg=%lld\n", 
                minVal, maxVal, sum / RECORD_SAMPLES);
  Serial.println("================================\n");
  
  return true;
}

void createWavHeader(uint8_t* header, int dataSize) {
  int fileSize = dataSize + 36;
  int byteRate = SAMPLE_RATE * 1 * 16 / 8;
  int blockAlign = 1 * 16 / 8;

  memcpy(header, "RIFF", 4);
  header[4] = fileSize & 0xFF;
  header[5] = (fileSize >> 8) & 0xFF;
  header[6] = (fileSize >> 16) & 0xFF;
  header[7] = (fileSize >> 24) & 0xFF;
  memcpy(header + 8, "WAVE", 4);
  memcpy(header + 12, "fmt ", 4);
  header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;
  header[20] = 1; header[21] = 0;
  header[22] = 1; header[23] = 0;
  header[24] = SAMPLE_RATE & 0xFF;
  header[25] = (SAMPLE_RATE >> 8) & 0xFF;
  header[26] = (SAMPLE_RATE >> 16) & 0xFF;
  header[27] = (SAMPLE_RATE >> 24) & 0xFF;
  header[28] = byteRate & 0xFF;
  header[29] = (byteRate >> 8) & 0xFF;
  header[30] = (byteRate >> 16) & 0xFF;
  header[31] = (byteRate >> 24) & 0xFF;
  header[32] = blockAlign; header[33] = 0;
  header[34] = 16; header[35] = 0;
  memcpy(header + 36, "data", 4);
  header[40] = dataSize & 0xFF;
  header[41] = (dataSize >> 8) & 0xFF;
  header[42] = (dataSize >> 16) & 0xFF;
  header[43] = (dataSize >> 24) & 0xFF;
}

// ============================================================================
// OPENAI API FUNCTIONS
// ============================================================================


String transcribeAudio() {
  Serial.println("\n========== TRANSCRIBING (Azure OpenAI) ==========");
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(60);

  String host = String(AZURE_OPENAI_ENDPOINT);
  host.replace("https://", "");
  int slash = host.indexOf('/');
  if (slash > 0) host = host.substring(0, slash); // Remove any trailing path

  String url = String(AZURE_OPENAI_ENDPOINT) + "/openai/deployments/" + AZURE_OPENAI_WHISPER_DEPLOYMENT + "/audio/transcriptions?api-version=" + AZURE_OPENAI_API_VERSION;
  Serial.println("Connecting to " + host + "...");
  if (!client.connect(host.c_str(), 443)) {
    Serial.println("ERROR: Connection failed!");
    return "Connection failed";
  }
  Serial.println("Connected");

  int audioDataSize = RECORD_SAMPLES * sizeof(int16_t);
  uint8_t wavHeader[44];
  createWavHeader(wavHeader, audioDataSize);

  String boundary = "----ESP32Boundary";
  String bodyStart = "--" + boundary + "\r\n";
  bodyStart += "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n";
  bodyStart += "Content-Type: audio/wav\r\n\r\n";
  String bodyEnd = "\r\n--" + boundary + "\r\n";
  bodyEnd += "Content-Disposition: form-data; name=\"response_format\"\r\n\r\ntext\r\n";
  bodyEnd += "--" + boundary + "--\r\n";

  int contentLength = bodyStart.length() + 44 + audioDataSize + bodyEnd.length();
  Serial.printf("Content length: %d bytes (audio: %d bytes)\n", contentLength, audioDataSize);

  // Send HTTP request
  client.print("POST " + url.substring(url.indexOf('/', 8)) + " HTTP/1.1\r\n");
  client.print("Host: " + host + "\r\n");
  client.print("api-key: " + String(AZURE_OPENAI_KEY) + "\r\n");
  client.print("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");
  client.print("Content-Length: " + String(contentLength) + "\r\n");
  client.print("Connection: close\r\n\r\n");

  client.print(bodyStart);
  client.write(wavHeader, 44);
  int chunkSize = 4096;
  uint8_t* ptr = (uint8_t*)audioBuffer;
  int remaining = audioDataSize;
  int sent = 0;
  while (remaining > 0) {
    int toSend = min(chunkSize, remaining);
    client.write(ptr, toSend);
    ptr += toSend;
    remaining -= toSend;
    sent += toSend;
    if (sent % 16384 == 0) {
      Serial.printf("  Sent %d / %d bytes\n", sent, audioDataSize);
    }
    delay(1);
  }
  client.print(bodyEnd);
  Serial.println("Request sent, waiting for response...");

  unsigned long timeout = millis();
  while (!client.available()) {
    if (millis() - timeout > 60000) {
      Serial.println("ERROR: Timeout waiting for response!");
      client.stop();
      return "Timeout";
    }
    delay(100);
  }

  Serial.println("Response received, reading headers...");
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    Serial.println("  " + line);
    if (line == "\r") break;
  }

  String response = client.readString();
  client.stop();

  Serial.println("Azure Whisper response body:");
  Serial.println(response);

  int textIdx = response.indexOf("\"text\"");
  if (textIdx < 0) {
    Serial.println("ERROR: No 'text' field in response!");
    return "No transcription";
  }
  int start = response.indexOf('"', textIdx + 6);
  if (start < 0) {
    Serial.println("ERROR: Parse error!");
    return "Parse error";
  }
  start++;
  String result;
  bool escaped = false;
  for (unsigned int i = start; i < response.length(); i++) {
    char c = response[i];
    if (escaped) {
      result += c;
      escaped = false;
    } else if (c == '\\') {
      escaped = true;
    } else if (c == '"') {
      break;
    } else {
      result += c;
    }
  }
  Serial.println("Transcription: " + result);
  Serial.println("==================================\n");
  return result;
}

// ✨ ENHANCED: Now supports conversation history!

String askGPT(const String &question) {
  Serial.println("\n========== ASKING GPT (Azure OpenAI) ==========");
  Serial.println("Question: " + question);
  Serial.println("Conversation mode: " + String(conversationMode ? "YES" : "NO"));

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();

  String url = String(AZURE_OPENAI_ENDPOINT) + "/openai/deployments/" + AZURE_OPENAI_DEPLOYMENT + "/chat/completions?api-version=" + AZURE_OPENAI_API_VERSION;
  Serial.println("Connecting to Azure OpenAI...");
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("api-key", String(AZURE_OPENAI_KEY));
  http.setTimeout(90000);

  // Escape special characters in question
  String escaped = question;
  escaped.replace("\\", "\\\\");
  escaped.replace("\"", "\\\"");
  escaped.replace("\n", " ");

  // Build messages array with history
  String messages;
  if (conversationMode && historyCount > 0) {
    messages = getHistoryJSON() + ",";
    messages += "{\"role\":\"user\",\"content\":\"" + escaped + "\"}";
  } else {
    messages = "{\"role\":\"user\",\"content\":\"" + escaped + "\"}";
  }

  // ✨ KID-FRIENDLY SYSTEM PROMPT
  String systemPrompt = "You are a calm, neutral explainer. Answer clearly and directly. Do not praise the question. Do not use exclamations or emojis. Avoid hype or encouragement language. Use simple but precise wording. Keep answers short by default, but prioritise clarity over brevity.";

  // Build JSON request with system prompt
  String body =
    "{"
      "\"messages\":["
        "{\"role\":\"system\",\"content\":\"" + systemPrompt + "\"},"
        + messages +
      "],"
      "\"max_tokens\":150"
    "}";

  Serial.println("Sending request...");
  Serial.println("Body: " + body);

  int httpCode = http.POST(body);
  Serial.printf("HTTP response code: %d\n", httpCode);

  if (httpCode != 200) {
    String error = http.getString();
    Serial.println("Error response: " + error);
    http.end();
    return "HTTP " + String(httpCode);
  }

  String resp = http.getString();
  http.end();

  Serial.println("Azure GPT response:");
  Serial.println(resp);

  int contentIdx = resp.indexOf("\"content\"");
  if (contentIdx < 0) {
    Serial.println("ERROR: No 'content' in response!");
    return "No content";
  }

  int start = resp.indexOf(":", contentIdx);
  if (start < 0) {
    Serial.println("ERROR: Parse error!");
    return "Parse error";
  }
  start = resp.indexOf("\"", start) + 1;

  String result;
  bool esc = false;
  for (unsigned int i = start; i < resp.length(); i++) {
    char c = resp[i];
    if (esc) {
      if (c == 'n') result += '\n';
      else if (c == 'u') { i += 4; result += '-'; }
      else result += c;
      esc = false;
    } else if (c == '\\') {
      esc = true;
    } else if (c == '"') {
      break;
    } else {
      result += c;
    }
  }

  // ✨ Save to conversation history
  addToHistory("user", escaped);
  addToHistory("assistant", result);
  conversationMode = true;

  Serial.println("Extracted answer: " + result);
  Serial.println("=================================\n");

  return result;
}

// ============================================================================
// TEXT FORMATTING
// ============================================================================

String wordWrap(const String &text, int maxChars) {
  String result;
  String word;
  int lineLen = 0;
  
  for (unsigned int i = 0; i <= text.length(); i++) {
    char c = (i < text.length()) ? text[i] : ' ';
    
    if (c == ' ' || c == '\n') {
      if (lineLen + word.length() > maxChars) {
        result += '\n';
        lineLen = 0;
      }
      result += word;
      lineLen += word.length();
      word = "";
      
      if (c == ' ' && lineLen > 0) {
        result += ' ';
        lineLen++;
      }
      if (c == '\n') {
        result += '\n';
        lineLen = 0;
      }
    } else {
      word += c;
    }
  }
  
  return result;
}

// ============================================================================
// SETUP
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n========================================");
  Serial.println("   M5StickC Plus Voice Assistant v2.7");
  Serial.println("   ✨ Memory-Optimized!");
  Serial.println("========================================\n");

  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);

  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());

  // Allocate audio buffer EARLY, before WiFi uses memory
  Serial.printf("Allocating audio buffer: %d samples, %d bytes\n", 
                RECORD_SAMPLES, RECORD_SAMPLES * sizeof(int16_t));
  audioBuffer = (int16_t*)heap_caps_malloc(RECORD_SAMPLES * sizeof(int16_t), MALLOC_CAP_8BIT);
  if (!audioBuffer) {
    Serial.println("ERROR: Failed to allocate audio buffer!");
    drawScreen("Memory Error!\n\nCan't allocate\naudio buffer\n\nTry restarting");
    while(1) delay(1000);  // Halt
  }
  Serial.println("Audio buffer allocated successfully");
  Serial.printf("Free heap after buffer: %d bytes\n", ESP.getFreeHeap());

  if (!connectToWiFi()) {
    startAPMode();
    return;
  }

  showReadyScreen();
  
  // Initialize microphone once at startup
  auto mic_cfg = M5.Mic.config();
  mic_cfg.sample_rate = SAMPLE_RATE;
  M5.Mic.config(mic_cfg);
  M5.Mic.begin();
  
  Serial.println("\n🎤 Ready!");
  Serial.println("Button A = Continue topic (keeps history)");
  Serial.println("Button B = New topic (clears history) OR hold 3 sec to reset WiFi");
  Serial.println();
}

// ============================================================================
// MAIN LOOP
// ============================================================================

void loop() {
  M5.update();

  if (apMode) {
    server.handleClient();
    if (M5.BtnB.pressedFor(3000)) {
      resetCredentials();
    }
    return;
  }

  if (M5.BtnB.pressedFor(3000)) {
    resetCredentials();
  }

  // Important: Check long press BEFORE short press to avoid triggering both
  // If button is being held for WiFi reset, don't process as conversation continue

  // ✨ BUTTON A: CONTINUE/FIRST QUESTION
  if (M5.BtnA.wasClicked()) {
    
    // Check memory before starting
    if (ESP.getFreeHeap() < 40000) {
      drawScreen("Memory low!\nRestarting...");
      delay(2000);
      ESP.restart();
      return;
    }
    
    if (historyCount == 0) {
      Serial.println("\n*** BUTTON A: FIRST QUESTION ***\n");
    } else {
      Serial.println("\n*** BUTTON A: CONTINUE TOPIC ***\n");
    }
    
    Serial.printf("Free heap before recording: %d bytes\n", ESP.getFreeHeap());
    
    if (!recordAudio()) {
      drawScreen("Mic error");
      delay(2000);
      showReadyScreen();
      return;
    }

    Serial.printf("Free heap after recording: %d bytes\n", ESP.getFreeHeap());

    drawScreen("Transcribing...");
    String question = transcribeAudio();

    if (question.length() < 2 || 
        question.startsWith("No ") || 
        question.startsWith("Parse") || 
        question.startsWith("Connection") || 
        question.startsWith("Timeout")) {
      Serial.println("Transcription failed or empty");
      drawScreen("Couldn't hear.\nTry again.");
      delay(2000);
      showReadyScreen();
      return;
    }

    drawThinking();
    String answer = askGPT(question);  // Uses conversation history!
    
    response = wordWrap(answer, 32);
    Serial.println("Final display text:");
    Serial.println(response);
    
    // Show answer with persistent button hints - stays until user presses button
    drawAnswerWithButtons(response);
    
    Serial.printf("Free heap at end: %d bytes\n", ESP.getFreeHeap());
    Serial.println("\n*** CONTINUE TOPIC COMPLETE ***");
    Serial.println("Waiting for user input (A or B)...\n");
  }

  // ✨ BUTTON B (SHORT PRESS): NEW TOPIC (clears history)
  // Note: We need to detect short press vs long press (3 sec for WiFi reset)
  if (M5.BtnB.wasClicked() && !M5.BtnB.isPressed()) {
    Serial.println("\n*** BUTTON B: NEW TOPIC ***\n");
    clearHistory();  // Start fresh!
    
    // Check memory before starting
    if (ESP.getFreeHeap() < 40000) {
      drawScreen("Memory low!\nRestarting...");
      delay(2000);
      ESP.restart();
      return;
    }
    
    Serial.printf("Free heap before recording: %d bytes\n", ESP.getFreeHeap());
    
    if (!recordAudio()) {
      drawScreen("Mic error");
      delay(2000);
      showReadyScreen();
      return;
    }

    Serial.printf("Free heap after recording: %d bytes\n", ESP.getFreeHeap());

    drawScreen("Transcribing...");
    String question = transcribeAudio();

    if (question.length() < 2 || 
        question.startsWith("No ") || 
        question.startsWith("Parse") || 
        question.startsWith("Connection") || 
        question.startsWith("Timeout")) {
      Serial.println("Transcription failed or empty");
      drawScreen("Couldn't hear.\nTry again.");
      delay(2000);
      showReadyScreen();
      return;
    }

    drawThinking();
    String answer = askGPT(question);
    
    response = wordWrap(answer, 32);  // Increased from 25 to use more screen width
    Serial.println("Final display text:");
    Serial.println(response);
    
    // Show answer with persistent button hints - stays until user presses button
    drawAnswerWithButtons(response);
    
    Serial.printf("Free heap at end: %d bytes\n", ESP.getFreeHeap());
    Serial.println("\n*** NEW TOPIC COMPLETE ***");
    Serial.println("Waiting for user input (A or B)...\n");
  }

  // ✨ BUTTON B (SHORT PRESS): CONTINUE TOPIC (keeps history)
  // Note: We need to detect short press vs long press (3 sec for WiFi reset)
  if (M5.BtnB.wasClicked() && !M5.BtnB.isPressed()) {  // Only if not currently being held
    Serial.println("\n*** BUTTON B: CONTINUE TOPIC ***\n");
    
    if (historyCount == 0) {
      // No history yet - tell user to start with Button A
      drawScreen("Press A first to\nstart a topic!");
      delay(2000);
      showReadyScreen();
      return;
    }
    
    Serial.printf("Free heap before recording: %d bytes\n", ESP.getFreeHeap());
    
    if (!recordAudio()) {
      drawScreen("Mic error");
      delay(2000);
      showReadyScreen();
      return;
    }

    Serial.printf("Free heap after recording: %d bytes\n", ESP.getFreeHeap());

    drawScreen("Transcribing...");
    String question = transcribeAudio();

    if (question.length() < 2 || 
        question.startsWith("No ") || 
        question.startsWith("Parse") || 
        question.startsWith("Connection") || 
        question.startsWith("Timeout")) {
      Serial.println("Transcription failed or empty");
      drawScreen("Couldn't hear.\nTry again.");
      delay(2000);
      showReadyScreen();
      return;
    }

    drawScreen("Thinking...");
    String answer = askGPT(question);  // Uses conversation history!
    
    response = wordWrap(answer, 32);  // Increased from 25 to use more screen width
    Serial.println("Final display text:");
    Serial.println(response);
    
    // Show answer with persistent button hints - stays until user presses button
    drawAnswerWithButtons(response);
    
    Serial.printf("Free heap at end: %d bytes\n", ESP.getFreeHeap());
    Serial.println("\n*** CONTINUE TOPIC COMPLETE ***");
    Serial.println("Waiting for user input (A or B)...\n");
  }

  delay(20);
}
