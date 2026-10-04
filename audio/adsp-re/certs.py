# SPDX-License-Identifier: GPL-2.0-or-later
# List the X.509 certificates embedded in a signed Qualcomm image (e.g. adsp.mdt or a
# NON-HLOS segment): subject + SHA-256 fingerprint, to compare signing chains.
# usage: python3 certs.py <file>   (needs openssl)
import subprocess,sys
d=open(sys.argv[1],'rb').read(); i=0
while True:
    i=d.find(b'\x30\x82',i)
    if i<0: break
    l=int.from_bytes(d[i+2:i+4],'big')+4
    r=subprocess.run(['openssl','x509','-inform','DER','-noout','-subject','-fingerprint','-sha256'],input=d[i:i+l],capture_output=True)
    if r.returncode==0: print(r.stdout.decode().replace('\n',' | ')[:400]); i+=l
    else: i+=2
