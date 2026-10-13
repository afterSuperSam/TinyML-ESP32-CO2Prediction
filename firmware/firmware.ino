#include "application.h"

// 程序入口只启动和调度集成模块。
void setup() { Application::begin(); }
void loop() { Application::tick(); }
