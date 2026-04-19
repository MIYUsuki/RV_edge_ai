import socket
import struct
import threading
import asyncio
import websockets
import base64
import cv2
import numpy as np
import os

# 全局变量，存放最新的一帧 Base64 画面供小程序读取
latest_frame_b64 = None


# ================= 1. TCP 接收与本地显示线程 =================
def recvall(sock, count):
    buf = b''
    while count:
        newbuf = sock.recv(count)
        if not newbuf: return None
        buf += newbuf
        count -= len(newbuf)
    return buf


def tcp_receiver_thread():
    global latest_frame_b64
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.bind(('0.0.0.0', 8080))
    server_socket.listen(1)
    print("[TCP] 等待 Luckfox Pico 连接 (端口 8080)...")

    while True:
        conn, addr = server_socket.accept()
        print(f"[TCP] 成功连接到板子: {addr}")
        try:
            while True:
                # 1. 接收包头和图片数据
                length_data = recvall(conn, 4)
                if not length_data: break

                img_size = struct.unpack('<L', length_data)[0]
                img_data = recvall(conn, img_size)
                if not img_data: break

                # ---------------- 核心 ----------------

                # 路线 A：转成 Base64 喂给后台的 WebSocket（小程序端）
                latest_frame_b64 = base64.b64encode(img_data).decode('utf-8')

                # 路线 B：解码并在电脑本地弹窗显示（PC 端）
                np_arr = np.frombuffer(img_data, np.uint8)
                frame = cv2.imdecode(np_arr, cv2.IMREAD_COLOR)

                if frame is not None:
                    # 电脑上可以放大 1.5 倍观看
                    frame = cv2.resize(frame, (int(frame.shape[1] * 1.5), int(frame.shape[0] * 1.5)),
                                       interpolation=cv2.INTER_LINEAR)
                    cv2.imshow('PC Local Monitor (Press Q to exit)', frame)

                    # 按 'q' 键退出程序
                    if cv2.waitKey(1) & 0xFF == ord('q'):
                        print("手动退出程序...")
                        os._exit(0)  # 强杀整个 Python 进程

        except Exception as e:
            print(f"[TCP] 接收异常: {e}")
        finally:
            cv2.destroyAllWindows()
            conn.close()


# ================= 2. WebSocket 发送服务 =================
async def ws_video_stream(websocket):
    print(f"[WS] 微信小程序已接入！开始推流...")
    try:
        while True:
            if latest_frame_b64:
                await websocket.send(latest_frame_b64)
            # 控制发给手机的帧率 (0.05秒发一次 = 20 FPS)，防止塞爆手机微信内存
            await asyncio.sleep(0.05)
    except websockets.exceptions.ConnectionClosed:
        print("[WS] 微信小程序断开连接")


async def start_ws_server():
    print("[WS] WebSocket 服务器启动，等待小程序连接 (端口 8765)...")
    async with websockets.serve(ws_video_stream, "0.0.0.0", 8765):
        await asyncio.Future()


if __name__ == '__main__':
    # 启动 TCP 接收和 OpenCV 显示的子线程
    threading.Thread(target=tcp_receiver_thread, daemon=True).start()

    # 在主线程启动 WebSocket 服务器
    asyncio.run(start_ws_server())