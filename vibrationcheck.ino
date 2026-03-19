#define VIBRATION_PIN 4

void setup() {
  Serial.begin(115200);
  pinMode(VIBRATION_PIN, INPUT);
  Serial.println("SW420 Vibration Sensor Ready");
}

void loop() {
  int val = digitalRead(VIBRATION_PIN);

  if (val == HIGH) {
    Serial.println("Vibration detected!");
  } else {
    Serial.println("No vibration");
  }

  delay(200);
}