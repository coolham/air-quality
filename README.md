# air-quality
Air Quality Monitor

## 环境

esp-idf v5.5

## LVGL

使用的LVGL版本是9.2.0

## 硬件接线说明（ESP32 模组）

[甲醛测试仪](./images/HCHO_monitor_1.jpg)

### 1. SSD1306 OLED 显示屏
- 通信方式：I2C
- SCL（时钟）：连接 ESP32S3 的 GPIO 17
- SDA（数据）：连接 ESP32S3 的 GPIO 16
- VCC：3.3V
- GND：GND

### 2. DART 甲醛传感器（UART1）
- TX（传感器输出）：连接 ESP32S3 的 GPIO 19（UART1 RX）
- RX（传感器输入）：连接 ESP32S3 的 GPIO 18（UART1 TX）
- VCC：3.3V 或 5V（根据传感器规格）
- GND：GND

### 3. 其他 UART 传感器（UART2，举例）
- TX（传感器输出）：连接 ESP32S3 的 GPIO 21（UART2 RX）
- RX（传感器输入）：连接 ESP32S3 的 GPIO 20（UART2 TX）
- VCC：3.3V 或 5V
- GND：GND

### 4. 供电
- ESP32S3、显示屏、传感器共用 3.3V 或 5V 电源，GND 共地。

### 5. 典型接线图
```
SSD1306 (I2C)      ESP32S3
-----------------------------
SCL   <-------->   GPIO17
SDA   <-------->   GPIO16
VCC   <-------->   3.3V
GND   <-------->   GND

DART传感器 (UART1) ESP32S3
-----------------------------
TX    <-------->   GPIO19 (RX1)
RX    <-------->   GPIO18 (TX1)
VCC   <-------->   3.3V/5V
GND   <-------->   GND

其他传感器 (UART2) ESP32S3
-----------------------------
TX    <-------->   GPIO21 (RX2)
RX    <-------->   GPIO20 (TX2)
VCC   <-------->   3.3V/5V
GND   <-------->   GND
```

> 注意：如有引脚冲突或需自定义，请在 `main/dart_sensor.c` 和相关配置中同步修改。


## 甲醛

HCHO 和 CH2O 都表示甲醛的分子式，它们是等价的。在有机化学中，为了更好地展示分子中原子间的连接方式，通常会使用 HCHO，因为它可以直观地看出两个氢原子分别连接在一个碳原子上，然后这个碳原子再通过双键连接到一个氧原子上。而 CH2O 更多地用于表示分子中各元素的原子数量比。


