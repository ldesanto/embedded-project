import socket
import argparse
import sys
import time

COUNT = 0

def update(data):
    #TODO : update the count based on the data received
    pass
def recv(sock):
    data = sock.recv(1)
    buf = b""
    while data.decode("utf-8") != "\n":
        buf += data
        data = sock.recv(1)
    return buf
def send(sock, data):
    sock.send(data.encode("utf-8"))

def main(ip, port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((ip, port))

    for _ in range(20):
        sock.send(b"test\n")
        data = recv(sock)
        print("Message received : "+ data.decode("utf-8"))
        update(data.decode("utf-8"))
        print("new count :" + str(COUNT))
        time.sleep(1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--ip", dest="ip", type=str)
    parser.add_argument("--port", dest="port", type=int)
    args = parser.parse_args()
