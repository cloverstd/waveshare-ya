"""Run actual gallery service persistence on a host; hardware calls are adapters."""
import pathlib
import subprocess
import sys
import pytest
ROOT=pathlib.Path(__file__).resolve().parents[1]
def test_gallery_store(tmp_path):
    cjson=ROOT/'.tools/esp-idf/components/json/cJSON'
    obj=tmp_path/'json.o'
    subprocess.run(['cc','-c',str(cjson/'cJSON.c'),'-o',str(obj)],check=True)
    binary=tmp_path/'gallery-store'
    subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-fsanitize=address,undefined','-I'+str(ROOT/'tests/gallery_host'),'-I'+str(ROOT/'main'),'-I'+str(cjson),str(ROOT/'tests/gallery_store_test.cpp'),str(obj),*(['-lcrypto'] if sys.platform!='darwin' else []),'-o',str(binary)],check=True)
    subprocess.run([str(binary),str(tmp_path/'flash')],check=True)
