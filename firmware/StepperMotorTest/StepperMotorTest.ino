#include <Stepper.h>

Stepper motor(200, 15, 16, 17, 18);  // 200 steps/rev, IN1-IN4

void setup() {
  motor.setSpeed(100);  // 30 RPM
}

void loop() {
  motor.step(200);     // 1 turn forward
  delay(1000);
  motor.step(-200);    // 1 turn backward
  delay(1000);
}