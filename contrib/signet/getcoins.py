#!/usr/bin/env python3
# Copyright (c) 2020-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import argparse
import ipaddress
import requests
import subprocess
import time
from urllib.parse import urlparse

CONNECT_TIMEOUT = 5
READ_TIMEOUT = 15
TOTAL_RESPONSE_TIMEOUT = 30
MAX_RESPONSE_BYTES = 256 * 1024

parser = argparse.ArgumentParser(
    description='Script to get coins from an explicitly selected ConnectCoin-compatible faucet.',
    epilog='No public ConnectCoin faucet is configured. Use regtest for local testing. You may need to start with double-dash (--) when providing connectcoin-cli arguments.',
)
parser.add_argument('-c', '--cmd', dest='cmd', default='connectcoin-cli', help='connectcoin-cli command to use')
parser.add_argument('-f', '--faucet', dest='faucet', required=True, help='HTTPS URL of a ConnectCoin-compatible faucet')
parser.add_argument('-a', '--addr', dest='addr', default='', help='ConnectCoin address to which the faucet should send')
parser.add_argument('-p', '--password', dest='password', default='', help='Faucet password, if any')
parser.add_argument('-n', '--amount', dest='amount', default='0.001', help='Amount to request (0.001-0.1, default is 0.001)')
parser.add_argument('--allow-insecure-localhost', action='store_true', help='Allow an HTTP faucet only on localhost or a loopback IP (development only)')
parser.add_argument('connectcoin_cli_args', nargs='*', help='Arguments to pass on to connectcoin-cli (default: -signet)')

args = parser.parse_args()

if args.connectcoin_cli_args == []:
    args.connectcoin_cli_args = ['-signet']


def is_loopback_host(hostname):
    if hostname.casefold() == 'localhost':
        return True
    try:
        return ipaddress.ip_address(hostname).is_loopback
    except ValueError:
        return False


def validate_faucet_url(url):
    try:
        parsed = urlparse(url)
        hostname = parsed.hostname
    except ValueError as e:
        raise SystemExit(f'Invalid faucet URL: {e}')
    if not hostname or parsed.username is not None or parsed.password is not None:
        raise SystemExit('The faucet must be a URL without embedded credentials.')
    if parsed.scheme == 'https':
        return
    if parsed.scheme == 'http' and args.allow_insecure_localhost and is_loopback_host(hostname):
        return
    raise SystemExit('The faucet must use HTTPS. For a local development faucet only, use an HTTP loopback URL together with --allow-insecure-localhost.')


def read_limited_response(response):
    content_length = response.headers.get('content-length')
    if content_length is not None:
        try:
            if int(content_length) > MAX_RESPONSE_BYTES:
                raise SystemExit(f'Faucet response exceeds the {MAX_RESPONSE_BYTES}-byte limit.')
        except ValueError:
            pass

    chunks = []
    size = 0
    deadline = time.monotonic() + TOTAL_RESPONSE_TIMEOUT
    for chunk in response.iter_content(chunk_size=16 * 1024):
        if time.monotonic() > deadline:
            raise SystemExit(f'Faucet response exceeded the {TOTAL_RESPONSE_TIMEOUT}-second total timeout.')
        size += len(chunk)
        if size > MAX_RESPONSE_BYTES:
            raise SystemExit(f'Faucet response exceeds the {MAX_RESPONSE_BYTES}-byte limit.')
        chunks.append(chunk)
    body = b''.join(chunks)
    try:
        return body.decode(response.encoding or 'utf-8', errors='replace')
    except LookupError:
        return body.decode('utf-8', errors='replace')


def connectcoin_cli(rpc_command_and_params):
    argv = [args.cmd] + args.connectcoin_cli_args + rpc_command_and_params
    try:
        return subprocess.check_output(argv).strip().decode()
    except FileNotFoundError:
        raise SystemExit(f"The binary {args.cmd} could not be found")
    except subprocess.CalledProcessError:
        cmdline = ' '.join(argv)
        raise SystemExit(f"-----\nError while calling {cmdline} (see output above).")


validate_faucet_url(args.faucet)

if args.addr == '':
    # get address for receiving coins
    args.addr = connectcoin_cli(['getnewaddress', 'faucet', 'bech32'])

data = {'address': args.addr, 'password': args.password, 'amount': args.amount}
session = requests.Session()

try:
    res = session.post(
        args.faucet,
        data=data,
        allow_redirects=False,
        stream=True,
        timeout=(CONNECT_TIMEOUT, READ_TIMEOUT),
    )
    if 300 <= res.status_code < 400:
        raise SystemExit('Faucet redirects are disabled; provide the final HTTPS endpoint explicitly.')
    response_text = read_limited_response(res)
except requests.exceptions.RequestException as e:
    raise SystemExit(f'Unexpected error when contacting faucet: {e}')
finally:
    if 'res' in locals():
        res.close()

# Display the output as per the returned status code
if res.ok:
    print(response_text)
elif res.status_code == 404:
    print('The specified faucet URL does not exist. Please check for any server issues/typo.')
elif res.status_code == 429:
    print('The selected faucet rate-limited this request. Check that service\'s policy before trying again.')
else:
    print(f'Returned Error Code {res.status_code}\n{response_text}\n')
    print('Please check the provided arguments for their validity and/or any possible typo.')
