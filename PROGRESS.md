# Bykiva Mail — progress and context

Last updated: 2026-11-18

## Context

- The old mail hosting at LWS (`@groupekiva.com`) is no longer paid. Mail now runs on our own
  C++/Qt6 mail server on the Alibaba Cloud Simple Application Server `Ubuntu-sidb`
  (Hong Kong, 47.82.117.8, 2 vCPU / 2 GB, instance `01ef7a060a274d33be47d29da47b01fd`).
- Addresses moved from `@groupekiva.com` to `@bykiva.com`.
- Outgoing mail to the internet is relayed through **Alibaba DirectMail**
  (`smtpdm-ap-southeast-1.aliyun.com:465`). DirectMail only lets an account send as itself,
  so each `@bykiva.com` sender has its own SMTP password (`outbound.relay.sender_passwords`).
- Webmail (Outlook/Gmail-style) is served at **https://mail.bykiva.com** (nginx → `127.0.0.1:9090`).

## Layout on the server

| What | Where |
|---|---|
| Binary | `/root/mailserver_deploy/usr/bin/mailserver` (previous builds kept as `mailserver.bak-*`) |
| Config | `/root/mailserver_deploy/etc/mailserver/config.json` (backups `config.json.bak-*`) |
| Mailboxes (Maildir++) | `/root/mailserver_deploy/etc/mailserver/maildir/<user>/` (folders are `.Sent`, `.Drafts`, …) |
| Per-user webmail settings | `maildir/<user>/.webmail.json` |
| Webmail UI files | `/root/mailserver_deploy/etc/mailserver/webmail/` |
| Outbound queue | `/root/mailserver_deploy/etc/mailserver/spool/{new,delivered,failed}` |
| Logs | `journalctl -u mailserver` |
| nginx site | `/etc/nginx/sites-available/mail.bykiva.com` |
| Old LWS backup (uploaded) | `/root/lws-mail-backup/` |

Admin API: `POST http://127.0.0.1:9090/api/login {"password": <admin token>}` → Bearer session.

## Accounts

`bme.sante`, `construction`, `dga_vyathekwahi`, `drh`, `logistics`, `test` — all `@bykiva.com`.
Passwords are kept privately (not in this repo).

## Done

- **Server repair:** DB permissions fixed, missing `users` columns added, admin API login fixed.
- **Accounts:** created via the API; old LWS inbox and folders (Sent/Drafts/Trash/Junk) imported.
- **IMAP rewrite:** ranges, full FETCH/BODYSTRUCTURE/ENVELOPE, SEARCH, STORE, COPY/MOVE, APPEND,
  folders with special-use flags, persistent UIDs/UIDVALIDITY. Works with Thunderbird/Outlook
  (IMAP 993 SSL, SMTP 587 STARTTLS).
- **Security:** open relay closed, no AUTH before TLS, sender must match login, passwords no longer logged.
- **Incoming mail fix:** SMTP no longer strips whitespace (Gmail mail showed as raw code); 23 damaged
  messages repaired (backup in `/root/maildir-header-repair-20261118-1931`).
- **Outgoing to Gmail works:** per-sender DirectMail SMTP passwords; DKIM module installed.
- **Webmail v1:** login, folders, list, reading pane (sandboxed HTML), compose/reply/forward,
  attachments, drafts, move/delete/flag, search, mobile layout.

## In progress — Gmail-style features

1. Conversation threads
2. Labels and stars
3. Archive (Archive folder)
4. Undo send (10 s)
5. Snooze (Snoozed folder, returns to Inbox automatically)
6. Search operators: `from:` `to:` `subject:` `label:` `has:attachment` `is:unread|read|starred` `before:` `after:`
7. Signature and out-of-office auto-reply
8. Contacts with autocomplete
9. Keyboard shortcuts

## Next / open items

- Owner actions: rotate the Alibaba AccessKey (it was exposed); optionally add DNS TXT
  `mail._domainkey` (value in `~/bykiva-dkim-dns-record.txt`) — Alibaba already signs with its own DKIM.
- Delete old spam rejects: `find /root/mailserver_deploy/etc/mailserver/spool/failed -type f -delete` (~141 MB).
- New accounts must also get a DirectMail SMTP password (automate in account creation).
- Check the Alibaba firewall rules (`aliyun swas-open ListFirewallRules …`).
- On the owner's PC, `/etc/hosts` maps `mail.bykiva.com` to `127.0.0.1` — remove that line to use the webmail there.
