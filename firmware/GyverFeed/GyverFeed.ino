/*
  Скетч к проекту "Автокормушка"
  - Страница проекта (схемы, описания): https://alexgyver.ru/gyverfeed/
  - Исходники на GitHub: https://github.com/AlexGyver/GyverFeed/
  Проблемы с загрузкой? Читай гайд для новичков: https://alexgyver.ru/arduino-first/
  Нравится, как написан код? Поддержи автора! https://alexgyver.ru/support_alex/
  Автор: AlexGyver, AlexGyver Technologies, 2019
  https://www.youtube.com/c/alexgyvershow
  https://github.com/AlexGyver
  https://AlexGyver.ru/
  alex@alexgyver.ru
*/

/*
  Клик - внеочередная кормёжка. Таймер кормёжки сброшен
  Удержание - задаём размер порции
*/
#define FEED_PERIOD 1     // период кормёжки В ЧАСАХ
#define FEED_SPEED 10     // скорость вращения шнека (условные единицы)
#define MIN_SPEED 150     // мин. скорость мотора (уменьшает время разгона)
#define INVERSE_MOTOR 0   // 0/1 инвертировать мотор (если лень перепаивать провода)
#define INVERSE_BUTTON 1  // 0 - норм. открытая, 1 - норм. замкнутая кнопка
#define CLEAR_TIME 300    // время заднего хода, мс (устранение засора)
#define WAIT_MODE 1       // 0 - сон, 1 - просто задержка

// пины
#define BTN_PIN 2
#define ENC_DO 3
#define ENC_VCC 4
#define MOTOR_DIR 8
#define MOTOR_PWM 9

int realPeriod;
int sleedAmount;
int feedAmount = 100;
int encCounter = 0;
int curEncSpeed = 0;
volatile bool isrState = false;
int motorSpeed = MIN_SPEED;
bool motorDirection = false;  // 0 - вперёд, 1 -  назад

#if (WAIT_MODE == 0)
#include "LowPower.h"
#endif
#include <EEPROM.h>

void setup() {
  if (EEPROM.read(1000) != 50) {  // первый запуск
#if (WAIT_MODE == 0)
    realPeriod = calibrateWDT();  // калибровка пёса, находим реальные 8 секунд
#endif
    EEPROM.write(1000, 50);
    EEPROM.put(0, feedAmount);
    EEPROM.put(2, realPeriod);
  }

  EEPROM.get(0, feedAmount);
#if (WAIT_MODE == 0)
  EEPROM.get(2, realPeriod);
  sleedAmount = (float)FEED_PERIOD * 3600 / realPeriod * 1000;  // находим сколько раз спать по 8 секунд!
#endif

  pinMode(ENC_VCC, OUTPUT);
  pinMode(MOTOR_DIR, OUTPUT);
  pinMode(MOTOR_PWM, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);
  digitalWrite(ENC_VCC, HIGH);

  // Пины D9 и D10 - 7.8 кГц чтобы не было ПИСКА
  TCCR1A = 0b00000001; // 8bit
  TCCR1B = 0b00001010; // x8 fast pwm
  attachInterrupt(0, isrHandler, INVERSE_BUTTON ? RISING : FALLING);
  delay(300);   // ждём когда устаканится прерывание
  isrState = false;
}

void loop() {
  if (isrState) {
    delay(1000);                                        // тупим секунду
    if (digitalRead(BTN_PIN) ^ INVERSE_BUTTON) {        // если кнопка отпущена (клик)
      isrState = false;                                 //
    } else {                                            // если кнопка нажата
      digitalWrite(ENC_VCC, HIGH);                      // включаем энкодер
      while (!digitalRead(BTN_PIN) ^ INVERSE_BUTTON) {  // пока кнопка нажата
        feedRoutine();                                  // крутим вертим
      }
      runMotor(0, 0);                                   // стоп машина
      feedAmount = encCounter;                          // запомнили сколько накрутили
      EEPROM.put(0, feedAmount);                        // записали в епром
      encCounter = 0;                                   // сброс счётчика
      motorSpeed = MIN_SPEED;                           // скорость в мин
      digitalWrite(ENC_VCC, LOW);                       // выключаем энкодер
    }
  }

  if (!isrState) {                      // если проснулись по таймеру или кликнули по кнопке
    digitalWrite(ENC_VCC, HIGH);        // включаем энкодер
    while (encCounter < feedAmount) {   // пока не повернёмся нужное количество раз
      feedRoutine();
    }
    encCounter = 0;               // сброс счётчика
    motorSpeed = MIN_SPEED;       // скорость в мин
    runMotor(0, 0);               // стоп машина
    digitalWrite(ENC_VCC, LOW);   // выключаем энкодер
  }
  isrState = false;

#if (WAIT_MODE == 0)
  // ЗДОРОВЫЙ СОН
  for (int i = 0; i < sleedAmount; i++) {   // спим по 8 секунд
    LowPower.powerDown(SLEEP_8S, ADC_OFF, BOD_OFF);
    if (isrState) break;        // если было нажатие - просыпаемся и уходим в начало loop
  }
#else
  // ИЛИ ОЖИДАНИЕ
  uint32_t tmr = millis();
  uint32_t waitPrd = (uint32_t)FEED_PERIOD * 3600 * 1000;
  while (millis() - tmr < waitPrd) {
    if (isrState) break;        // если было нажатие - выходим и уходим в начало loop
  }
#endif
}

void feedRoutine() {
  encTick();                                    // опрос энкодера
  static uint32_t tmr;
  if (millis() - tmr >= 30) {                   // управление мотором
    tmr = millis();

    if (curEncSpeed < FEED_SPEED) motorSpeed += 2;  // прибавить газу
    if (curEncSpeed > FEED_SPEED) motorSpeed -= 2;  // тормозить
    motorSpeed = constrain(motorSpeed, 0, 255);     // ограничить 0-255

    static bool stuckFlag = false;
    static uint32_t stuckTimeout;
    if (motorSpeed == 255 && curEncSpeed == 0) {    // если мотор жарит на полную, но скорость 0
      if (millis() - stuckTimeout > 1000) {         // и это продолжается больше секунды
        runMotor(1, 255);                           // полный назад! Прочищаем
        uint32_t stuckTmr = millis();
        while (millis() - stuckTmr < CLEAR_TIME) {  // в течение CLEAR_TIME
          encTick();                                // опрос энкодера
        }
        //delay(CLEAR_TIME);
        motorSpeed = MIN_SPEED;                     // сбросили скорость (чтобы не дёргать)
        stuckTimeout = millis();                    // сброс таймаута
      }
    } else {                                        // если всё ок
      stuckTimeout = millis();                      // сброс таймаута
    }
    runMotor(0, motorSpeed);                        // крутим мотор
  }
}

void encTick() {
  static bool lastState;
  bool curState = digitalRead(ENC_DO);    // опрос
  if (lastState != curState) {            // словили изменение
    lastState = curState;
    if (curState) {                       // по фронту
      encCounter += (motorDirection ? -1 : 1);  // запомнили поворот
    }
  }
  static uint32_t tmr;
  if (millis() - tmr >= 300) {            // таймер для расчёта скорости
    tmr = millis();
    static int lastPos = 0;
    curEncSpeed = encCounter - lastPos;   // собственно скорость
    lastPos = encCounter;
  }
}

void runMotor(bool dir, byte speed) {
  motorDirection = dir;
  dir = dir ^ INVERSE_MOTOR;            // инверсия если надо
  digitalWrite(MOTOR_DIR, dir);         // направление
  analogWrite(MOTOR_PWM, (dir) ? (255 - speed) : (speed));  // скорость
}

void isrHandler() {
  if (!isrState) isrState = true;       // поднять флаг прерывания  если он сброшен
}

int calibrateWDT() {
  WDTCSR |= (1 << WDCE) | (1 << WDE);   // разрешаем вмешательство
  WDTCSR = 0x47;                        // таймаут ~ 2 c
  asm ("wdr");                          // сбросили пса
  uint16_t startTime = millis();        // засекли время
  while (!(WDTCSR & (1 << WDIF)));      // ждем таймаута
  uint16_t ms = millis() - startTime;
  WDTCSR |= (1 << WDCE) | (1 << WDE);   // разрешаем вмешательство
  WDTCSR = 0;                           // выкл wdt
  return ms * 4;                        // ms ~ 2000. Ищем реальные 8 секунд
}
