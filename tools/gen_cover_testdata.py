#!/usr/bin/env python3
"""
gen_cover_testdata.py -- synthetic tagged files for covertag.c.

One minimal file per container, each carrying a known title, artist,
album and picture, so a parser change can be checked against something
with an expected answer rather than against whatever is on the card.
Deliberately includes the awkward shapes: a FLAC whose back cover comes
before its front cover, an MP4 written with the 64-bit atom size form,
an Ogg whose comment packet spans pages, a FLAC with an illegal ID3v2
tag bolted on the front.

Companion to the fuzz corpus described in CLAUDE.md -- these are the
valid inputs; mutate them for the invalid ones.

Usage:  ./tools/gen_cover_testdata.py [outdir]

SPDX-License-Identifier: MIT
"""

import struct, base64, zlib, os
JPEG = b'\xff\xd8\xff\xe0' + b'JFIF-fake-cover' * 20 + b'\xff\xd9'
PNG  = b'\x89PNG\r\n\x1a\n' + b'fake' * 10

def picblock(img, ptype=3, mime=b'image/jpeg', desc='cover'.encode()):
    return (struct.pack('>I', ptype) + struct.pack('>I', len(mime)) + mime +
            struct.pack('>I', len(desc)) + desc +
            struct.pack('>IIII', 500, 500, 24, 0) +
            struct.pack('>I', len(img)) + img)

def vc(comments, vendor=b'ref libFLAC'):
    out = struct.pack('<I', len(vendor)) + vendor + struct.pack('<I', len(comments))
    for c in comments:
        c = c.encode('utf-8')
        out += struct.pack('<I', len(c)) + c
    return out

# ---- FLAC: streaminfo, vorbis comment, a back cover, then a front cover
def flac(path, img=JPEG, prefix=b''):
    b = prefix + b'fLaC'
    def blk(t, body, last=False):
        return bytes([(0x80 if last else 0) | t]) + len(body).to_bytes(3,'big') + body
    b += blk(0, b'\0'*34)
    b += blk(4, vc(['TITLE=Hoppípolla', 'ARTIST=Sigur Rós', 'ALBUM=Takk...']))
    b += blk(6, picblock(PNG, ptype=4))     # back cover first, deliberately
    b += blk(6, picblock(img, ptype=3), last=True)
    open(path,'wb').write(b + b'\xff\xf8'*100)

# ---- MP4
def atom(t, body): return struct.pack('>I', len(body)+8) + t + body
def data_atom(kind, payload):
    return atom(b'data', struct.pack('>I', kind) + b'\0'*4 + payload)
def mp4(path, large=False):
    ilst = (atom(b'\xa9nam', data_atom(1, 'Águas de Março'.encode())) +
            atom(b'\xa9ART', data_atom(1, 'Elis Regina'.encode())) +
            atom(b'\xa9alb', data_atom(1, 'Elis & Tom'.encode())) +
            atom(b'covr', data_atom(13, JPEG)))
    meta = atom(b'meta', b'\0'*4 + atom(b'ilst', ilst))
    udta = atom(b'udta', meta)
    moov_body = atom(b'mvhd', b'\0'*100) + udta
    if large:   # 64-bit size form
        moov = struct.pack('>I', 1) + b'moov' + struct.pack('>Q', len(moov_body)+16) + moov_body
    else:
        moov = atom(b'moov', moov_body)
    open(path,'wb').write(atom(b'ftyp', b'M4A isom') + atom(b'free', b'\0'*8) + moov)

# ---- Ogg: comment packet spanning pages via 255-byte segments
def ogg_pages(packets, serial=0xC0FFEE):
    out = b''
    seq = 0
    for pkt in packets:
        segs = [pkt[i:i+255] for i in range(0, len(pkt), 255)] or [b'']
        if len(segs[-1]) == 255: segs.append(b'')   # terminating short segment
        while segs:
            take = segs[:255]; segs = segs[255:]
            body = b''.join(take)
            table = bytes(len(s) for s in take)
            hdr = (b'OggS' + bytes([0,0]) + b'\0'*8 + struct.pack('<I', serial)
                   + struct.pack('<I', seq) + b'\0'*4 + bytes([len(take)]) + table)
            out += hdr + body
            seq += 1
    return out

def ogg(path, opus=False, img=JPEG):
    pic = base64.b64encode(picblock(img)).decode()
    if opus:
        ident = b'OpusHead' + b'\0'*11
        comment = b'OpusTags' + vc(['TITLE=Björk','ARTIST=Homogenic','ALBUM=Jóga',
                                    'METADATA_BLOCK_PICTURE=' + pic], b'libopus')
    else:
        ident = b'\x01vorbis' + b'\0'*23
        comment = b'\x03vorbis' + vc(['TITLE=Björk','ARTIST=Homogenic','ALBUM=Jóga',
                                      'METADATA_BLOCK_PICTURE=' + pic])
    open(path,'wb').write(ogg_pages([ident, comment, b'\x05vorbis'+b'\0'*300]))

# ---- WAV with an id3 chunk
def id3v2(frames):
    body = b''
    for fid, payload in frames:
        body += fid + struct.pack('>I', len(payload)) + b'\0\0' + payload
    size = len(body)
    ss = bytes([(size>>21)&0x7f, (size>>14)&0x7f, (size>>7)&0x7f, size&0x7f])
    return b'ID3\x03\x00\x00' + ss + body
def wav(path):
    apic = b'\x00' + b'image/jpeg\x00' + b'\x03' + b'front\x00' + JPEG
    tag = id3v2([(b'TIT2', b'\x00Fanfare'), (b'TPE1', b'\x00Caf\xe9 Tacvba'), (b'APIC', apic)])
    if len(tag) & 1: tag += b'\0'
    data = b'RIFF' + struct.pack('<I', 4+8+16+8+len(tag)+8+4) + b'WAVE'
    data += b'fmt ' + struct.pack('<I',16) + b'\0'*16
    data += b'id3 ' + struct.pack('<I', len(tag)) + tag
    data += b'data' + struct.pack('<I', 4) + b'\0'*4
    open(path,'wb').write(data)

# ---- MP3, unchanged path
def mp3(path):
    apic = b'\x00' + b'image/png\x00' + b'\x03' + b'\x00' + PNG
    open(path,'wb').write(id3v2([(b'TIT2', b'\x00Blue Monday'), (b'APIC', apic)]) + b'\xff\xfb'*100)

import sys
out = sys.argv[1] if len(sys.argv) > 1 else 't'
os.makedirs(out, exist_ok=True)
flac(out + '/a.flac'); flac(out + '/id3prefix.flac', prefix=id3v2([(b'TIT2', b'\x00ignored')]))
mp4(out + '/a.m4a'); mp4(out + '/large.m4a', large=True)
ogg(out + '/a.ogg'); ogg(out + '/a.opus', opus=True)
wav(out + '/a.wav'); mp3(out + '/a.mp3')
open(out + '/junk.bin','wb').write(os.urandom(4096))
open(out + '/empty.flac','wb').write(b'fLaC')
print('generated', sorted(os.listdir(out)))
