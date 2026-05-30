import asyncio
import struct
import websockets
import threading
import http.server
import socketserver
import os

connected_clients = set()

def f_to_c(f): return int((f - 32) * 5 / 9)

class ForzaTelemetryProtocol:
    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        # Forward to Flydigi Adaptive Trigger (tốc độ cao nhất: forward trước, parse sau)
        self.transport.sendto(data, ('127.0.0.1', 5300))

        if len(data) < 324 or not connected_clients:
            return
        if not struct.unpack_from('<i', data, 0)[0]:  # is_race_on
            return

        rpm,       = struct.unpack_from('<f', data, 16)
        ax, ay, az = struct.unpack_from('<fff', data, 20)
        speed_ms,  = struct.unpack_from('<f', data, 256)
        power_w,   = struct.unpack_from('<f', data, 260)
        torque_nm, = struct.unpack_from('<f', data, 264)
        tfl, tfr, trl, trr = struct.unpack_from('<ffff', data, 268)
        boost_psi, = struct.unpack_from('<f', data, 284)
        sfl, sfr, srl, srr = struct.unpack_from('<ffff', data, 84)
        susp_fl, susp_fr, susp_rl, susp_rr = struct.unpack_from('<ffff', data, 68)

        accel  = data[315]; brake  = data[316]
        clutch = data[317]; ebrake = data[318]
        gear   = data[319]
        steer, = struct.unpack_from('<b', data, 320)

        gear_str = 'R' if gear == 0 else ('N' if gear == 11 else str(gear))
        boost_bar = (boost_psi - 14.7) * 0.0689476

        # f-string JSON — faster than json.dumps (avoids dict alloc + encoder)
        payload = (
            f'{{"speed":{speed_ms*3.6:.1f},"rpm":{int(rpm)},'
            f'"hp":{max(0,int(power_w*0.00134102))},"torque":{max(0,int(torque_nm))},'
            f'"boost":{boost_bar:.2f},"gear":"{gear_str}",'
            f'"accel":{int(accel/255*100)},"brake":{int(brake/255*100)},'
            f'"clutch":{int(clutch/255*100)},"ebrake":{int(ebrake/255*100)},'
            f'"steer":{int(steer/127*100)},'
            f'"glat":{ax/9.81:.3f},"glon":{az/9.81:.3f},'
            f'"susp":{{"fl":{susp_fl:.3f},"fr":{susp_fr:.3f},"rl":{susp_rl:.3f},"rr":{susp_rr:.3f}}},'
            f'"tires":{{'
            f'"fl":{{"t":{f_to_c(tfl)},"s":{abs(sfl):.2f}}},'
            f'"fr":{{"t":{f_to_c(tfr)},"s":{abs(sfr):.2f}}},'
            f'"rl":{{"t":{f_to_c(trl)},"s":{abs(srl):.2f}}},'
            f'"rr":{{"t":{f_to_c(trr)},"s":{abs(srr):.2f}}}'
            f'}}}}'
        )
        websockets.broadcast(connected_clients, payload)


async def websocket_handler(websocket):
    connected_clients.add(websocket)
    try:
        await websocket.wait_closed()
    finally:
        connected_clients.discard(websocket)


async def main():
    loop = asyncio.get_running_loop()
    transport, _ = await loop.create_datagram_endpoint(
        ForzaTelemetryProtocol,
        local_addr=('0.0.0.0', 5607),
    )
    async with websockets.serve(websocket_handler, '0.0.0.0', 8765):
        print('Forza Telemetry Server đang chạy.')
        print('  UDP  ← port 5607 (từ game)')
        print('  UDP  → port 5300 (Flydigi Adaptive Trigger)')
        print('  HTTP → http://192.168.1.199:8000 (mở link này trên tablet)')
        print('  WS   → ws://192.168.1.199:8765 (dashboard LAN)')
        await asyncio.Future()

def start_http_server():
    import os
    if os.name == 'nt':
        import ctypes
        # Set process priority to BELOW_NORMAL_PRIORITY_CLASS to NOT steal CPU from Forza process (micro-stuttering)
        try:
            ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x00004000)
        except Exception:
            pass

    # Chuyển working directory về thư mục chứa file server.py để phục vụ index.html
    os.chdir(os.path.dirname(os.path.abspath(__file__)))
    handler = http.server.SimpleHTTPRequestHandler
    # Tắt log HTTP dư thừa
    handler.log_message = lambda *args: None
    with socketserver.TCPServer(("0.0.0.0", 8000), handler) as httpd:
        httpd.serve_forever()

if __name__ == '__main__':
    # Chạy Web Server trong một luồng (thread) nền, chung với Process Python chính
    threading.Thread(target=start_http_server, daemon=True).start()
    asyncio.run(main())
