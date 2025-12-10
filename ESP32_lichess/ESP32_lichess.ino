
/*
  ESP32 Lichess Bridge - patched (castling payload + owner detection fixes)
  - Moves->ATmega formats:
    Normal:  <from><to>           e.g. e2e4
    Capture: <from><to><cap>      e.g. e5d6d5 (en-passant uses captured square)
    Castle:  <kfrom><kto><rfrom><rto>  e.g. e1g1h1f1 (8 chars)
  - Serial Monitor and Serial2 treated as ATmega-originated moves
  - Suppresses sending streamed moves that exactly match last ATmega-originated payload
  - Requires ArduinoJson (6.x)
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "network_stuff.h" // must define WIFI_SSID, WIFI_PASS, LICHESS_API_TOKEN, LICHESS_USER

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASS;
const char* lichessToken = LICHESS_API_TOKEN;

WiFiClientSecure httpsClient;
WiFiClientSecure streamClient;

unsigned long lastPlayingFetch = 0;
const unsigned long PLAYING_FETCH_INTERVAL = 4UL * 1000UL; // 20s

String currentGameId = "";
bool streaming = false;

// ownership tracking
String myColor = "";            // "white" or "black"
String lastProcessedMove = "";  // last UCI move handled

// Serial2 pins (adjust if needed)
const int SERIAL2_RX = 16; // ESP32 RX pin
const int SERIAL2_TX = 17; // ESP32 TX pin
const long SERIAL2_BAUD = 9600;

// If ATmega not connected, set false; still Serial Monitor will be used if you type
bool atmegaConnected = true; // set true when you wire the ATmega

// Track last move that came FROM the ATmega/Serial (payload format that would be sent to ATmega)
String lastMoveFromAtmega = "";

// Your lichess username (lowercased) - we'll initialize below
String myLichessId;

// minimal board representation for capture detection
// board_[rank][file], rank 0 = '1', file 0 = 'a'
char board_[8][8];

WiFiClientSecure client; // helper

// ---------- Helper: board utilities ----------
int fileCharToX(char f) { return (f - 'a'); }
int rankCharToY(char r) { return (r - '1'); }
String coordsXYtoSquare(int x, int y) {
  String s = "";
  s += char('a' + x);
  s += char('1' + y);
  return s;
}

void setBoardSquare(int x, int y, char piece) {
  if (x < 0 || x > 7 || y < 0 || y > 7) return;
  board_[y][x] = piece;
}
char getBoardSquare(int x, int y) {
  if (x < 0 || x > 7 || y < 0 || y > 7) return ' ';
  return board_[y][x];
}

void initBoardFromFEN() {
  // Standard start position
  const char* fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
  int x = 0, y = 7; // FEN starts rank8 -> rank1
  for (const char* p = fen; *p && *p != ' '; ++p) {
    char c = *p;
    if (c == '/') { y--; x = 0; }
    else if (c >= '1' && c <= '8') {
      int n = c - '0';
      for (int i = 0; i < n; ++i) board_[y][x++] = ' ';
    } else {
      board_[y][x++] = c;
    }
  }
}

// Build the "payload" that we send to ATmega for a simple UCI (non-capture) move
String buildPayloadForUciSimple(const String &uci) {
  if (uci.length() < 4) return "";
  String fromSq = uci.substring(0,2);
  String toSq   = uci.substring(2,4);
  return fromSq + toSq; // e2e4
}

// Apply UCI move to our board and determine capture square if any.
// Returns capture square string if capture happened, else empty string.
// Handles promotions, castling, en-passant best-effort.
String applyUciMoveToBoard(const String &uci) {
  if (uci.length() < 4) return "";

  int fromX = fileCharToX(uci[0]);
  int fromY = rankCharToY(uci[1]);
  int toX   = fileCharToX(uci[2]);
  int toY   = rankCharToY(uci[3]);

  char piece = getBoardSquare(fromX, fromY);
  char destPiece = getBoardSquare(toX, toY);

  bool isCapture = false;
  int capX = toX, capY = toY;

  // Detect en-passant: pawn moves diagonally into an empty square: captured pawn is on (toX, fromY)
  bool possiblePawn = (piece == 'P' || piece == 'p');
  if (possiblePawn && abs(toX - fromX) == 1 && toY != fromY && destPiece == ' ') {
    isCapture = true;
    capX = toX; capY = fromY;
  } else {
    if (destPiece != ' ') {
      isCapture = true;
      capX = toX; capY = toY;
    }
  }

  // Handle castling rook move (simple)
  if ((piece == 'K' || piece == 'k') && abs(toX - fromX) == 2) {
    if (toX == 6) { // king-side g-file
      int rookFromX = 7, rookToX = 5, rookY = fromY;
      char rookPiece = getBoardSquare(rookFromX, rookY);
      setBoardSquare(rookFromX, rookY, ' ');
      setBoardSquare(rookToX, rookY, rookPiece);
    } else if (toX == 2) { // queen-side c-file
      int rookFromX = 0, rookToX = 3, rookY = fromY;
      char rookPiece = getBoardSquare(rookFromX, rookY);
      setBoardSquare(rookFromX, rookY, ' ');
      setBoardSquare(rookToX, rookY, rookPiece);
    }
  }

  // Move piece
  setBoardSquare(fromX, fromY, ' ');

  // promotion
  if (uci.length() == 5) {
    char promo = uci[4];
    bool isWhite = (piece >= 'A' && piece <= 'Z');
    char pchar = isWhite ? char(toupper(promo)) : char(tolower(promo));
    setBoardSquare(toX, toY, pchar);
  } else {
    setBoardSquare(toX, toY, piece);
  }

  if (isCapture) return coordsXYtoSquare(capX, capY);
  return "";
}

// ---------- Networking helpers ----------
void connectWiFi() {
  Serial.print("Connecting WiFi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println(" connected.");
}

bool httpGetJson(const char* host, const String& path, String& outBody) {
  httpsClient.setInsecure();
  if (!httpsClient.connect(host, 443)) {
    Serial.println("HTTPS connect failed");
    return false;
  }
  httpsClient.printf("GET %s HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %s\r\nAccept: application/json\r\nConnection: close\r\n\r\n",
                    path.c_str(), host, lichessToken);

  String statusLine = httpsClient.readStringUntil('\n');
  if (!statusLine.startsWith("HTTP/1.1 200")) {
    Serial.println("HTTP GET failed: " + statusLine);
    while (httpsClient.connected()) { if (httpsClient.available()) httpsClient.read(); else delay(1); }
    httpsClient.stop();
    return false;
  }

  // Skip headers
  while (httpsClient.connected()) {
    String line = httpsClient.readStringUntil('\n');
    if (line == "\r" || line == "") break;
  }

  outBody = "";
  while (httpsClient.connected() || httpsClient.available()) {
    if (httpsClient.available()) outBody += (char)httpsClient.read();
    else delay(1);
  }
  httpsClient.stop();
  return true;
}

bool httpPostNoBody(const char* host, const String& path, String& outStatus) {
  WiFiClientSecure client;
  client.setInsecure();
  if (!client.connect(host, 443)) {
    outStatus = "connect fail";
    return false;
  }
  client.printf("POST %s HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                path.c_str(), host, lichessToken);

  String statusLine = client.readStringUntil('\n');
  outStatus = statusLine;
  while (client.connected()) { if (client.available()) client.read(); else delay(1); }
  client.stop();
  return statusLine.startsWith("HTTP/1.1 200") || statusLine.startsWith("HTTP/1.1 201") || statusLine.startsWith("HTTP/1.1 204");
}

// Parse /api/account/playing JSON and pick a game id (first)
String parsePlayingForGameId(const String& json) {
  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.println("parsePlayingForGameId JSON error: " + String(err.c_str()));
    return "";
  }
  if (!doc.containsKey("nowPlaying")) return "";
  JsonArray arr = doc["nowPlaying"].as<JsonArray>();
  if (arr.size() == 0) return "";
  for (JsonVariant v : arr) {
    if (v.containsKey("id")) return String((const char*)v["id"]);
    if (v.containsKey("gameId")) return String((const char*)v["gameId"]);
  }
  return "";
}

void fetchPlayingGames() {
  String body;
  Serial.println("Fetching /api/account/playing ...");
  if (!httpGetJson("lichess.org", "/api/account/playing", body)) {
    Serial.println("Failed to fetch playing");
    return;
  }
  String gid = parsePlayingForGameId(body);
  if (gid.length() > 0) {
    if (gid != currentGameId) {
      Serial.println("Selected game: " + gid);
      currentGameId = gid;
      if (streaming) {
        streamClient.stop();
        streaming = false;
      }
      delay(50);
      // reset board to start (we'll reconstruct from moves)
      initBoardFromFEN();
      startGameStream(currentGameId);
    }
  } else {
    Serial.println("No active game found.");
  }
}

void startGameStream(const String& gameId) {
  if (gameId.length() == 0) return;
  Serial.println("Starting stream for game: " + gameId);
  streamClient.setInsecure();
  if (!streamClient.connect("lichess.org", 443)) {
    Serial.println("Stream connect failed");
    return;
  }

  String path = "/api/board/game/stream/" + gameId;
  streamClient.printf("GET %s HTTP/1.1\r\nHost: lichess.org\r\nAuthorization: Bearer %s\r\nAccept: application/x-ndjson\r\nConnection: keep-alive\r\n\r\n",
                      path.c_str(), lichessToken);

  String status = streamClient.readStringUntil('\n');
  if (!status.startsWith("HTTP/1.1 200")) {
    Serial.println("Stream HTTP error: " + status);
    while (streamClient.connected()) { if (streamClient.available()) streamClient.read(); else delay(1); }
    streamClient.stop();
    return;
  }
  while (streamClient.connected()) {
    String lh = streamClient.readStringUntil('\n');
    if (lh == "\r" || lh == "") break;
  }
  streaming = true;
  Serial.println("Stream open.");
}

String readStreamLine() {
  String line = "";
  unsigned long start = millis();
  while (streamClient.connected()) {
    if (streamClient.available()) {
      char c = streamClient.read();
      line += c;
      if (c == '\n') break;
    } else {
      if (millis() - start > 200) break;
      delay(1);
    }
  }
  line.trim();
  return line;
}

// Process NDJSON stream lines; update board and send to ATmega (raw) unless suppressed
void handleStreamLine(const String& line) {
  if (line.length() == 0) return;
  if (line.length() < 5 || line[0] != '{') return;

  StaticJsonDocument<3072> doc; // increased a bit for nested objects
  DeserializationError err = deserializeJson(doc, line);
  if (err) return;

  const char* type = doc["type"];
  if (!type) return;
  String t = String(type);

  if (t == "gameFull") {
    // Determine white/black user ids
    String whiteUserId = "";
    String blackUserId = "";
    if (doc.containsKey("white")) {
      if (doc["white"].containsKey("id")) whiteUserId = String((const char*)doc["white"]["id"]);
      else if (doc["white"].containsKey("user") && doc["white"]["user"].containsKey("name"))
        whiteUserId = String((const char*)doc["white"]["user"]["name"]);
    }
    if (doc.containsKey("black")) {
      if (doc["black"].containsKey("id")) blackUserId = String((const char*)doc["black"]["id"]);
      else if (doc["black"].containsKey("user") && doc["black"]["user"].containsKey("name"))
        blackUserId = String((const char*)doc["black"]["user"]["name"]);
    }

    // lowercase comparison to avoid case mismatch
    whiteUserId.toLowerCase();
    blackUserId.toLowerCase();

    if (myLichessId.length() == 0) {
      // ensure our stored id is lowercased (in case setup didn't)
      myLichessId = String(LICHESS_USER);
      myLichessId.toLowerCase();
    }

    if (myLichessId == whiteUserId) myColor = "white";
    else if (myLichessId == blackUserId) myColor = "black";
    else myColor = "";

    Serial.println("White user: " + whiteUserId);
    Serial.println("Black user: " + blackUserId);
    Serial.println("Me (API id): " + myLichessId);
    Serial.println("Detected color: " + myColor);

    // Reconstruct initial board and apply moves from "initialFen"/"state" or "moves" if provided
    initBoardFromFEN();
    if (doc.containsKey("state") && doc["state"].containsKey("moves")) {
      String movesStr = String((const char*)doc["state"]["moves"]);
      movesStr.trim();
      if (movesStr.length()) {
        int start = 0;
        while (start < movesStr.length()) {
          int sp = movesStr.indexOf(' ', start);
          String m;
          if (sp == -1) { m = movesStr.substring(start); start = movesStr.length(); }
          else { m = movesStr.substring(start, sp); start = sp + 1; }
          m.trim();
          if (m.length() >= 4) {
            applyUciMoveToBoard(m);
            lastProcessedMove = m;
          }
        }
      }
    }
  }
  else if (t == "gameState") {
    const char* movesC = doc["moves"];
    if (!movesC) return;
    String movesStr = String(movesC); movesStr.trim();
    if (movesStr.length() == 0) return;

    // get latest move in UCI
    int lastSpace = movesStr.lastIndexOf(' ');
    String latestMove = (lastSpace == -1) ? movesStr : movesStr.substring(lastSpace + 1);
    latestMove.trim();
    if (latestMove.length() < 4) return; // sanity

    // avoid duplicates
    if (latestMove == lastProcessedMove) return;

    // Determine move number index
    int moveCount = 1;
    for (int i = 0; i < movesStr.length(); i++) if (movesStr[i] == ' ') moveCount++;
    bool isWhiteMove = (moveCount % 2 == 1); // white starts on move 1
    bool isMyMove = ((isWhiteMove && myColor == "white") ||
                     (!isWhiteMove && myColor == "black"));

    // compute capture square by applying to our board
    String captureSquare = applyUciMoveToBoard(latestMove);

    // Prepare payload to ATmega:
    String fromSq = latestMove.substring(0,2);
    String toSq   = latestMove.substring(2,4);

    // Detect castling by checking moved piece was a king and two-file move
    bool isKing = false;
    {
      int fx = fileCharToX(fromSq[0]), fy = rankCharToY(fromSq[1]);
      char pc = getBoardSquare(fx, fy);
      isKing = (toupper(pc) == 'K');
    }
    bool isCastle = isKing && abs(fileCharToX(toSq[0]) - fileCharToX(fromSq[0])) == 2;

    String payload;

        if (isCastle) {
        // produce 4 coordinate pairs: king-from, king-to, rook-from, rook-to (8 chars)
        if (toSq[0] == 'g') { 
            // King-side castle (h -> f)
            payload = fromSq + toSq + String("h") + String(fromSq[1]) + String("f") + String(fromSq[1]);
        } else {
            // Queen-side castle (a -> d)
            payload = fromSq + toSq + String("a") + String(fromSq[1]) + String("d") + String(fromSq[1]);
        }
    } else if (captureSquare.length()) {
        // Capture (normal or en-passant): from,to,capture
        payload = fromSq + toSq + captureSquare;
    } else {
        // Normal move: from,to
        payload = fromSq + toSq;
    }
    // update lastProcessedMove
    lastProcessedMove = latestMove;

    // Decide whether to send to ATmega: send unless it exactly matches lastMoveFromAtmega
    if (payload == lastMoveFromAtmega) {
      Serial.println((isMyMove ? "MY MOVE (suppressed send to ATmega): " : "OPPONENT MOVE (suppressed send to ATmega): ") + payload);
    } else {
      // send raw payload to ATmega (no prefixes)
      Serial.println((isMyMove ? "MY MOVE detected: " : "OPPONENT MOVE detected: ") + latestMove + (captureSquare.length() ? ("  capture at " + captureSquare) : ""));
      Serial.println(" -> send to ATmega: " + payload);
      if (atmegaConnected) Serial2.println(payload);
      else Serial.println("[ATmega not connected] would send: " + payload);
    }
  }
  else {
    // ignore other types
    return;
  }
}

// Send move to lichess
bool sendMoveToLichess(const String& gameId, const String& uci) {
  if (gameId.length() == 0 || uci.length() == 0) return false;
  String path = "/api/board/game/" + gameId + "/move/" + uci;
  String status;
  bool ok = httpPostNoBody("lichess.org", path, status);
  Serial.println("POST move " + uci + " -> " + status);
  return ok;
}

// normalize inputs like "e2e4", "e2 e4" into "e2e4"
String normalizeATmegaMove(String s) {
  String out = "";
  for (char c : s) {
    if (c == ' ' || c == '-' || c == ',') continue;
    out += c;
  }
  out.toLowerCase();
  if (out.length() == 4 || out.length() == 5) return out; // uci (4) or promotion (5)
  return "";
}

void setup() {
  Serial.begin(115200);
  delay(100);

  // store our lichess user (lowercased) for later color detection
  myLichessId = String(LICHESS_USER);
  myLichessId.toLowerCase();

  Serial2.begin(SERIAL2_BAUD, SERIAL_8N1, SERIAL2_RX, SERIAL2_TX);
  delay(100);

  initBoardFromFEN();
  connectWiFi();
  lastPlayingFetch = 0;
}

void loop() {
  // Periodically fetch playing games
  if (millis() - lastPlayingFetch > PLAYING_FETCH_INTERVAL) {
    lastPlayingFetch = millis();
    if (!streaming) fetchPlayingGames();
  }

  // Stream handling
  if (streaming && streamClient.connected()) {
    if (streamClient.available()) {
      String line = readStreamLine();
      if (line.length() > 0) handleStreamLine(line);
    }
  } else {
    if (streaming && !streamClient.connected()) {
      Serial.println("Stream disconnected.");
      streaming = false;
      currentGameId = "";
    }
  }

  // Read from Serial Monitor (USB) and treat as ATmega input
  while (Serial.available()) {
    String raw = Serial.readStringUntil('\n');
    raw.trim();
    if (raw.length() == 0) continue;
    if (raw.startsWith("ack:") || raw.startsWith("my:") || raw.startsWith("op:")) continue;

    String uci = normalizeATmegaMove(raw);
    if (uci.length() < 4) {
      Serial.println("Invalid move from Serial Monitor: " + raw);
      continue;
    }

    // Build the payload form we expect to later send to ATmega (so suppression will match)
    String candidatePayload = buildPayloadForUciSimple(uci);
    lastMoveFromAtmega = candidatePayload;
    Serial.println("Local move (Serial Monitor) -> post to Lichess: " + uci + "  payloadRecorded: " + candidatePayload);

    bool ok = sendMoveToLichess(currentGameId, uci);
    if (!ok) Serial.println("Failed to post move from Serial.");
  }

  // Read from Serial2 (ATmega) and treat input as local move or capture triple
  if (atmegaConnected) {
    while (Serial2.available()) {
      String raw = Serial2.readStringUntil('\n');
      raw.trim();
      if (raw.length() == 0) continue;
      if (raw.startsWith("ack:") || raw.startsWith("my:") || raw.startsWith("op:")) continue;

      String uciCandidate = normalizeATmegaMove(raw);

      // If ATmega sends a UCI (4/5 chars), post to lichess and remember payload in expected format
      if (uciCandidate.length() == 4 || uciCandidate.length() == 5) {
        // Prepare payload form for suppression (non-capture)
        String candidatePayload = buildPayloadForUciSimple(uciCandidate);
        lastMoveFromAtmega = candidatePayload;
        Serial.println("Local move (Serial2) -> post to Lichess: " + uciCandidate + "  payloadRecorded: " + candidatePayload);
        bool ok = sendMoveToLichess(currentGameId, uciCandidate);
        if (!ok) Serial.println("Failed to post move from ATmega.");
      } else {
        // ATmega sent capture-triple or castling payload already (6 or 8 chars) — don't post to lichess
        lastMoveFromAtmega = raw; // store exact payload so stream suppression works
        Serial.println("ATmega sent (raw payload): " + raw + " (not posted to Lichess)");
      }
    }
  }

  delay(10);
}

