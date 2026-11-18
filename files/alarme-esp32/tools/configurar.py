#!/usr/bin/env python3
"""Gera os segredos e sincroniza os arquivos comuns dos dois firmwares.

Uso:
  python tools/configurar.py              # cria/atualiza tudo
  python tools/configurar.py --nova-chave # sorteia nova chave HMAC e novo PIN
  python tools/configurar.py --verificar  # só confere se as cópias estão em dia

Fluxo (detalhes no README):
  1. 1ª execução cria tools/segredos.json (chave e PIN aleatórios).
  2. Grave o controlador, leia o MAC no Serial, coloque em mac_controlador.
  3. Rode de novo, grave o chaveiro, leia o MAC dele, coloque em mac_transmissor.
  4. Rode de novo e regrave o controlador.
"""
import argparse, json, re, secrets, shutil, sys
from pathlib import Path

RAIZ = Path(__file__).resolve().parent.parent
CFG = RAIZ / "tools" / "segredos.json"
COMUM = RAIZ / "firmware" / "common"
DESTINOS = [RAIZ / "firmware" / "controlador", RAIZ / "firmware" / "transmissor"]
MAC_RE = re.compile(r"^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$")


def mac_c(mac):
    return ", ".join("0x" + b.upper() for b in mac.split(":"))


def novo_cfg(antigo=None):
    cfg = dict(antigo or {})
    cfg["chave_hex"] = secrets.token_hex(16)
    cfg["pin_ble"] = 100000 + secrets.randbelow(900000)   # sem zero à esquerda (octal em C!)
    cfg.setdefault("mac_controlador", "00:00:00:00:00:00")
    cfg.setdefault("mac_transmissor", "00:00:00:00:00:00")
    return cfg


def validar(cfg):
    if not re.fullmatch(r"[0-9a-fA-F]{32}", cfg["chave_hex"]):
        sys.exit("chave_hex deve ter 32 caracteres hexadecimais (16 bytes)")
    if not (100000 <= int(cfg["pin_ble"]) <= 999999):
        sys.exit("pin_ble deve ter 6 dígitos e não começar com 0")
    for k in ("mac_controlador", "mac_transmissor"):
        if not MAC_RE.match(cfg[k]):
            sys.exit(f"{k} inválido (use AA:BB:CC:DD:EE:FF)")


def render(cfg):
    b = bytes.fromhex(cfg["chave_hex"])
    linhas = ",\n  ".join(", ".join(f"0x{x:02X}" for x in b[i:i + 8]) for i in range(0, 16, 8))
    return f"""#pragma once
// GERADO por tools/configurar.py — NÃO edite e NÃO versione.
#include <stdint.h>

static const uint8_t CHAVE_SECRETA[16] = {{
  {linhas}
}};

#define PIN_BLE_PASSKEY {int(cfg["pin_ble"])}

static const uint8_t MAC_CONTROLADOR[6] = {{{mac_c(cfg["mac_controlador"])}}};
static const uint8_t MAC_TRANSMISSOR[6] = {{{mac_c(cfg["mac_transmissor"])}}};
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nova-chave", action="store_true")
    ap.add_argument("--verificar", action="store_true")
    a = ap.parse_args()

    if a.verificar:
        ok = True
        for d in DESTINOS:
            if (d / "protocolo.h").read_bytes() != (COMUM / "protocolo.h").read_bytes():
                print(f"DIFERENTE: {d.name}/protocolo.h"); ok = False
        sys.exit(0 if ok else 1)

    if CFG.exists() and not a.nova_chave:
        cfg = json.loads(CFG.read_text())
    else:
        cfg = novo_cfg(json.loads(CFG.read_text()) if CFG.exists() else None)
        CFG.write_text(json.dumps(cfg, indent=2) + "\n")
        print(f"Segredos novos gravados em {CFG.relative_to(RAIZ)}")
    validar(cfg)

    header = render(cfg)
    for d in DESTINOS:
        shutil.copyfile(COMUM / "protocolo.h", d / "protocolo.h")
        (d / "segredos.h").write_text(header)
        print(f"OK  {d.relative_to(RAIZ)}/ (protocolo.h + segredos.h)")
    if "00:00:00:00:00:00" in (cfg["mac_controlador"], cfg["mac_transmissor"]):
        print("\nATENÇÃO: ainda há MAC 00:00:... em tools/segredos.json. "
              "Siga o passo a passo do README (grave, leia o MAC no Serial, rode de novo).")


if __name__ == "__main__":
    main()
