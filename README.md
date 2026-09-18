# ESP-IRC on LILYGO T-Display-S3

An ESP-IDF 6.0.2 IRC server with a status dashboard on the board's built-in
ST7789 screen. Hold the board in landscape with its USB connector on the left.
The display shows Wi-Fi/IP, connected users, free memory, listener ports,
uptime, and UTC time.

The board uses 16 MB flash and 8 MB octal PSRAM. The existing storage partition
layout is retained. Display wiring is fixed to the standard T-Display-S3:

| Signal | GPIO |
| --- | --- |
| Power / backlight | 15 / 38 |
| Reset / CS / DC | 5 / 6 / 7 |
| WR / RD | 8 / 9 |
| D0–D7 | 39, 40, 41, 42, 45, 46, 47, 48 |

1. Activate an ESP-IDF 6.0.2 environment.
2. Copy `main/config.example.h` to `main/config.h` and set the Wi-Fi credentials.
3. If TLS is enabled, provide the files described in [main/certs/README.md](main/certs/README.md).
4. Run `idf.py build`, then `idf.py -p /dev/serial/by-id/<board> flash monitor`.

Wi-Fi settings and TLS keys/certificates are ignored by Git. Plaintext IRC uses
port 6667; the checked-in configuration also enables TLS on 6697. For a local
self-signed certificate, run:

```sh
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
  -sha256 -noenc -days 365 -subj '/CN=esp-irc' \
  -addext 'subjectAltName=DNS:esp-irc,DNS:esp-irc.local' \
  -addext 'extendedKeyUsage=serverAuth' \
  -keyout main/certs/server.key -out main/certs/server.crt
chmod 600 main/certs/server.key
```

Clients must trust this certificate explicitly. The certificate names do not
configure DNS; use the IP shown on screen to reach the board.
