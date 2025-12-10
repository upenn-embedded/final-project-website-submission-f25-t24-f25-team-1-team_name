// Example: ESP32 Feather UART communication with ATmega
// Sends a "HELLO AtMega" string and prints any received reply

// Use UART1 → Serial1
// Pins for Adafruit ESP32 Feather (original): RX=32, TX=33


void setup() {
  // USB serial
  Serial.begin(115200);
  while (!Serial) { }

  Serial.println("ESP32 Feather UART Test");

  // Init UART1
  Serial2.begin(9600, SERIAL_8N1, 16, 17);  
  delay(100);

  
}

void loop() {
  
  // Send message to ATmega
  Serial2.println("HELLO AtMega");
  Serial.println("Sent to ATmega: HELLO AtMega");

  Serial.println("Waiting for reply...");
  // If ATmega sends something, forward to USB Serial Monitor
  if (Serial2.available()) {
    String msg = Serial2.readStringUntil('\n');
    Serial.print("Received from ATmega: ");
    Serial.println(msg);
  }

  delay(1000);
}
