/**
 * Fanatec H档 + 序列档 + PC模式 三合一排挡（带防抖，修复Joystick对象问题）
 * 基于 Arduino Pro Micro (3.3V/16MHz)
 * 
 * 霍尔开关：低电平有效（磁铁靠近输出LOW）
 * 按钮：低电平有效（按下LOW）
 * 模式开关：双刀三掷，两个引脚内部上拉，组合识别三种模式：
 *   - A2=LOW, A3=LOW   -> 基座H档模式
 *   - A2=LOW, A3=HIGH  -> 基座序列档模式
 *   - A2=HIGH, A3=HIGH -> PC模式
 * 
 * 依赖库：Joystick by MHeironimus
 */

#include <Joystick.h>

// 手动创建 Joystick 对象（因为库的类名是 Joystick_）
Joystick_ Joystick;

// ======================= 引脚定义 =======================
// 霍尔输入（低电平有效）
const int HALL_1 = 2;
const int HALL_2 = 3;
const int HALL_3 = 4;
const int HALL_4 = 5;
const int HALL_5 = 6;
const int HALL_6 = 7;
const int hallPins[7] = {0, HALL_1, HALL_2, HALL_3, HALL_4, HALL_5, HALL_6};

// 按钮输入
const int BTN_PIN = 8;

// 模式选择（双刀三掷）
const int MODE_A = A2;   // 对应数字20
const int MODE_B = A3;   // 对应数字21

// SEQ输出（控制基座H档/序列档）
const int SEQ_PIN = 9;

// X轴下拉引脚（低电平有效）
const int X_7   = 10;   // 0Ω
const int X_56  = 14;   // 3.3kΩ
const int X_34  = 15;   // 10kΩ
const int X_12  = 16;   // 30kΩ

// Y轴下拉引脚（低电平有效）
const int Y_REAR = A0;  // 0Ω
const int Y_MID  = A1;  // 10kΩ

// ======================= 去抖参数 =======================
const unsigned long DEBOUNCE_DELAY = 20;  // 20ms 去抖延时

// ======================= 枚举定义 =======================
enum Gear {
  GEAR_NEUTRAL,
  GEAR_1, GEAR_2, GEAR_3, GEAR_4, GEAR_5, GEAR_6,
  GEAR_R, GEAR_7
};

enum Mode {
  MODE_FANATEC_H,
  MODE_FANATEC_SEQ,
  MODE_PC
};

// ======================= 全局变量 =======================
bool stableHallState[7] = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};  // 稳定后的霍尔状态
unsigned long lastDebounceTime[7] = {0, 0, 0, 0, 0, 0, 0};             // 上次抖动开始时间

int currentSlot = 0;       // 当前挂入的物理槽位（1-6），0为空挡
bool extendedMode = false;  // 当前是否处于扩展模式（R或7）
Mode currentMode;           // 当前工作模式

Gear lastSentGear = GEAR_NEUTRAL;  // 上次发送给PC的档位（用于避免重复更新）

// ======================= 初始化 =======================
void setup() {
  // 霍尔输入引脚（启用内部上拉）
  for (int i = 1; i <= 6; i++) {
    pinMode(hallPins[i], INPUT_PULLUP);
  }
  // 按钮输入
  pinMode(BTN_PIN, INPUT_PULLUP);
  
  // 模式选择引脚（启用内部上拉）
  pinMode(MODE_A, INPUT_PULLUP);
  pinMode(MODE_B, INPUT_PULLUP);
  
  // SEQ输出
  pinMode(SEQ_PIN, OUTPUT);
  
  // 所有X/Y下拉引脚初始化为输入（高阻）
  pinMode(X_7,   INPUT);
  pinMode(X_56,  INPUT);
  pinMode(X_34,  INPUT);
  pinMode(X_12,  INPUT);
  pinMode(Y_REAR, INPUT);
  pinMode(Y_MID,  INPUT);
  
  // PC模式下初始化Joystick
  Joystick.begin();
  // 不需要手动设置按钮范围，默认支持足够按钮
  
  // 可选：串口调试（如需调试，取消注释）
  // Serial.begin(9600);
}

// ======================= 主循环 =======================
void loop() {
  // 1. 读取模式
  currentMode = getMode();
  
  // 2. 根据模式执行不同操作
  switch (currentMode) {
    case MODE_FANATEC_H:
      runHPatternMode();
      break;
    case MODE_FANATEC_SEQ:
      runSeqMode();
      break;
    case MODE_PC:
      runPCMode();
      break;
  }
  
  delay(5);  // 基础循环延时
}

// ======================= 模式检测 =======================
Mode getMode() {
  bool a = digitalRead(MODE_A);
  bool b = digitalRead(MODE_B);
  if (!a && !b) return MODE_FANATEC_H;
  if (!a &&  b) return MODE_FANATEC_SEQ;
  return MODE_PC;  // a=HIGH, b=HIGH
}

// ======================= 带防抖的霍尔状态读取 =======================
void readHallWithDebounce() {
  for (int i = 1; i <= 6; i++) {
    int reading = digitalRead(hallPins[i]);   // 当前读取值
    if (reading != stableHallState[i]) {
      // 状态变化，记录变化开始时间
      if (lastDebounceTime[i] == 0) {
        lastDebounceTime[i] = millis();
      }
      // 如果稳定时间超过DEBOUNCE_DELAY，则更新稳定状态
      if ((millis() - lastDebounceTime[i]) > DEBOUNCE_DELAY) {
        stableHallState[i] = reading;
        lastDebounceTime[i] = 0;  // 重置计时
      }
    } else {
      // 状态未变，重置计时
      lastDebounceTime[i] = 0;
    }
  }
}

// ======================= 档位状态更新（H档和PC模式共用） =======================
void updateGearState() {
  // 先读取去抖后的霍尔状态
  readHallWithDebounce();
  
  // 读取按钮状态（按下为true）
  bool btnPressed = (digitalRead(BTN_PIN) == LOW);
  
  // 检测挂入事件（从 HIGH 变为 LOW）
  static bool lastHallState[7] = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};
  for (int i = 1; i <= 6; i++) {
    if (lastHallState[i] == HIGH && stableHallState[i] == LOW) {
      currentSlot = i;
      if (btnPressed && (i == 1 || i == 5)) {
        extendedMode = true;   // 按下按钮且是槽位1或5，进入扩展模式
      } else {
        extendedMode = false;
      }
    }
    // 检测离开事件（从 LOW 变为 HIGH）
    if (lastHallState[i] == LOW && stableHallState[i] == HIGH) {
      if (currentSlot == i) {
        currentSlot = 0;        // 空挡
        extendedMode = false;
      }
    }
  }
  
  // 更新上一次状态
  for (int i = 1; i <= 6; i++) {
    lastHallState[i] = stableHallState[i];
  }
}

// ======================= 根据状态获取最终档位 =======================
Gear getCurrentGearFromState() {
  if (currentSlot == 0) return GEAR_NEUTRAL;
  if (extendedMode) {
    if (currentSlot == 1) return GEAR_R;
    if (currentSlot == 5) return GEAR_7;
    // 理论上不会到这里，但为了安全
    return static_cast<Gear>(currentSlot);
  } else {
    return static_cast<Gear>(currentSlot);
  }
}

// ======================= 设置X/Y模拟输出（H档模式专用） =======================
void setAnalogOutputs() {
  Gear g = getCurrentGearFromState();
  
  // 先将所有X/Y下拉引脚设为输入（高阻）
  pinMode(X_7,   INPUT);
  pinMode(X_56,  INPUT);
  pinMode(X_34,  INPUT);
  pinMode(X_12,  INPUT);
  pinMode(Y_REAR, INPUT);
  pinMode(Y_MID,  INPUT);
  
  // 设置X轴
  switch (g) {
    case GEAR_R:
      // X：所有引脚输入 -> 3.3V
      break;
    case GEAR_1:
    case GEAR_2:
      pinMode(X_12, OUTPUT);
      digitalWrite(X_12, LOW);
      break;
    case GEAR_3:
    case GEAR_4:
      pinMode(X_34, OUTPUT);
      digitalWrite(X_34, LOW);
      break;
    case GEAR_5:
    case GEAR_6:
      pinMode(X_56, OUTPUT);
      digitalWrite(X_56, LOW);
      break;
    case GEAR_7:
      pinMode(X_7, OUTPUT);
      digitalWrite(X_7, LOW);
      break;
    case GEAR_NEUTRAL:
      // 空挡时X回到中位（3/4档电压）
      pinMode(X_34, OUTPUT);
      digitalWrite(X_34, LOW);
      break;
    default: break;
  }
  
  // 设置Y轴
  switch (g) {
    case GEAR_R:
    case GEAR_1:
    case GEAR_3:
    case GEAR_5:
    case GEAR_7:
      // 前档：所有Y引脚输入 -> 3.3V
      break;
    case GEAR_2:
    case GEAR_4:
    case GEAR_6:
      pinMode(Y_REAR, OUTPUT);
      digitalWrite(Y_REAR, LOW);
      break;
    case GEAR_NEUTRAL:
      pinMode(Y_MID, OUTPUT);
      digitalWrite(Y_MID, LOW);
      break;
    default: break;
  }
}

// ======================= H档模式 =======================
void runHPatternMode() {
  digitalWrite(SEQ_PIN, LOW);   // H档模式
  updateGearState();            // 更新档位状态
  setAnalogOutputs();           // 输出模拟电压
}

// ======================= 序列档模式 =======================
void runSeqMode() {
  digitalWrite(SEQ_PIN, HIGH);  // 序列档模式
  
  // 禁用所有电阻网络引脚（设为输入，避免干扰）
  pinMode(X_7,   INPUT);
  pinMode(X_56,  INPUT);
  pinMode(X_34,  INPUT);
  pinMode(X_12,  INPUT);
  pinMode(Y_REAR, INPUT);
  pinMode(Y_MID,  INPUT);
  
  // 读取霍尔，检测升档/降档动作（假设霍尔1对应升档/前，霍尔2对应降档/后）
  // 这里使用霍尔1和霍尔2（可根据实际排挡结构调整）
  bool forward = (digitalRead(HALL_1) == LOW);   // 升档
  bool backward = (digitalRead(HALL_2) == LOW);  // 降档
  
  // 模拟换挡信号：将对应引脚短暂拉低再释放
  if (forward) {
    pinMode(X_7, OUTPUT);
    digitalWrite(X_7, LOW);
    delay(50);
    pinMode(X_7, INPUT);
  }
  if (backward) {
    pinMode(Y_REAR, OUTPUT);
    digitalWrite(Y_REAR, LOW);
    delay(50);
    pinMode(Y_REAR, INPUT);
  }
}

// ======================= PC模式 =======================
void runPCMode() {
  // 更新档位状态（含防抖）
  updateGearState();
  Gear currentGear = getCurrentGearFromState();
  
  // 仅当档位发生变化时，才更新USB按钮（避免频繁刷新导致闪烁）
  if (currentGear != lastSentGear) {
    // 先释放所有按钮（假设按钮0-7用于档位）
    for (int i = 0; i < 8; i++) {
      Joystick.setButton(i, 0);
    }
    // 再按下当前档位对应的按钮
    switch (currentGear) {
      case GEAR_1:   Joystick.setButton(0, 1); break;
      case GEAR_2:   Joystick.setButton(1, 1); break;
      case GEAR_3:   Joystick.setButton(2, 1); break;
      case GEAR_4:   Joystick.setButton(3, 1); break;
      case GEAR_5:   Joystick.setButton(4, 1); break;
      case GEAR_6:   Joystick.setButton(5, 1); break;
      case GEAR_R:   Joystick.setButton(6, 1); break;
      case GEAR_7:   Joystick.setButton(7, 1); break;
      default: break;  // 空挡：所有按钮已释放
    }
    lastSentGear = currentGear;
    
    // 可选：串口打印调试信息
    // Serial.print("Gear changed to: ");
    // Serial.println(currentGear);
  }
}