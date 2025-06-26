# HW4 – Proxy Load Balancer (C or python - yet to decide)

## 📌 Overview
This project implements a multithreaded Proxy Load Balancer in **C or python (yet to decide)** using raw socket programming.  
It accepts client requests and forwards them to one of three backend servers using a **greedy scheduling policy**.

## ⚙️ Features
- Persistent TCP connections to all 3 servers
- Handles multiple clients with `pthread`
- Scheduling based on server workload and request cost
- Automatic retry on server disconnection
- Fully compliant with HW4 specs

## 🧠 Scheduling Logic (greedy bc we are required to outperform round robin)
Each server tracks an estimated workload (`expected_time`).  
When a client request arrives (e.g., `M3`, `V1`, `P2`), the LB computes the projected cost on each server using:

| Server Type | M | V | P |
|-------------|---|---|---|
| VIDEO       | ×2| ×1| ×1|
| MUSIC       | ×1| ×3| ×2|

The server with the lowest total estimated time is selected.

## 🚀 How to Run
in C:

```bash
gcc -o load_balancer code/load_balancer.c -lpthread
sudo ./load_balancer
