#!/usr/bin/env python3
"""Give a mailbox the right to send through Alibaba DirectMail.

DirectMail only lets an SMTP login send as itself, so every address needs its own
registered sender and its own SMTP password. This registers the address (if needed),
sets a fresh password, and stores it in config.json under
outbound.relay.sender_passwords, which OutboundWorker re-reads on every send.

Usage: alibaba_provision_sender.py <email> --config /path/to/config.json [--quiet]
"""

import argparse
import base64
import fcntl
import hashlib
import hmac
import json
import os
import secrets
import string
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

API_VERSION = "2015-11-23"
PAGE_SIZE = 50


def percent_encode(value):
    return urllib.parse.quote(str(value), safe="").replace("+", "%20").replace("*", "%2A").replace("%7E", "~")


def call_api(endpoint, key_id, key_secret, action, extra):
    params = {
        "Action": action, "AccessKeyId": key_id, "Format": "JSON",
        "SignatureMethod": "HMAC-SHA1", "SignatureNonce": uuid.uuid4().hex,
        "SignatureVersion": "1.0", "Version": API_VERSION,
        "Timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        **extra,
    }
    canonical = "&".join(f"{percent_encode(k)}={percent_encode(params[k])}" for k in sorted(params))
    to_sign = "GET&" + percent_encode("/") + "&" + percent_encode(canonical)
    digest = hmac.new((key_secret + "&").encode(), to_sign.encode(), hashlib.sha1).digest()
    params["Signature"] = base64.b64encode(digest).decode()
    url = f"https://{endpoint}/?{urllib.parse.urlencode(params)}"
    try:
        with urllib.request.urlopen(urllib.request.Request(url), timeout=15) as resp:
            return json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        try:
            return json.loads(e.read().decode())
        except ValueError:
            return {"Code": f"HTTP{e.code}", "Message": str(e)}


def find_mail_address_id(api, email):
    page = 1
    while True:
        listing = api("QueryMailAddressByParam", {"PageSize": PAGE_SIZE, "PageNo": page})
        if "data" not in listing:
            raise RuntimeError(f"QueryMailAddressByParam failed: {listing}")
        for entry in listing["data"]["mailAddress"]:
            if entry["SenderAddress"].lower() == email:
                return entry["MailAddressId"]
        if page * PAGE_SIZE >= listing.get("TotalCount", 0):
            return None
        page += 1


def store_password(config_path, email, password):
    # Lock: batch account creation provisions several addresses at once.
    with open(config_path + ".lock", "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        with open(config_path) as f:
            cfg = json.load(f)
        relay = cfg.setdefault("outbound", {}).setdefault("relay", {})
        relay.setdefault("sender_passwords", {})[email] = password
        # Atomic replace so the mail server never reads a half-written file.
        fd, tmp = tempfile.mkstemp(dir=os.path.dirname(config_path), prefix=".config.", suffix=".json")
        with os.fdopen(fd, "w") as f:
            json.dump(cfg, f, indent=4)
        os.chmod(tmp, os.stat(config_path).st_mode)
        os.replace(tmp, config_path)


def provision(email, config_path):
    with open(config_path) as f:
        ali = json.load(f).get("alibaba_cloud", {})
    key_id, key_secret = ali.get("access_key_id"), ali.get("access_key_secret")
    if not key_id or not key_secret:
        raise RuntimeError("alibaba_cloud access key missing from config")
    endpoint = f"dm.{ali.get('region', 'ap-southeast-1')}.aliyuncs.com"
    api = lambda action, extra: call_api(endpoint, key_id, key_secret, action, extra)

    created = api("CreateMailAddress", {"AccountName": email, "Sendtype": "trigger"})
    if "Code" in created and created["Code"] != "InvalidMailAddressName.Duplicate":
        raise RuntimeError(f"CreateMailAddress failed: {created}")

    mail_address_id = find_mail_address_id(api, email)
    if mail_address_id is None:
        raise RuntimeError("address not found in DirectMail after creation")

    password = "".join(secrets.choice(string.ascii_letters + string.digits) for _ in range(16))
    modified = api("ModifyMailAddress", {"MailAddressId": mail_address_id, "AccountName": email, "Password": password})
    if "Code" in modified:
        raise RuntimeError(f"ModifyMailAddress failed: {modified}")

    store_password(config_path, email, password)
    return mail_address_id


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("email")
    parser.add_argument("--config", required=True)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()
    email = args.email.strip().lower()
    try:
        mail_address_id = provision(email, args.config)
    except RuntimeError as e:
        print(f"{email}: {e}", file=sys.stderr)
        sys.exit(1)
    if not args.quiet:
        print(f"provisioned {email} (MailAddressId {mail_address_id})")


if __name__ == "__main__":
    main()
