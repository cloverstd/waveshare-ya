#!/usr/bin/env python3
"""Private, temporary TLS fixture. No deployment or trust-store changes."""

import argparse, pathlib, datetime, ipaddress, ssl, http.server, functools, json, os
from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa


def setup(ip):
    p = pathlib.Path("private/tls")
    p.mkdir(parents=True, exist_ok=True)
    now = datetime.datetime.now(datetime.timezone.utc)
    ca_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    ca_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "YA temporary QA CA")])
    ca = (
        x509.CertificateBuilder()
        .subject_name(ca_name)
        .issuer_name(ca_name)
        .public_key(ca_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=7))
        .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
        .sign(ca_key, hashes.SHA256())
    )
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, ip)])
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(ca_name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=7))
        .add_extension(
            x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address(ip))]),
            critical=False,
        )
        .sign(ca_key, hashes.SHA256())
    )
    pem = ca.public_bytes(serialization.Encoding.PEM).decode()
    (p / "ca.crt").write_text(pem)
    (p / "server.crt").write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    (p / "server.key").write_bytes(
        key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        )
    )
    os.chmod(p / "server.key", 0o600)
    pathlib.Path("private/test-ca.h").write_text(
        '#define YA_TEST_CA R"CERT(' + pem + ')CERT"\n'
    )
    pathlib.Path("artifacts/https").mkdir(parents=True, exist_ok=True)
    print("Temporary fixture CA generated; enable only in test build")


def serve():
    c = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    c.load_cert_chain("private/tls/server.crt", "private/tls/server.key")
    h = http.server.ThreadingHTTPServer(
        ("0.0.0.0", 8443),
        functools.partial(
            http.server.SimpleHTTPRequestHandler, directory="artifacts/https"
        ),
    )
    h.socket = c.wrap_socket(h.socket, server_side=True)
    h.serve_forever()


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--setup")
    a = p.parse_args()
    setup(a.setup) if a.setup else serve()
