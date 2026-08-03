#!/usr/bin/env python3
"""Drop-in DKIM signer: reads raw email from stdin, outputs DKIM-signed email to stdout.
Usage: python3 dkim_sign.py -d <domain> -s <selector> -k <private_key_path>"""
import sys
import argparse
import dkim

parser = argparse.ArgumentParser()
parser.add_argument("-d", required=True, help="Domain")
parser.add_argument("-s", required=True, help="Selector")
parser.add_argument("-k", required=True, help="Private key file path")
args = parser.parse_args()

raw_message = sys.stdin.buffer.read()

with open(args.k, "rb") as f:
    private_key = f.read()

sig_header = dkim.sign(
    raw_message,
    args.s.encode(),
    args.d.encode(),
    private_key,
)

if sig_header:
    # Prepend DKIM-Signature header before the original message
    signed = sig_header + raw_message
    sys.stdout.buffer.write(signed)
else:
    sys.stdout.buffer.write(raw_message)
