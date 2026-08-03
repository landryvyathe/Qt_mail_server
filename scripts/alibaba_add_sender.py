#!/usr/bin/env python3
"""
Alibaba Cloud DirectMail - Create Sender Address
Called from mailserver after user creation.

Usage:
    python3 alibaba_add_sender.py <email_address> [--sendtype trigger|batch]
                                   [--access-key-id KEY] [--secret SECRET] [--region REGION]

If --access-key-id and --secret are not provided, reads from config.json.
"""

import sys
import json
import time
import hmac
import hashlib
import base64
import urllib.request
import urllib.parse
import uuid
import os


REGION = "ap-southeast-1"
ENDPOINT = "dm.ap-southeast-1.aliyuncs.com"
API_VERSION = "2015-11-23"
SENDTYPE = "trigger"


def percent_encode(s: str) -> str:
    """RFC 3986 percent-encoding for Alibaba Cloud API signing."""
    res = urllib.parse.quote(str(s), safe='')
    res = res.replace('+', '%20')
    res = res.replace('*', '%2A')
    res = res.replace('%7E', '~')
    return res


def compute_signature(params: dict, method: str, secret: str) -> str:
    """Sign parameters using HMAC-SHA1 per Alibaba Cloud API signature V1."""
    # Sort parameters lexicographically by encoded key
    sorted_keys = sorted(params.keys())
    canonical_query = '&'.join(
        f"{percent_encode(k)}={percent_encode(params[k])}"
        for k in sorted_keys
    )
    string_to_sign = method + "&" + percent_encode("/") + "&" + percent_encode(canonical_query)
    key = secret + "&"
    signature = hmac.new(key.encode('utf-8'),
                         string_to_sign.encode('utf-8'),
                         hashlib.sha1).digest()
    return base64.b64encode(signature).decode('utf-8')


def call_api(params: dict, access_key_id: str, access_key_secret: str) -> dict:
    """Make a signed API call to Alibaba Cloud."""
    # Common parameters
    params["AccessKeyId"] = access_key_id
    params["Format"] = "JSON"
    params["SignatureMethod"] = "HMAC-SHA1"
    params["SignatureNonce"] = uuid.uuid4().hex
    params["SignatureVersion"] = "1.0"
    params["Timestamp"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    params["Version"] = API_VERSION

    # Compute signature
    signature = compute_signature(params, "GET", access_key_secret)
    params["Signature"] = signature

    # Build URL
    query = urllib.parse.urlencode(params)
    url = f"https://{ENDPOINT}/?{query}"

    try:
        req = urllib.request.Request(url)
        req.add_header("User-Agent", "mailserver-admin/1.0")
        with urllib.request.urlopen(req, timeout=15) as resp:
            return json.loads(resp.read().decode('utf-8'))
    except urllib.error.HTTPError as e:
        return {"error": f"HTTP {e.code}: {e.reason}", "request_id": "N/A"}
    except Exception as e:
        return {"error": str(e), "request_id": "N/A"}


def create_sender_address(email: str, sendtype: str = SENDTYPE,
                          access_key_id: str = None,
                          access_key_secret: str = None,
                          region: str = REGION) -> dict:
    """Create a sender address in Alibaba DirectMail."""
    if not access_key_id or not access_key_secret:
        print("ERROR: Missing Alibaba Cloud AccessKey ID or Secret", file=sys.stderr)
        sys.exit(1)

    global ENDPOINT
    if region:
        ENDPOINT = "dm.%s.aliyuncs.com" % region

    params = {
        "Action": "CreateMailAddress",
        "AccountName": email,
        "Sendtype": sendtype,
    }

    result = call_api(params, access_key_id, access_key_secret)

    if "error" in result:
        print(f"ERROR: API call failed: {result['error']}", file=sys.stderr)
        sys.exit(1)

    if "Code" in result and result.get("Code") != "200":
        print(f"ERROR: Alibaba API error: {result.get('Code')} - {result.get('Message', '')}")
        sys.exit(1)

    return result


def load_config(config_path: str) -> dict:
    """Load alibaba_cloud credentials from config.json."""
    if not os.path.exists(config_path):
        print(f"ERROR: config not found: {config_path}", file=sys.stderr)
        sys.exit(1)
    with open(config_path, "r") as f:
        cfg = json.load(f)
    return cfg.get("alibaba_cloud", {})


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Create Alibaba DirectMail sender address")
    parser.add_argument("email", help="Email address to add as sender")
    parser.add_argument("--sendtype", default=SENDTYPE, choices=["trigger", "batch"],
                        help="Send type (default: trigger)")
    parser.add_argument("--access-key-id", default=None, help="Alibaba Cloud AccessKeyId")
    parser.add_argument("--secret", default=None, help="Alibaba Cloud AccessKeySecret")
    parser.add_argument("--region", default=REGION, help="Region ID (default: ap-southeast-1)")
    parser.add_argument("--config", default=None, help="Path to config.json")
    parser.add_argument("--quiet", action="store_true", help="Suppress output")

    args = parser.parse_args()
    access_key_id = args.access_key_id
    access_key_secret = args.secret

    if not access_key_id or not access_key_secret:
        # Try config.json
        config_path = args.config or os.path.join(os.path.dirname(__file__), "..", "config.json")
        alibaba_cfg = load_config(os.path.abspath(config_path))
        access_key_id = access_key_id or alibaba_cfg.get("access_key_id", "")
        access_key_secret = access_key_secret or alibaba_cfg.get("access_key_secret", "")
        if args.region == REGION and alibaba_cfg.get("region"):
            args.region = alibaba_cfg["region"]

    if not access_key_id or not access_key_secret:
        print("ERROR: Alibaba Cloud AccessKeyId and Secret required. "
              "Provide via --access-key-id/--secret or config.json", file=sys.stderr)
        sys.exit(1)

    result = create_sender_address(
        email=args.email,
        sendtype=args.sendtype,
        access_key_id=access_key_id,
        access_key_secret=access_key_secret,
        region=args.region,
    )

    if not args.quiet:
        print(f"OK: Created sender {args.email}")
        print(json.dumps(result))
