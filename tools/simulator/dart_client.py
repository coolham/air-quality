#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Dart传感器客户端程序
支持主动接收和问答两种模式访问模拟器数据
"""

import serial
import time
import threading
import argparse
import sys
from typing import Optional, List


class DartClient:
    """Dart传感器客户端"""
    
    def __init__(self, port: str, baudrate: int = 9600):
        """
        初始化客户端
        
        Args:
            port: 串口端口
            baudrate: 波特率
        """
        self.port = port
        self.baudrate = baudrate
        self.serial_conn = None
        self.running = False
        self.thread = None
        self.mode = "active"  # 当前模式：active 或 qa
        
        # 数据统计
        self.received_count = 0
        self.error_count = 0
        
        # 自动请求相关
        self.auto_request_thread = None
        self.auto_request_interval = 5  # 自动请求间隔（秒）
        self.last_request_time = 0
        
        print(f"🎯 Dart传感器客户端初始化完成")
        print(f"📡 串口: {port}")
        print(f"⚙️  波特率: {baudrate}")
        print(f"🔧 当前模式: 主动接收")
        print(f"⏰ 自动请求间隔: {self.auto_request_interval}秒")
        print("-" * 50)
    
    def calculate_checksum(self, data: List[int]) -> int:
        """计算校验和"""
        if len(data) < 8:
            return 0
        
        # 计算第1到第7字节的和
        checksum = sum(data[1:8])
        # 取反+1
        checksum = (~checksum + 1) & 0xFF
        return checksum
    
    def verify_checksum(self, data: bytes) -> bool:
        """验证校验和"""
        if len(data) < 9:
            return False
        
        received_checksum = data[8]
        calculated_checksum = self.calculate_checksum(list(data))
        
        return received_checksum == calculated_checksum
    
    def parse_active_upload_data(self, data: bytes) -> Optional[dict]:
        """解析主动上传数据"""
        if len(data) != 9 or data[0] != 0xFF:
            return None
        
        # 验证校验和
        if not self.verify_checksum(data):
            print(f"❌ 校验和错误")
            return None
        
        # 解析数据
        gas_name = data[1]
        unit = data[2]
        decimal_places = data[3]
        concentration_high = data[4]
        concentration_low = data[5]
        full_scale_high = data[6]
        full_scale_low = data[7]
        
        # 计算浓度值
        concentration = (concentration_high << 8) | concentration_low
        full_scale = (full_scale_high << 8) | full_scale_low
        
        return {
            'type': 'active_upload',
            'gas_name': gas_name,
            'unit': unit,
            'decimal_places': decimal_places,
            'concentration': concentration,
            'full_scale': full_scale,
            'timestamp': time.time()
        }
    
    def parse_qa_response(self, data: bytes) -> Optional[dict]:
        """解析问答模式响应数据"""
        if len(data) != 9 or data[0] != 0xFF or data[1] != 0x86:
            return None
        
        # 验证校验和
        if not self.verify_checksum(data):
            print(f"❌ 校验和错误")
            return None
        
        # 解析数据
        ugm3_high = data[2]
        ugm3_low = data[3]
        ppb_high = data[6]
        ppb_low = data[7]
        
        # 计算浓度值
        concentration_ugm3 = (ugm3_high << 8) | ugm3_low
        concentration_ppb = (ppb_high << 8) | ppb_low
        
        return {
            'type': 'qa_response',
            'concentration_ugm3': concentration_ugm3,
            'concentration_ppb': concentration_ppb,
            'timestamp': time.time()
        }
    
    def switch_to_qa_mode(self) -> bool:
        """切换到问答模式"""
        command = [0xFF, 0x01, 0x78, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00]
        command[8] = self.calculate_checksum(command)
        
        try:
            if self.serial_conn and self.serial_conn.is_open:
                self.serial_conn.write(bytes(command))
                print(f"📤 发送切换到问答模式命令: {' '.join([f'0x{x:02X}' for x in command])}")
                
                # 等待响应
                time.sleep(0.1)
                if self.serial_conn.in_waiting >= 9:
                    response = self.serial_conn.read(9)
                    print(f"📥 收到响应: {' '.join([f'0x{x:02X}' for x in response])}")
                
                self.mode = "qa"
                self.last_request_time = time.time()  # 重置请求时间
                print("✅ 已切换到问答模式")
                print("🔄 自动请求已启动（每5秒）")
                return True
        except Exception as e:
            print(f"❌ 切换模式失败: {e}")
        
        return False
    
    def switch_to_active_mode(self) -> bool:
        """切换到主动上传模式"""
        command = [0xFF, 0x01, 0x78, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00]
        command[8] = self.calculate_checksum(command)
        
        try:
            if self.serial_conn and self.serial_conn.is_open:
                self.serial_conn.write(bytes(command))
                print(f"📤 发送切换到主动上传模式命令: {' '.join([f'0x{x:02X}' for x in command])}")
                
                # 等待响应
                time.sleep(0.1)
                if self.serial_conn.in_waiting >= 9:
                    response = self.serial_conn.read(9)
                    print(f"📥 收到响应: {' '.join([f'0x{x:02X}' for x in response])}")
                
                self.mode = "active"
                print("✅ 已切换到主动上传模式")
                print("⏹️  自动请求已停止")
                return True
        except Exception as e:
            print(f"❌ 切换模式失败: {e}")
        
        return False
    
    def request_concentration(self, auto_request: bool = False) -> Optional[dict]:
        """请求浓度数据（问答模式）"""
        if self.mode != "qa":
            if not auto_request:
                print("⚠️  当前不是问答模式，无法请求数据")
            return None
        
        command = [0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]
        command[8] = self.calculate_checksum(command)
        
        try:
            if self.serial_conn and self.serial_conn.is_open:
                self.serial_conn.write(bytes(command))
                if auto_request:
                    print(f"🔄 自动请求浓度数据: {' '.join([f'0x{x:02X}' for x in command])}")
                else:
                    print(f"📤 发送读取浓度命令: {' '.join([f'0x{x:02X}' for x in command])}")
                
                # 等待响应
                time.sleep(0.1)
                if self.serial_conn.in_waiting >= 9:
                    response = self.serial_conn.read(9)
                    if auto_request:
                        print(f"📥 自动响应: {' '.join([f'0x{x:02X}' for x in response])}")
                    else:
                        print(f"📥 收到响应: {' '.join([f'0x{x:02X}' for x in response])}")
                    
                    # 解析响应
                    data = self.parse_qa_response(response)
                    if data:
                        self.received_count += 1
                        return data
                    else:
                        self.error_count += 1
                        print("❌ 解析响应数据失败")
        except Exception as e:
            print(f"❌ 请求数据失败: {e}")
            self.error_count += 1
        
        return None
    
    def display_data(self, data: dict):
        """显示数据"""
        if data['type'] == 'active_upload':
            print(f"📊 主动上传数据:")
            print(f"   气体名称: 0x{data['gas_name']:02X}")
            print(f"   单位: 0x{data['unit']:02X}")
            print(f"   小数位数: {data['decimal_places']}")
            print(f"   浓度: {data['concentration']} ppb")
            print(f"   满量程: {data['full_scale']} ppb")
            print(f"   时间戳: {time.strftime('%H:%M:%S', time.localtime(data['timestamp']))}")
        
        elif data['type'] == 'qa_response':
            print(f"📊 问答响应数据:")
            print(f"   浓度: {data['concentration_ppb']} ppb ({data['concentration_ugm3']} ug/m3)")
            print(f"   时间戳: {time.strftime('%H:%M:%S', time.localtime(data['timestamp']))}")
        
        print(f"📈 统计: 接收={self.received_count}, 错误={self.error_count}")
        print("-" * 30)
    
    def serial_read_loop(self):
        """串口读取循环"""
        while self.running:
            try:
                if self.serial_conn and self.serial_conn.is_open:
                    # 读取数据
                    if self.serial_conn.in_waiting >= 9:
                        data = self.serial_conn.read(9)
                        print(f"📥 收到数据: {' '.join([f'0x{x:02X}' for x in data])}")
                        
                        # 解析数据
                        parsed_data = None
                        if self.mode == "active":
                            parsed_data = self.parse_active_upload_data(data)
                        else:
                            parsed_data = self.parse_qa_response(data)
                        
                        if parsed_data:
                            self.received_count += 1
                            self.display_data(parsed_data)
                        else:
                            self.error_count += 1
                            print("❌ 解析数据失败")
                    
                    # 问答模式下的自动请求
                    if self.mode == "qa":
                        current_time = time.time()
                        if current_time - self.last_request_time >= self.auto_request_interval:
                            self.request_concentration(auto_request=True)
                            self.last_request_time = current_time
                
                time.sleep(0.1)  # 短暂休眠
                
            except Exception as e:
                print(f"❌ 串口读取错误: {e}")
                self.error_count += 1
                break
    
    def start(self):
        """启动客户端"""
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
            
            print("🚀 客户端启动完成")
            print("💡 使用以下命令:")
            print("   'qa' - 切换到问答模式")
            print("   'active' - 切换到主动上传模式")
            print("   'read' - 读取浓度数据（问答模式）")
            print("   'stats' - 显示统计信息")
            print("   'quit' - 退出程序")
            print("-" * 50)
            
            # 交互循环
            while self.running:
                try:
                    command = input("请输入命令: ").strip().lower()
                    
                    if command == 'quit':
                        break
                    elif command == 'qa':
                        self.switch_to_qa_mode()
                    elif command == 'active':
                        self.switch_to_active_mode()
                    elif command == 'read':
                         if self.mode == "qa":
                             data = self.request_concentration(auto_request=False)
                             if data:
                                 self.display_data(data)
                         else:
                             print("⚠️  当前为主动上传模式，请先切换到问答模式")
                    elif command == 'stats':
                        print(f"📈 统计信息:")
                        print(f"   接收数据包: {self.received_count}")
                        print(f"   错误数据包: {self.error_count}")
                        print(f"   当前模式: {self.mode}")
                    else:
                        print("❓ 未知命令，请使用: qa, active, read, stats, quit")
                
                except KeyboardInterrupt:
                    print("\n⏹️  收到停止信号")
                    break
                except Exception as e:
                    print(f"❌ 命令执行错误: {e}")
                
        except Exception as e:
            print(f"❌ 启动失败: {e}")
        finally:
            self.stop()
    
    def stop(self):
        """停止客户端"""
        self.running = False
        if self.serial_conn and self.serial_conn.is_open:
            self.serial_conn.close()
            print("🔌 串口已关闭")
        print("👋 客户端已停止")


def main():
    """主函数"""
    parser = argparse.ArgumentParser(description="Dart传感器客户端")
    parser.add_argument("--port", default="COM6", help="串口端口 (例如: COM3, /dev/ttyUSB0)")
    parser.add_argument("--baudrate", "-b", type=int, default=9600, help="波特率 (默认: 9600)")
    
    args = parser.parse_args()
    
    print("🎯 Dart传感器客户端")
    print("=" * 50)
    
    # 创建并启动客户端
    client = DartClient(args.port, args.baudrate)
    client.start()


if __name__ == "__main__":
    main()
