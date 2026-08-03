# Mail Server

Production-grade custom mail server in **C++17 with Qt6**, built for Ubuntu Server. Implements **SMTP, IMAP, POP3** from scratch with PostgreSQL authentication, TLS encryption, Rspamd anti-spam, Alibaba Cloud DirectMail relay, and a web-based admin panel.

---

## Features

| Feature | Details |
|---------|---------|
| SMTP | Port 25 (ESMTP), STARTTLS |
| Submission | Port 587 (client submission), STARTTLS |
| SMTPS | Port 465 (implicit TLS) |
| IMAP | Port 143 (STARTTLS) |
| IMAPS | Port 993 (implicit TLS) |
| POP3 | Port 110 (STARTTLS) |
| POP3S | Port 995 (implicit TLS) |
| Admin API | Port 9090 (REST + Web UI) |
| Database | PostgreSQL (users, auth) |
| Storage | Maildir format |
| Passwords | SHA-512 crypt (`$6$`), same as Linux `/etc/shadow` |
| Outbound | Alibaba Cloud DirectMail (ap-southeast-1) |
| Anti-spam | Rspamd integration |
| DKIM | Signing support |
| Rate limiting | Per-IP connection & auth limits |
| Catch-All | Toggleable via admin panel — no restart needed |

---

## Quick Start — Development

### Prerequisites

```bash
sudo apt install -y build-essential cmake qt6-base-dev libqt6sql6-psql libssl-dev pkg-config postgresql
```

### Build

```bash
cd mailServer
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

### Setup PostgreSQL

```bash
sudo -u postgres psql -c "CREATE USER mailserver WITH PASSWORD 'CHANGE_ME';"
sudo -u postgres psql -c "CREATE DATABASE mailserver OWNER mailserver;"
PGPASSWORD=CHANGE_ME psql -h 127.0.0.1 -U mailserver -d mailserver -f ../migrations/001_create_users_table.sql
```

### Configure

```bash
cp config.json.example config.json
# Edit config.json with your domain, TLS certs, DB password, and Alibaba Cloud keys
```

### Run Locally (non-privileged ports)

```bash
./mailserver -c ../config.json
# Uses ports: 2525, 5877, 4655, 1143, 9933, 1103, 9955, 9090
```

---

## Production Deployment

### 1. Build & Prepare

```bash
cd mailServer/build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
cd ..
mkdir -p deploy_update
cp build/mailserver deploy_update/
cp admin.html config.json deploy_update/
cp -r scripts/alibaba_add_sender.py deploy_update/scripts/
```

### 2. Deploy to Server

```bash
# Copy files
scp deploy_update/mailserver root@YOUR_SERVER:/root/mailserver_deploy/usr/bin/
scp deploy_update/admin.html deploy_update/config.json root@YOUR_SERVER:/root/mailserver_deploy/etc/mailserver/
scp deploy_update/scripts/* root@YOUR_SERVER:/root/mailserver_deploy/etc/mailserver/scripts/

# Set permission to bind privileged ports
ssh root@YOUR_SERVER "setcap cap_net_bind_service=+ep /root/mailserver_deploy/usr/bin/mailserver"

# Start
ssh root@YOUR_SERVER "cd /root/mailserver_deploy/etc/mailserver && nohup /root/mailserver_deploy/usr/bin/mailserver -c /root/mailserver_deploy/etc/mailserver/config.json > server_live.log 2>&1 &"
```

### 3. Verify

```bash
# Check SMTP
nc -v YOUR_SERVER 25
# → 220 YOUR_SERVER ESMTP MailServer/1.0

# Check Admin API
curl -X POST http://YOUR_SERVER:9090/api/login \
  -H "Content-Type: application/json" \
  -d '{"password":"YOUR_ADMIN_PASSWORD"}'

# Check catch-all status
curl http://YOUR_SERVER:9090/api/catch-all \
  -H "Authorization: Bearer <token>"
```

### 4. Access Admin Panel

Open `http://YOUR_SERVER:9090` in a browser. Login with your configured admin password.

---

## Catch-All Inbox

The catch-all feature can be toggled from the **Admin Panel → Settings** tab:

- **Enabled (ON):** All incoming emails to your domain are accepted, even for addresses that don't have a registered user.
- **Disabled (OFF):** Only registered users receive email. Non-existent addresses get proper **bounce notifications** (550 5.1.1 No such user) — just like Google does.

The toggle takes effect **immediately** — no restart required. It is also stored in `config.json` under `catch_all_enabled: true/false`.

---

## Admin Panel

The admin panel (`admin.html`) is served by the API server on port 9090. Features:

- **Dashboard** — Server health, uptime, service status
- **Users** — Create, edit, delete mail users. When Alibaba Cloud is configured, new users are auto-added as DirectMail senders.
- **Logs** — View live server logs
- **Settings** — Toggle catch-all, configure Alibaba Cloud sender creation, change admin password

---

## Configuration Reference (`config.json`)

Copy `config.json.example` to `config.json` and fill in your values:

| Section | Key | Description |
|---------|-----|-------------|
| `hostname` | | Your server hostname (e.g. `mail.example.com`) |
| `interfaces` | | Bind addresses (`["0.0.0.0"]` for all) |
| `local_domains` | | Accepted local domains |
| `catch_all_enabled` | | Accept mail for unknown users |
| `maildir_base_path` | | Maildir storage root |
| `quota_max_bytes` | | Hard quota in bytes (1 GB = `1073741824`) |
| `quota_soft_percent` | | Soft quota warning % |
| `ports.*` | | SMTP/IMAP/POP3 port configuration |
| `tls.cert_file` | | Path to TLS certificate |
| `tls.key_file` | | Path to TLS private key |
| `database.*` | | PostgreSQL connection settings |
| `outbound.relay.*` | | Alibaba DirectMail relay config |
| `outbound.spool_dir` | | Email spool directory |
| `alibaba_cloud.*` | | Alibaba Cloud API credentials |
| `api.token_hash` | | SHA-256 hash of admin password |
| `api.rate_limit_rpm` | | API rate limit (requests/min) |
| `rate_limits.*` | | Per-IP connection/auth rate limits |
| `rspamd.*` | | Spam filter host/port |
| `dkim.*` | | DKIM signing configuration |

See `config.json.example` for all available options with placeholders.

---

## API Endpoints

Token: `Authorization: Bearer <token>`

| Method | Endpoint | Auth | Description |
|--------|----------|------|-------------|
| POST | `/api/login` | No | Admin login |
| GET | `/api/health` | No | Health check |
| GET | `/api/stats` | Yes | Server statistics |
| GET | `/api/config/status` | Yes | Config summary |
| GET | `/api/users` | Yes | List users |
| POST | `/api/users` | Yes | Create user |
| PUT | `/api/users/:email` | Yes | Update user |
| DELETE | `/api/users/:email` | Yes | Delete user |
| POST | `/api/users/:email/password` | Yes | Change password |
| GET | `/api/logs?lines=100` | Yes | View logs |
| GET | `/api/catch-all` | Yes | Get catch-all status |
| POST | `/api/catch-all` | Yes | Toggle catch-all (`{"enabled":true}`) |
| POST | `/api/admin/password` | Yes | Change admin password |

---

## Email Client Setup

| Setting | IMAP (Incoming) | SMTP (Outgoing) |
|---------|-----------------|-----------------|
| Server | Your server hostname | Your server hostname |
| Port | `143` (STARTTLS) / `993` (TLS) | `587` (STARTTLS) / `465` (TLS) |
| Username | `user@yourdomain.com` | `user@yourdomain.com` |
| Auth | Normal password | Normal password |

---

## DNS Records

| Type | Name | Value | Purpose |
|------|------|-------|---------|
| A | @ | YOUR_SERVER_IP | Root domain |
| A | mail | YOUR_SERVER_IP | Mail subdomain |
| MX | @ | yourdomain.com (10) | Inbound routing |
| TXT | @ | `v=spf1 mx include:spfdm-ap-southeast-1.aliyun.com -all` | SPF |
| TXT | mail._domainkey | *(DKIM key)* | DKIM |
| TXT | _dmarc | `v=DMARC1; p=quarantine` | DMARC |

---

## TLS Certificate

```bash
sudo certbot certonly --webroot -w /var/www/html -d yourdomain.com \
     --email admin@yourdomain.com --agree-tos --non-interactive
```

Then update `config.json`:
```json
"tls": {
    "cert_file": "/etc/letsencrypt/live/yourdomain.com/fullchain.pem",
    "key_file": "/etc/letsencrypt/live/yourdomain.com/privkey.pem"
}
```

---

## Project Structure

```
mailServer/
├── main.cpp                          # Entry point
├── config.json.example               # Config template (copy to config.json)
├── admin.html                        # Web admin panel
├── CMakeLists.txt                    # Build system
├── src/
│   ├── ConfigManager.{h,cpp}         # Config reader (supports runtime reload)
│   ├── Logger.{h,cpp}                # Structured logging
│   ├── auth/
│   │   ├── AuthManager.{h,cpp}       # Auth + rate limiting
│   │   └── UserRepository.{h,cpp}    # PostgreSQL user CRUD
│   ├── tls/
│   │   └── TlsContext.{h,cpp}        # TLS setup
│   ├── services/
│   │   ├── SmtpServer.{h,cpp}        # SMTP server
│   │   ├── SmtpConnection.{h,cpp}    # SMTP per-connection handler
│   │   ├── ImapServer.{h,cpp}        # IMAP server
│   │   ├── ImapConnection.{h,cpp}    # IMAP per-connection handler
│   │   ├── Pop3Server.{h,cpp}        # POP3 server
│   │   └── Pop3Connection.{h,cpp}    # POP3 per-connection handler
│   ├── storage/
│   │   └── MaildirStorage.{h,cpp}    # Maildir storage engine
│   ├── queue/
│   │   ├── QueueManager.{h,cpp}      # Email queue manager
│   │   └── OutboundWorker.{h,cpp}    # Outbound relay delivery
│   ├── filters/
│   │   └── SpamFilter.{h,cpp}        # Async Rspamd integration
│   ├── api/
│   │   └── ApiServer.{h,cpp}         # REST API + admin panel server
│   ├── parser/
│   │   └── MimeParser.{h,cpp}        # MIME parsing
│   └── utils/
│       └── RateLimiter.h             # Per-IP rate limiter
├── migrations/
│   └── 001_create_users_table.sql    # DB schema
├── scripts/
│   └── alibaba_add_sender.py         # Alibaba DirectMail sender helper
├── deploy/
│   └── mailserver.service            # systemd unit
└── certs/
    └── server.crt                    # TLS certificate
```

---

## Systemd Service

```ini
[Unit]
Description=Mail Server
After=network.target postgresql.service

[Service]
Type=simple
User=root
ExecStart=/root/mailserver_deploy/usr/bin/mailserver -c /root/mailserver_deploy/etc/mailserver/config.json
WorkingDirectory=/root/mailserver_deploy/etc/mailserver
Restart=on-failure
RestartSec=5
MemoryMax=2G
LimitNOFILE=65536
AmbientCapabilities=CAP_NET_BIND_SERVICE

[Install]
WantedBy=multi-user.target
```
