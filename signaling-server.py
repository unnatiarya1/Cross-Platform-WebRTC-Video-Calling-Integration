import asyncio
import websockets

connected_clients = set()
peer_connections = {}

async def handler(ws, path):
    raddr = ws.remote_address
    print("Connected to {!r}".format(raddr))
    connected_clients.add(ws)
    # await ws.send("HELLO")
    try:
        while True:
            msg = await ws.recv()
            print(msg,"------------")
            for client in connected_clients:
                if client != ws:
                    await client.send(msg)
    except websockets.ConnectionClosed:
        print("Connection to peer {!r} closed")
        connected_clients.remove(ws)
        print(ws.remote_address)
        for client in connected_clients:
        #         # if client != ws:
                    await client.send("closed")
        # # await client.send("closed")
        


def run_server(addr='192.168.68.103', port=8443):
    print("Listening on http://{}:{}".format(addr, port))
    wsd = websockets.serve(handler, addr, port, max_queue=16)
    return wsd

def main():
    loop = asyncio.get_event_loop()
    wsd = run_server()

    try:
        loop.run_until_complete(wsd)
        loop.run_forever()
    except KeyboardInterrupt:
        print('Exiting server...')
    finally:
        loop.run_until_complete(wsd.wait_closed())

if __name__ == "__main__":
    main()
