/*
  Wildlife Detection Prototype (Arduino Uno)
  -------------------------------------------------
  Features:
  - 3 PIR motion sensors + 3 HC-SR04 ultrasonic sensors
  - Non-blocking scheduler using millis() (no delay)
  - State machine based detection logic
  - LED + buzzer alert output
  - Anti-spam alert prints
  - PIR warm-up period (~30 seconds)
*/

// ---------------------------
// Pin mapping
// ---------------------------
const int pirPins[3]  = {2, 3, 4};
const int trigPins[3] = {5, 7, 11};
const int echoPins[3] = {6, 10, 12};

const int ledPin = 8;
const int buzzerPin = 9;

// ---------------------------
// System constants
// ---------------------------
const long DANGER_THRESHOLD_CM = 50;
const unsigned long PIR_WARMUP_MS = 30000UL;

// ---------------------------
// Task scheduler
// ---------------------------
struct Task {
  unsigned long interval;
  unsigned long lastRun;
};

Task sensorTask = {300, 0};
Task stateTask  = {100, 0};
Task alertTask  = {100, 0};
Task serialTask = {500, 0};

bool shouldRun(Task &task) {
  unsigned long now = millis();
  if (now - task.lastRun >= task.interval) {
    task.lastRun = now;
    return true;
  }
  return false;
}

// ---------------------------
// Data arrays for all zones
// ---------------------------
int motionValues[3]   = {0, 0, 0};
long distanceValues[3] = {999, 999, 999};
bool zoneDanger[3]    = {false, false, false};

// Track which zone is being measured each sensor cycle
int currentUltrasonicZone = 0;

// ---------------------------
// State machine
// ---------------------------
enum SystemState {
  IDLE,
  SCANNING,
  CONFIRM_DANGER,
  ALERT,
  COOLDOWN
};

SystemState currentState = IDLE;

bool alertActive = false;          // Controls LED/buzzer output
bool alertPrinted = false;         // Anti-spam flag for "ALERT TRIGGERED!"
bool zoneAlertPrinted[3] = {false, false, false}; // Anti-spam per zone message

bool pirWarmupDone = false;
unsigned long startupTime = 0;

// ---------------------------
// Read one ultrasonic sensor
// Returns 999 if timeout/invalid
// ---------------------------
long readDistanceCM(int trigPin, int echoPin) {
  // Ensure a clean LOW pulse before trigger
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);

  // 10us HIGH trigger pulse
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  // Timeout around 25ms (~4m max distance)
  unsigned long duration = pulseIn(echoPin, HIGH, 25000UL);

  // Invalid reading or timeout
  if (duration == 0) {
    return 999;
  }

  // Convert microseconds to centimeters
  long distance = duration / 58;
  if (distance <= 0) {
    return 999;
  }
  return distance;
}

bool anyZoneDanger() {
  for (int i = 0; i < 3; i++) {
    if (zoneDanger[i]) {
      return true;
    }
  }
  return false;
}

bool allZonesSafe() {
  for (int i = 0; i < 3; i++) {
    if (zoneDanger[i]) {
      return false;
    }
  }
  return true;
}

// ---------------------------
// Task: Read sensors
// - Read all PIRs every run
// - Read one ultrasonic per run (round-robin)
// ---------------------------
void readSensorsTask() {
  // Read all PIR motion values
  for (int i = 0; i < 3; i++) {
    motionValues[i] = digitalRead(pirPins[i]);
  }

  // Read only one ultrasonic each cycle to avoid interference
  int z = currentUltrasonicZone;
  distanceValues[z] = readDistanceCM(trigPins[z], echoPins[z]);

  // Danger logic per updated zone
  zoneDanger[z] = (motionValues[z] == HIGH) && (distanceValues[z] < DANGER_THRESHOLD_CM);

  // Also refresh danger status for other zones from latest data
  for (int i = 0; i < 3; i++) {
    if (i != z) {
      zoneDanger[i] = (motionValues[i] == HIGH) && (distanceValues[i] < DANGER_THRESHOLD_CM);
    }
  }

  // Move to next zone for next ultrasonic reading
  currentUltrasonicZone = (currentUltrasonicZone + 1) % 3;
}

// ---------------------------
// Task: State machine update
// ---------------------------
void stateMachineTask() {
  // During warm-up, keep system in IDLE and outputs off
  if (!pirWarmupDone) {
    currentState = IDLE;
    alertActive = false;
    return;
  }

  switch (currentState) {
    case IDLE:
      currentState = SCANNING;
      break;

    case SCANNING:
      currentState = CONFIRM_DANGER;
      break;

    case CONFIRM_DANGER:
      if (anyZoneDanger()) {
        currentState = ALERT;
      } else {
        currentState = IDLE;
      }
      break;

    case ALERT:
      alertActive = true;
      if (!anyZoneDanger()) {
        currentState = COOLDOWN;
      }
      break;

    case COOLDOWN:
      // Stay here until all zones are safe, then reset alert flags
      if (allZonesSafe()) {
        alertActive = false;
        if (alertPrinted) {
          Serial.println("System reset. Ready for next alert.");
        }
        alertPrinted = false;
        for (int i = 0; i < 3; i++) {
          zoneAlertPrinted[i] = false;
        }
        currentState = IDLE;
      }
      break;
  }

  // If we just entered ALERT phase from confirm logic, ensure output ON
  if (currentState == ALERT) {
    alertActive = true;

    if (!alertPrinted) {
      Serial.println("ALERT TRIGGERED!");
      alertPrinted = true;
    }

    for (int i = 0; i < 3; i++) {
      if (zoneDanger[i] && !zoneAlertPrinted[i]) {
        Serial.print("Danger in Zone ");
        Serial.println(i + 1);
        zoneAlertPrinted[i] = true;
      }
    }
  }
}

// ---------------------------
// Task: Update outputs
// ---------------------------
void alertOutputTask() {
  digitalWrite(ledPin, alertActive ? HIGH : LOW);
  digitalWrite(buzzerPin, alertActive ? HIGH : LOW);
}

// ---------------------------
// Task: Serial monitor output
// ---------------------------
void serialPrintTask() {
  for (int i = 0; i < 3; i++) {
    Serial.print("Zone ");
    Serial.print(i + 1);
    Serial.print(" | Motion: ");
    Serial.print(motionValues[i] ? 1 : 0);
    Serial.print(" | Distance: ");
    Serial.print(distanceValues[i]);
    Serial.println(" cm");
  }
  Serial.println("-----------------------------");
}

void setup() {
  Serial.begin(9600);

  // Configure sensor pins
  for (int i = 0; i < 3; i++) {
    pinMode(pirPins[i], INPUT);
    pinMode(trigPins[i], OUTPUT);
    pinMode(echoPins[i], INPUT);
    digitalWrite(trigPins[i], LOW);
  }

  // Configure outputs
  pinMode(ledPin, OUTPUT);
  pinMode(buzzerPin, OUTPUT);
  digitalWrite(ledPin, LOW);
  digitalWrite(buzzerPin, LOW);

  startupTime = millis();
  Serial.println("Starting system...");
  Serial.println("PIR warm-up in progress (30 seconds)...");
}

void loop() {
  // PIR warm-up handling without delay()
  if (!pirWarmupDone && (millis() - startupTime >= PIR_WARMUP_MS)) {
    pirWarmupDone = true;
    Serial.println("PIR warm-up complete. System active.");
  }

  // Non-blocking task scheduler
  if (shouldRun(sensorTask)) {
    readSensorsTask();
  }

  if (shouldRun(stateTask)) {
    stateMachineTask();
  }

  if (shouldRun(alertTask)) {
    alertOutputTask();
  }

  if (shouldRun(serialTask)) {
    serialPrintTask();
  }
}
