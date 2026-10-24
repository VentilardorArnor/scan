#!/usr/bin/env python3
"""Grava a linha de provisionamento de UMA placa em main/provisioning.h.

Lê o CSV baixado na tela Provisionamento (colunas cn;codigo_claim_etiqueta;
linha_firmware;valida_ate_utc;modelo) e atualiza a definição AGRO_PROVISIONING,
mantendo AGRO_BLE_POP. Nunca imprime o token nem o código de claim.

    python3 tools/importar_linha.py ~/Downloads/placas-AAAA-MM-DD.csv [--cn agp-xxxx]

Só usa a biblioteca padrão do Python.
"""
import argparse
import csv
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_HEADER = os.path.join(ROOT, "main", "provisioning.h")
LINE_RE = re.compile(r"^agro1://([A-Za-z0-9._-]{3,64}):([^\s]{8,64})@([A-Za-z0-9.-]+(?::\d{1,5})?)$")
DEFINE_RE = re.compile(r"^\s*(?:/\*\s*)?#define\s+AGRO_PROVISIONING\b.*$", re.M)
EXPECTED_HOST = "api.smartagrobr.com"


def fail(msg: str) -> None:
    print(f"erro: {msg}", file=sys.stderr)
    sys.exit(1)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("csv", help="CSV baixado na tela Provisionamento")
    ap.add_argument("--cn", help="CN da placa, se o CSV tiver mais de uma")
    ap.add_argument("--header", default=DEFAULT_HEADER, help=argparse.SUPPRESS)
    args = ap.parse_args()

    with open(args.csv, newline="", encoding="utf-8-sig") as f:
        rows = list(csv.DictReader(f, delimiter=";"))
    if not rows or "linha_firmware" not in rows[0] or "cn" not in rows[0]:
        fail("não parece o CSV da tela Provisionamento (faltam as colunas cn/linha_firmware)")
    if args.cn:
        rows = [r for r in rows if r["cn"] == args.cn]
        if not rows:
            fail(f"CN {args.cn} não está no CSV")
    elif len(rows) > 1:
        fail(f"o CSV tem {len(rows)} placas; escolha uma com --cn")
    row = rows[0]

    line = row["linha_firmware"].strip()
    m = LINE_RE.match(line)
    if not m:
        fail("a linha não tem o formato agro1://<cn>:<token>@<host>")
    cn, _token, host = m.groups()
    if cn != row["cn"]:
        fail("o CN da linha não bate com a coluna cn")
    if host != EXPECTED_HOST:
        print(f"aviso: host da linha é {host}, não {EXPECTED_HOST}")

    header = args.header
    text = open(header, encoding="utf-8").read() if os.path.exists(header) else "#pragma once\n"
    if not re.search(r'^#define\s+AGRO_BLE_POP\s+"[^"]{8,}"', text, re.M):
        print("aviso: AGRO_BLE_POP ausente ou curta em provisioning.h (o BLE não abre sem ela)")
    new_define = f'#define AGRO_PROVISIONING "{line}"'
    if DEFINE_RE.search(text):
        text = DEFINE_RE.sub(lambda _: new_define, text, count=1)
    else:
        text = text.rstrip("\n") + "\n\n" + new_define + "\n"

    fd = os.open(header, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write(text)
    os.chmod(header, 0o600)

    ignored = subprocess.run(["git", "-C", os.path.dirname(header), "check-ignore", "-q", header],
                             capture_output=True).returncode == 0
    print(f"linha gravada em {os.path.relpath(header, ROOT)} (permissão 600)")
    print(f"  CN:          {cn}")
    print(f"  host da API: {host}")
    print(f"  válida até:  {row.get('valida_ate_utc', '?')} (UTC)")
    print(f"  fora do git: {'sim' if ignored else 'NÃO — confira o .gitignore antes de qualquer commit'}")


if __name__ == "__main__":
    main()
