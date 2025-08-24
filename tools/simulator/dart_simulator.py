#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Dart WZ-S-K 甲醛传感器模拟器
支持主动上传和问答两种工作模式
"""

import serial
import time
import threading
import random
import argparse
import sys
from typing import List, Optional


class DartSensorSimulator:
    """Dart传感器模拟器"""
    
    def __init__(self, port: str, baudrate: int = 9600):
        """
        初始化模拟器
        
        Args:
            port: 串口端口
            baudrate: 波特率
        """
        self.port = port
        self.baudrate = baudrate
        self.mode = "active"  # 默认主动上传模式
        self.serial_conn = None
        self.running = False
        self.thread = None
        
        # 传感器参数
        self.gas_name = 0x17  # CH2O
        self.unit = 0x04      # Ppb
        self.decimal_places = 0x00
        self.full_scale_high = 0x07
        self.full_scale_low = 0xD0  # 2000 ppb
        
        # 当前浓度值 (ppb)
        self.current_concentration = 25  # 初始值25 ppb
        
        print(f"🎯 Dart传感器模拟器初始化完成")
        print(f"📡 串口: {port}")
        print(f"⚙️  波特率: {baudrate}")
        print(f"🔧 工作模式: 主动上传 (可通过串口命令切换)")
        print(f"🌡️  初始浓度: {self.current_concentration} ppb")
        print("-" * 50)
    
    def calculate_checksum(self, data: List[int]) -> int:
        """
        计算校验和
        求和校验：取发送、接收协议的1到7字节的和取反+1
        """
        if len(data) < 8:
            return 0
        
        # 计算第1到第7字节的和
        checksum = sum(data[1:8])
        # 取反+1
        checksum = (~checksum + 1) & 0xFF
        return checksum
    
    def generate_active_upload_data(self) -> bytes:
        """
        生成主动上传数据包
        """
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
        
        print(f"📤 主动上传: 浓度={self.current_concentration} ppb, "
              f"数据包={' '.join([f'0x{x:02X}' for x in data])}")
        
        return bytes(data)
    
    def generate_qa_response(self, concentration_ppb: int) -> bytes:
        """
        生成问答模式响应数据包
        """
        # 转换为ug/m3 (粗略转换: 1 ppb ≈ 1.23 ug/m3 for CH2O)
        concentration_ugm3 = int(concentration_ppb * 1.23)
        
        # 分解为高低字节
        ugm3_high = (concentration_ugm3 >> 8) & 0xFF
        ugm3_low = concentration_ugm3 & 0xFF
        ppb_high = (concentration_ppb >> 8) & 0xFF
        ppb_low = concentration_ppb & 0xFF
        
        data = [
            0xFF,  # 起始位
            0x86,  # 命令
            ugm3_high,   # 气体浓度高位(ug/m3)
            ugm3_low,    # 气体浓度低位(ug/m3)
            0x00,  # 保留
            0x00,  # 保留
            ppb_high,    # 气体浓度高位(ppb)
            ppb_low,     # 气体浓度低位(ppb)
            0x00  # 校验值占位
        ]
        
        # 计算校验和
        data[8] = self.calculate_checksum(data)
        
        print(f"📤 问答响应: {concentration_ppb} ppb ({concentration_ugm3} ug/m3), "
              f"数据包={' '.join([f'0x{x:02X}' for x in data])}")
        
        return bytes(data)
    
    def switch_to_qa_mode(self) -> bytes:
        """切换到问答模式"""
        data = [0xFF, 0x01, 0x78, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00]
        data[8] = self.calculate_checksum(data)
        print(f"🔄 切换到问答模式: {' '.join([f'0x{x:02X}' for x in data])}")
        return bytes(data)
    
    def switch_to_active_mode(self) -> bytes:
        """切换到主动上传模式"""
        data = [0xFF, 0x01, 0x78, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00]
        data[8] = self.calculate_checksum(data)
        print(f"🔄 切换到主动上传模式: {' '.join([f'0x{x:02X}' for x in data])}")
        return bytes(data)
    
    def process_command(self, data: bytes) -> Optional[bytes]:
        """
        处理接收到的命令
        
        Args:
            data: 接收到的数据
            
        Returns:
            响应数据，如果没有响应则返回None
        """
        if len(data) < 9:
            return None
        
        # 检查起始位
        if data[0] != 0xFF:
            return None
        
        # 检查校验和
        received_checksum = data[8]
        calculated_checksum = self.calculate_checksum(list(data))
        
        if received_checksum != calculated_checksum:
            print(f"❌ 校验和错误: 接收={received_checksum:02X}, 计算={calculated_checksum:02X}")
            return None
        
        # 解析命令
        if data[1] == 0x01 and data[2] == 0x78:  # 模式切换命令
            if data[3] == 0x41:  # 切换到问答模式
                self.mode = "qa"
                print("✅ 已切换到问答模式")
                return self.switch_to_qa_mode()
            elif data[3] == 0x40:  # 切换到主动上传模式
                self.mode = "active"
                print("✅ 已切换到主动上传模式")
                return self.switch_to_active_mode()
        
        elif data[1] == 0x01 and data[2] == 0x86:  # 读取气体浓度命令
            if self.mode == "qa":
                print("📖 收到读取浓度命令")
                return self.generate_qa_response(self.current_concentration)
            else:
                print("⚠️  当前为主动上传模式，忽略读取命令")
        
        return None
    
    def simulate_concentration_change(self):
        """模拟浓度变化"""
        # 随机变化浓度值 (20-100 ppb之间)
        change = random.randint(-5, 5)
        new_concentration = max(20, min(100, self.current_concentration + change))
        
        if new_concentration != self.current_concentration:
            self.current_concentration = new_concentration
            print(f"🌡️  浓度变化: {new_concentration} ppb")
    
    def active_upload_loop(self):
        """主动上传模式循环"""
        while self.running and self.mode == "active":
            try:
                # 模拟浓度变化
                self.simulate_concentration_change()
                
                # 生成并发送数据
                data = self.generate_active_upload_data()
                if self.serial_conn and self.serial_conn.is_open:
                    self.serial_conn.write(data)
                
                # 等待1秒
                time.sleep(1)
                
            except Exception as e:
                print(f"❌ 主动上传错误: {e}")
                break
    
    def serial_read_loop(self):
        """串口读取循环"""
        while self.running:
            try:
                if self.serial_conn and self.serial_conn.is_open:
                    # 读取数据
                    if self.serial_conn.in_waiting >= 9:
                        data = self.serial_conn.read(9)
                        print(f"📥 收到数据: {' '.join([f'0x{x:02X}' for x in data])}")
                        
                        # 处理命令
                        response = self.process_command(data)
                        if response:
                            self.serial_conn.write(response)
                
                time.sleep(0.1)  # 短暂休眠
                
            except Exception as e:
                print(f"❌ 串口读取错误: {e}")
                break
    
    def start(self):
        """启动模拟器"""
        try:
            # 打开串口
            self.serial_conn = serial.Serial(
                port=self.port,
                baudrate=self.baudrate,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=1
            )
            
            print(f"✅ 串口 {self.port} 打开成功")
            
            self.running = True
            
            # 启动读取线程
            self.thread = threading.Thread(target=self.serial_read_loop)
            self.thread.daemon = True
            self.thread.start()
            
            # 如果是主动上传模式，启动上传线程
            if self.mode == "active":
                upload_thread = threading.Thread(target=self.active_upload_loop)
                upload_thread.daemon = True
                upload_thread.start()
            
            print("🚀 模拟器启动完成，按 Ctrl+C 停止")
            
            # 主循环
            try:
                while self.running:
                    time.sleep(1)
            except KeyboardInterrupt:
                print("\n⏹️  收到停止信号")
                
        except Exception as e:
            print(f"❌ 启动失败: {e}")
        finally:
            self.stop()
    
    def stop(self):
        """停止模拟器"""
        self.running = False
        if self.serial_conn and self.serial_conn.is_open:
            self.serial_conn.close()
            print("🔌 串口已关闭")


def main():
    """主函数"""
    parser = argparse.ArgumentParser(description="Dart WZ-S-K 甲醛传感器模拟器")
    parser.add_argument("--port", help="串口端口 (例如: COM3, /dev/ttyUSB0)")
    parser.add_argument("--baudrate", "-b", type=int, default=9600, help="波特率 (默认: 9600)")
    
    args = parser.parse_args()
    
    print("🎯 Dart WZ-S-K 甲醛传感器模拟器")
    print("=" * 50)
    
    # 创建并启动模拟器
    simulator = DartSensorSimulator(args.port, args.baudrate)
    simulator.start()


if __name__ == "__main__":
    main()
