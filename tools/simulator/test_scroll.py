#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LVGL滚动功能测试脚本
用于验证HCHO标签的滚动显示效果
"""

import time
import serial
import threading
import random
from dart_simulator import DartSensorSimulator


class ScrollTestSimulator(DartSensorSimulator):
    """专门用于测试滚动功能的模拟器"""
    
    def __init__(self, port: str, baudrate: int = 9600):
        super().__init__(port, baudrate)
        self.test_mode = "scroll"
        self.scroll_texts = [
            "HCHO: 0.025 mg/m3 (25 ppb) - Air Quality Monitor System - Real-time Formaldehyde Detection - Dart WZ-S-K Sensor",
            "HCHO: 0.037 mg/m3 (37 ppb) - Air Quality Monitor System - Real-time Formaldehyde Detection - Dart WZ-S-K Sensor",
            "HCHO: 0.049 mg/m3 (49 ppb) - Air Quality Monitor System - Real-time Formaldehyde Detection - Dart WZ-S-K Sensor",
            "HCHO: 0.061 mg/m3 (61 ppb) - Air Quality Monitor System - Real-time Formaldehyde Detection - Dart WZ-S-K Sensor",
            "HCHO: 0.073 mg/m3 (73 ppb) - Air Quality Monitor System - Real-time Formaldehyde Detection - Dart WZ-S-K Sensor",
            "HCHO: 0.085 mg/m3 (85 ppb) - Air Quality Monitor System - Real-time Formaldehyde Detection - Dart WZ-S-K Sensor",
            "HCHO: 0.097 mg/m3 (97 ppb) - Air Quality Monitor System - Real-time Formaldehyde Detection - Dart WZ-S-K Sensor",
        ]
        self.text_index = 0
        
    def generate_active_upload_data(self) -> bytes:
        """生成测试用的滚动数据"""
        # 循环使用不同的长文本
        test_text = self.scroll_texts[self.text_index % len(self.scroll_texts)]
        self.text_index += 1
        
        # 模拟不同的浓度值
        self.current_concentration = random.randint(20, 100)
        
        # 将浓度值分解为高低字节
        concentration_high = (self.current_concentration >> 8) & 0xFF
        concentration_low = self.current_concentration & 0xFF
        
        data = [
            0xFF,  # 起始位
            self.gas_name,  # 气体名称 (CH2O)
            self.unit,  # 单位 (Ppb)
            self.decimal_places,  # 小数位数
            concentration_high,  # 气体浓度高位
            concentration_low,   # 气体浓度低位
            self.full_scale_high,  # 满量程高位
            self.full_scale_low,   # 满量程低位
            0x00  # 校验值占位
        ]
        
        # 计算校验和
        data[8] = self.calculate_checksum(data)
        
        print(f"📤 滚动测试: 浓度={self.current_concentration} ppb, 文本长度={len(test_text)}")
        print(f"   文本: {test_text[:50]}...")
        
        return bytes(data)
    
    def active_upload_loop(self):
        """滚动测试循环"""
        while self.running and self.mode == "active":
            try:
                # 生成并发送数据
                data = self.generate_active_upload_data()
                if self.serial_conn and self.serial_conn.is_open:
                    self.serial_conn.write(data)
                
                # 等待2秒，给滚动效果更多时间
                time.sleep(2)
                
            except Exception as e:
                print(f"❌ 滚动测试错误: {e}")
                break


def test_scroll_functionality():
    """测试滚动功能"""
    print("🎯 LVGL滚动功能测试")
    print("=" * 50)
    print("这个测试将模拟发送长文本数据，以验证HCHO标签的滚动显示效果")
    print("请观察ESP32屏幕上的HCHO标签是否能够正常滚动显示")
    print("=" * 50)
    
    # 这里需要实际的串口端口
    port = input("请输入串口端口 (例如 COM3): ").strip()
    if not port:
        print("❌ 未提供串口端口")
        return
    
    try:
        # 创建滚动测试模拟器
        simulator = ScrollTestSimulator(port)
        simulator.start()
    except KeyboardInterrupt:
        print("\n⏹️  测试已停止")
    except Exception as e:
        print(f"❌ 测试失败: {e}")


def show_scroll_help():
    """显示滚动测试帮助信息"""
    print("""
🎯 LVGL滚动功能测试说明

1. 确保ESP32程序已编译并烧录到设备
2. 确保ESP32已连接到串口
3. 运行此测试脚本
4. 观察ESP32屏幕上的HCHO标签滚动效果

滚动测试特点：
- 发送长文本数据以触发滚动
- 每2秒更新一次数据
- 循环显示不同的测试文本
- 模拟真实的浓度变化

如果滚动不工作，请检查：
1. LVGL版本是否为9.2
2. 标签宽度设置是否正确
3. 文本长度是否足够长
4. 滚动模式是否正确设置
""")


if __name__ == "__main__":
    import sys
    
    if len(sys.argv) > 1 and sys.argv[1] == "--help":
        show_scroll_help()
    else:
        test_scroll_functionality()
