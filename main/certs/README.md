# IRC TLS credentials

Enable `CONFIG_IRC_TLS_ENABLED` in `idf.py menuconfig`. Plaintext IRC remains
on `PORT` (normally 6667); implicit TLS uses `CONFIG_IRC_TLS_PORT` (normally
6697).

Provide:

- `server.crt`: server certificate followed by intermediate certificates
- `server.key`: matching unencrypted private key

The paths are configurable under **IRC server configuration**. Use a trusted
certificate whose Subject Alternative Name matches the server hostname. Keep
the private key out of version control and use a unique key per device.

Credentials are embedded in the firmware, so certificate renewal requires a
new build. Production devices should use Secure Boot, Flash Encryption, and
signed updates. Do not expose the plaintext listener outside a trusted network.
