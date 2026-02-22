import socket
import threading
import tkinter as tk
from tkinter import messagebox
from pynput import keyboard, mouse

SERVER_PORT = 5555

class HidClient:
    def __init__(self, host):
        self.host = host
        self.sock = None
        self.lock = threading.Lock()
        self.prev_pos = None
        self.buttons_state = 0  # bitmask: left=1, right=2, middle=4

    def connect(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((self.host, SERVER_PORT))
        self.sock = s

    def send_line(self, line):
        with self.lock:
            if self.sock:
                try:
                    self.sock.sendall((line + "\n").encode("utf-8"))
                except OSError:
                    self.sock = None

    def on_key_press(self, key):
        # Alfanumeryczne i symbole
        if hasattr(key, "char") and key.char:
            self.send_line(key.char)
            return
        # Specjalne, które obsługuje serwer
        mapping = {
            keyboard.Key.enter: "\n",
            keyboard.Key.space: " ",
            keyboard.Key.backspace: "\b",
            keyboard.Key.tab: "\t",
            keyboard.Key.esc: chr(0x1B),
        }
        if key in mapping:
            self.send_line(mapping[key])

    def on_move(self, x, y):
        if self.prev_pos is None:
            self.prev_pos = (x, y)
            return
        dx = x - self.prev_pos[0]
        dy = y - self.prev_pos[1]
        self.prev_pos = (x, y)
        if dx or dy:
            self.send_line(f"M {dx} {dy} {self.buttons_state}")

    def on_click(self, x, y, button, pressed):
        mask_map = {
            mouse.Button.left: 1,
            mouse.Button.right: 2,
            mouse.Button.middle: 4,
        }
        bit = mask_map.get(button, 0)
        if bit:
            if pressed:
                self.buttons_state |= bit
            else:
                self.buttons_state &= ~bit
            # zero ruchu; tylko stan przycisków
            self.send_line(f"M 0 0 {self.buttons_state}")

def start_client(host, status_label):
    client = HidClient(host)
    try:
        client.connect()
        status_label.config(text=f"Connected to {host}:{SERVER_PORT}")
    except OSError as e:
        messagebox.showerror("Connection failed", str(e))
        status_label.config(text="Not connected")
        return

    kb_listener = keyboard.Listener(on_press=client.on_key_press)
    ms_listener = mouse.Listener(
        on_move=client.on_move,
        on_click=client.on_click,
    )
    kb_listener.start()
    ms_listener.start()

def main():
    root = tk.Tk()
    root.title("Virtual HID client")

    tk.Label(root, text="Server IP:").grid(row=0, column=0, padx=5, pady=5)
    host_var = tk.StringVar(value="10.1.20.21")
    tk.Entry(root, textvariable=host_var, width=20).grid(row=0, column=1, padx=5, pady=5)

    status_label = tk.Label(root, text="Not connected")
    status_label.grid(row=2, column=0, columnspan=2, padx=5, pady=5)

    def on_connect():
        host = host_var.get().strip()
        threading.Thread(target=start_client, args=(host, status_label), daemon=True).start()

    tk.Button(root, text="Connect", command=on_connect).grid(row=1, column=0, columnspan=2, pady=5)

    root.mainloop()

if __name__ == "__main__":
    main()
