"""U3 LAN discovery protocol research service, not a game session backend.

Wire contract recovered from PS3 0x370260, 0x3700b0 and bit codec assembly.
The standalone responder has NO room provider and truthfully reports zero rooms.
It is not integrated with the remaster or advertised as playable multiplayer.
"""
from dataclasses import dataclass
import argparse,json,socket,struct,threading

REQUEST=b'\xc8'
HEADER=struct.Struct('>BI')
ROOM=struct.Struct('>HQHH16s')

@dataclass(frozen=True)
class RoomRecord:
    tcp_port:int
    room_id:int
    members:int
    capacity:int
    host_name:bytes

def encode_response(rooms=()):
    if len(rooms)>4:raise ValueError('native host has four session slots')
    data=bytearray(HEADER.pack(0xc9,len(rooms)))
    for room in rooms:
        if not 0<room.tcp_port<=65535 or not 0<room.room_id<1<<64:raise ValueError('invalid room address/id')
        if not 0<=room.members<=room.capacity<=65535 or room.capacity==0:raise ValueError('invalid occupancy')
        if len(room.host_name)!=16:raise ValueError('host identity is exactly 16 raw bytes')
        data.extend(ROOM.pack(room.tcp_port,room.room_id,room.members,room.capacity,room.host_name))
    return bytes(data)

def decode_response(data):
    if len(data)<HEADER.size:raise ValueError('short discovery response')
    kind,count=HEADER.unpack_from(data)
    if kind!=0xc9 or count>20:raise ValueError('invalid message/count')
    if len(data)!=HEADER.size+count*ROOM.size:raise ValueError('invalid discovery length')
    return tuple(RoomRecord(*ROOM.unpack_from(data,HEADER.size+i*ROOM.size)) for i in range(count))

class EmptyDiscoveryServer:
    """No eligible sessions exist: never manufacture an advertised room."""
    def __init__(self,host='127.0.0.1',port=9012):
        self.socket=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        try:
            self.socket.bind((host,port));self.socket.settimeout(.1)
        except BaseException:
            self.socket.close();raise
        self.address=self.socket.getsockname()
        self.requests=0
    def run(self,stop):
        while not stop.is_set():
            try:data,sender=self.socket.recvfrom(1024)
            except socket.timeout:continue
            if data!=REQUEST:continue
            # Match native self-request suppression when bound to one address.
            if sender[0]==self.address[0]:continue
            self.socket.sendto(encode_response(),sender)
            self.requests+=1
    def close(self):self.socket.close()

def self_test():
    # Codec fixture only: not a real or advertised room.
    fixture=bytes.fromhex('c900000001233311223344556677880001000a5265766976616c486f73740000000000')
    record=RoomRecord(9011,0x1122334455667788,1,10,b'RevivalHost'.ljust(16,b'\0'))
    assert len(fixture)==35 and encode_response((record,))==fixture
    assert decode_response(fixture)==(record,)
    for invalid in (b'',b'\xc9',fixture[:-1],fixture+b'\0',b'\xc9\0\0\0\x15'):
        try:decode_response(invalid)
        except ValueError:pass
        else:raise AssertionError('malformed packet accepted')
    service=EmptyDiscoveryServer();stop=threading.Event()
    thread=threading.Thread(target=service.run,args=(stop,));thread.start()
    try:
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as client:
            client.bind(('127.0.0.2',0));client.settimeout(2)
            source=client.getsockname()
            client.sendto(REQUEST,service.address)
            reply,sender=client.recvfrom(1024)
            assert reply==b'\xc9\0\0\0\0' and decode_response(reply)==()
            assert sender==service.address
            client.sendto(b'\xc7',service.address);client.settimeout(.2)
            try:client.recvfrom(1024)
            except socket.timeout:pass
            else:raise AssertionError('invalid request received a response')
        result=dict(status='pass',scope='standalone loopback discovery only; no remaster integration',server=list(service.address),client=list(source),request_hex=REQUEST.hex(),response_hex=reply.hex(),eligible_rooms=0,codec_fixture_only=fixture.hex(),malformed_messages_rejected=True)
    finally:
        stop.set();thread.join(3);service.close()
        if thread.is_alive():raise RuntimeError('discovery thread did not stop')
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--self-test',action='store_true')
    p.add_argument('--bind',default='127.0.0.1')
    p.add_argument('--port',type=int,default=9012)
    a=p.parse_args()
    if a.self_test:print(json.dumps(self_test(),indent=2));return
    service=EmptyDiscoveryServer(a.bind,a.port)
    print(f'Discovery research responder {service.address}; zero eligible rooms; not connected to the game',flush=True)
    try:service.run(threading.Event())
    except KeyboardInterrupt:pass
    finally:service.close()
if __name__=='__main__':main()
