import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys

import detools
import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import delta_release as delta


def test_recent_bases_skips_drafts_prereleases_and_future():
    entries = [dict(tagName='v'+v, isDraft=d, isPrerelease=p) for v,d,p in [
        ('0.6.0',False,False), ('0.5.1',False,False), ('0.5.0',False,False),
        ('0.4.0',False,False), ('0.3.0',False,False), ('0.5.2',True,False),
        ('0.5.3',False,True), ('0.7.0',False,False)]]
    assert [r['tagName'] for r in delta.recent_bases('0.6.0', entries)] == ['v0.5.1','v0.5.0','v0.4.0']


def test_delta_round_trip_includes_signed_tail():
    base = bytes(range(256))*128
    target = base[:1000] + b'changed firmware' + base[1016:-4096] + b'signature tail'*300
    patch = delta.make_patch(base, target)
    assert len(patch) < len(target)
    out = io.BytesIO()
    detools.apply_patch(io.BytesIO(base),io.BytesIO(patch),out)
    assert out.getvalue() == target


@pytest.fixture(scope='module')
def decoder(tmp_path_factory):
    cc = shutil.which('cc')
    component = ROOT / 'managed_components/espressif__esp_delta_ota'
    if not cc or not component.exists(): pytest.skip('Fetch pinned component and native compiler first')
    tmp = tmp_path_factory.mktemp('decoder')
    (tmp/'esp_err.h').write_text('#pragma once\n#include <stddef.h>\n#include <stdint.h>\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_INVALID_ARG 258\nstatic inline const char *esp_err_to_name(int e){(void)e;return "error";}\n')
    (tmp/'esp_log.h').write_text('#pragma once\n#define ESP_LOGE(...) ((void)0)\n')
    (tmp/'esp_idf_version.h').write_text('#define ESP_IDF_VERSION_VAL(a,b,c) (((a)<<16)|((b)<<8)|(c))\n#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(5,5,5)\n')
    binary=tmp/'decode'
    subprocess.run([cc,'-std=c99','-O1','-g','-fsanitize=address,undefined',
        '-DDETOOLS_CONFIG_FILE_IO=0','-DDETOOLS_CONFIG_COMPRESSION_NONE=0',
        '-DDETOOLS_CONFIG_COMPRESSION_LZMA=0','-DDETOOLS_CONFIG_COMPRESSION_CRLE=0',
        '-I',str(tmp),'-I',str(component/'include'),'-I',str(component/'detools/c'),
        '-I',str(component/'detools/c/heatshrink'),str(ROOT/'tests/delta_decoder_test.c'),
        str(component/'src/esp_delta_ota.c'),str(component/'detools/c/detools.c'),
        str(component/'detools/c/heatshrink/heatshrink_decoder.c'),'-o',str(binary)], check=True)
    return binary


@pytest.mark.parametrize('chunk',[1,7,4096])
def test_device_decoder_matches_generator(decoder,tmp_path,chunk):
    base=bytes(range(256))*128
    target=base[:4567]+b'NEW IMAGE'+base[4576:]+b'new RSA signature bytes'
    patch=delta.make_patch(base,target)
    for name,data in [('base',base),('target',target),('patch',patch)]: (tmp_path/name).write_bytes(data)
    subprocess.run([str(decoder),str(tmp_path/'base'),str(tmp_path/'patch'),str(tmp_path/'target'),str(chunk)],check=True)


@pytest.mark.parametrize('fault',['truncated','corrupt','wrong_base','oversize'])
def test_decoder_rejects_faults_without_accepting_image(decoder,tmp_path,fault):
    base=bytes(range(256))*128; target=base+b'new signed tail'
    patch=delta.make_patch(base,target)
    if fault=='truncated': patch=patch[:-3]
    if fault=='corrupt': patch=b'\xff'+patch[1:]
    if fault=='wrong_base': base=bytes(len(base))
    if fault=='oversize': target=target[:-100]
    for name,data in [('base',base),('target',target),('patch',patch)]: (tmp_path/name).write_bytes(data)
    result=subprocess.run([str(decoder),str(tmp_path/'base'),str(tmp_path/'patch'),str(tmp_path/'target'),'7'])
    assert result.returncode==1


def fake_image(version):
    import struct
    data=bytearray(bytes(range(256))*128)
    data[0]=0xe9
    struct.pack_into('<I',data,32,0xabcd5432)
    data[48:80]=version.encode().ljust(32,b'\0')
    data[80:112]=b'waveshare-ya'.ljust(32,b'\0')
    struct.pack_into('<I',data,288,0x59414142)
    data[292:332]=delta.BOARD.encode().ljust(40,b'\0')
    data[332:348]=delta.LAYOUT.encode().ljust(16,b'\0')
    return bytes(data)


def test_package_attaches_verified_index_and_rejects_tampering(tmp_path,monkeypatch):
    target=fake_image('0.6.0');base=fake_image('0.5.1')
    (tmp_path/'waveshare-ya-0.6.0.bin').write_bytes(target)
    base_path=tmp_path/'base.bin';base_path.write_bytes(base)
    m=dict(product=delta.PRODUCT,board=delta.BOARD,layout=delta.LAYOUT,
           version='0.6.0',size=len(target),sha256=delta.digest(target))
    (tmp_path/'manifest.json').write_text(json.dumps(m))
    # Signature verification has its own real-image integration check; isolate packaging here.
    monkeypatch.setattr(delta,'verify_image',lambda p,k: delta.image_metadata(p.read_bytes()))
    index=delta.package_delta(tmp_path,[base_path],tmp_path/'key')
    m=json.loads((tmp_path/'manifest.json').read_text())
    delta.verify_delta(tmp_path,m)
    assert index['patches'][0]['base_sha256']==delta.digest(base)
    assert isinstance(m['delta_index_url'],str)  # v1 remains flat
    patch=tmp_path/'waveshare-ya-0.5.1-to-0.6.0.patch'
    patch.write_bytes(patch.read_bytes()[:-1])
    with pytest.raises(ValueError,match='Patch digest'): delta.verify_delta(tmp_path,m)


def test_empty_history_can_publish_full_only(tmp_path,monkeypatch):
    data=fake_image('0.6.0');(tmp_path/'waveshare-ya-0.6.0.bin').write_bytes(data)
    m=dict(product=delta.PRODUCT,board=delta.BOARD,layout=delta.LAYOUT,
           version='0.6.0',size=len(data),sha256=delta.digest(data))
    (tmp_path/'manifest.json').write_text(json.dumps(m))
    monkeypatch.setattr(delta,'verify_image',lambda p,k: '0.6.0')
    assert delta.package_delta(tmp_path,[],tmp_path/'key')['patches']==[]
    delta.verify_delta(tmp_path,json.loads((tmp_path/'manifest.json').read_text()))
