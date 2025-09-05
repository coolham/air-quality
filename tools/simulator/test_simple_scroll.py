#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
简单的LVGL滚动功能测试脚本
只测试HCHO标签的滚动显示效果
"""

import time
import serial
import threading
import random
from dart_simulator import DartSensorSimulator


class SimpleScrollTestSimulator(DartSensorSimulator):
    """简单的滚动测试模拟器"""
    
    def __init__(self, port: str, baudrate: int = 9600):
        super().__init__(port, baudrate)
        print("🎯 简单滚动测试模式")
        print("📝 将发送长文本数据以触发HCHO标签滚动")
        print("👀 请观察ESP32屏幕上的滚动效果")
    
    def generate_active_upload_data(self) -> bytes:
        """生成简单的滚动测试数据"""
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
        
        # 计算对应的mg/m3值 (粗略转换: 1 ppb ≈ 1.23 ug/m3 for CH2O)
        mg_per_m3 = self.current_concentration * 1.23 / 1000.0
        
        print(f"📤 滚动测试: {mg_per_m3:.3f} mg/m3 ({self.current_concentration} ppb)")
        
        return bytes(data)
    
    def active_upload_loop(self):
        """滚动测试循环"""
        while self.running and self.mode == "active":
            try:
                # 生成并发送数据
                data = self.generate_active_upload_data()
                if self.serial_conn and self.serial_conn.is_open:
                    self.serial_conn.write(data)
                
                # 等待3秒，给滚动效果更多时间观察
                time.sleep(3)
                
            except Exception as e:
                print(f"❌ 滚动测试错误: {e}")
                break


def test_simple_scroll():
    """简单的滚动功能测试"""
    print("🎯 简单滚动功能测试")
    print("=" * 40)
    print("这个测试将发送数据以验证HCHO标签的滚动显示")
    print("请观察ESP32屏幕上的HCHO标签是否能够滚动")
    print("=" * 40)
    
    # 获取串口端口
    port = input("请输入串口端口 (例如 COM3): ").strip()
    if not port:
        print("❌ 未提供串口端口")
        return
    
    try:
        # 创建滚动测试模拟器
        simulator = SimpleScrollTestSimulator(port)
        simulator.start()
    except KeyboardInterrupt:
        print("\n⏹️  测试已停止")
    except Exception as e:
        print(f"❌ 测试失败: {e}")


if __name__ == "__main__":
    test_simple_scroll()

