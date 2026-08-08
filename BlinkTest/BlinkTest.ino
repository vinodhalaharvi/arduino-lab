void setup() { pinMode(LED_BUILTIN, OUTPUT); }
void loop() {
  for (int i = 0; i < 3; i++) { digitalWrite(LED_BUILTIN, HIGH); delay(120); digitalWrite(LED_BUILTIN, LOW); delay(120); }
  delay(900);
}
