import socket as Socket
from socket import socket
import threading
import time


class ServerSession(threading.Thread):
    def __init__(self, clientSocket, serverSocket, serverIp, serverPort, request):
        threading.Thread.__init__(self)
        self.clientSocket = clientSocket
        self.serverSocket = serverSocket
        self.serverIp = serverIp
        self.serverPort = serverPort
        self.message = request.encode()  # Ensure we send bytes
        self.bufferSize = 1024

    def run(self):
        try:
            self.serverSocket.send(self.message)               # LB sends request to server
            res = self.serverSocket.recv(self.bufferSize)      # LB receives result from server
            self.clientSocket.send(res)                        # LB sends result to client
        except Exception as e:
            print(f"[Error] Server connection lost: {e}")
            # Attempt to reconnect and retry once
            try:
                self.serverSocket.close()
                new_socket = socket(Socket.AF_INET, Socket.SOCK_STREAM)
                new_socket.connect((self.serverIp, self.serverPort))
                new_socket.send(self.message)
                res = new_socket.recv(self.bufferSize)
                self.clientSocket.send(res)
            except Exception as retry_e:
                print(f"[Critical] Failed to retry request: {retry_e}")
        finally:
            self.clientSocket.close()                          # Close client connection


class LoadBalancer:
    def __init__(self, initIp, initPort, serverList):
        self.ip = initIp
        self.port = initPort
        self.knownServerList = serverList
        self.serverConnections = {}
        self.serverTypes = {0: 'VIDEO', 1: 'VIDEO', 2: 'MUSIC'}  # serv1=0, serv2=1, serv3=2
        self.bufferSize = 1024
        self.runningSessions = []
        self.timestamp = time.perf_counter()
        self.serverSchedule = {i: 0 for i in serverList}       # Estimated time left per server

    def loadBalance(self):
        print("=== Starting Load Balancer ===")

        # Connect to all servers upfront
        for serverNum, ip in self.knownServerList.items():
            try:
                serverConnection = socket(Socket.AF_INET, Socket.SOCK_STREAM)
                serverConnection.connect((ip, self.port))
                self.serverConnections[serverNum] = serverConnection
                print(f"[Info] Connected to server {serverNum} ({self.serverTypes[serverNum]}) at {ip}")
            except Exception as e:
                print(f"[Error] Could not connect to server {serverNum} at {ip}: {e}")
                return

        # Open socket for client connections
        clientListener = socket(Socket.AF_INET, Socket.SOCK_STREAM)
        clientListener.bind((self.ip, self.port))
        clientListener.listen(5)
        print(f"[Info] LB listening on {self.ip}:{self.port}")

        while True:
            newClient, clientAddr = clientListener.accept()
            try:
                request = newClient.recv(self.bufferSize).decode().strip()
                print(f"[Client] {clientAddr[0]} sent request: {request}")
                if len(request) < 2 or not request[1].isdigit():
                    print(f"[Warning] Malformed request: '{request}'")
                    newClient.close()
                    continue

                delegatedServer = self.greedyBalance(request)
                newSession = ServerSession(
                    newClient,
                    self.serverConnections[delegatedServer],
                    self.knownServerList[delegatedServer],
                    self.port,
                    request
                )
                self.runningSessions.append(newSession)
                newSession.start()
                self.cleanupSessions()
            except Exception as e:
                print(f"[Error] Failed to handle client {clientAddr}: {e}")
                newClient.close()

    def cleanupSessions(self):
        """Remove finished threads from runningSessions."""
        self.runningSessions = [t for t in self.runningSessions if t.is_alive()]

    def greedyBalance(self, request):
        """Selects the server with the least expected completion time for the request."""
        currentTime = time.perf_counter()
        elapsed = currentTime - self.timestamp

        # Update remaining work estimates for all servers
        for serverNum in self.serverSchedule:
            self.serverSchedule[serverNum] = max(self.serverSchedule[serverNum] - elapsed, 0)

        self.timestamp = currentTime

        # Calculate weighted times based on server type and request
        weightedTimes = self.getWeightedResponseTime(request)
        greedyServer = min(weightedTimes, key=weightedTimes.get)
        self.serverSchedule[greedyServer] = weightedTimes[greedyServer]

        print(f"[LB] Delegated {request} to server {greedyServer} ({self.serverTypes[greedyServer]}), "
              f"expected time: {weightedTimes[greedyServer]:.2f}s")
        return greedyServer

    def getWeightedResponseTime(self, request):
        """Returns expected completion times for all servers based on request type."""
        req_type = request[0].upper()
        duration = int(request[1])
        expectedTimes = {}

        for serverNum in self.serverSchedule:
            if self.serverTypes[serverNum] == 'VIDEO':
                weight = 2 if req_type == 'M' else 1  # Video servers: M=2x, V/P=1x
            else:  # MUSIC server
                if req_type == 'V':
                    weight = 3
                elif req_type == 'P':
                    weight = 2
                else:  # M
                    weight = 1
            expectedTimes[serverNum] = self.serverSchedule[serverNum] + duration * weight

        return expectedTimes


if __name__ == '__main__':
    # Configuration (adjust IPs if needed)
    knownServers = {
        0: '192.168.0.101',  # serv1 (VIDEO)
        1: '192.168.0.102',  # serv2 (VIDEO)
        2: '192.168.0.103'   # serv3 (MUSIC)
    }
    localIp = '10.0.0.1'     # LB's client-facing IP
    localPort = 80           # Port for clients/servers

    lb = LoadBalancer(localIp, localPort, knownServers)
    lb.loadBalance()