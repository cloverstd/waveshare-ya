#!/usr/bin/env python3
"""Local USB Wi-Fi regression. Uses saved credentials without reading or exporting them."""
import argparse,json,pathlib,time
from device import Device

def main():
 p=argparse.ArgumentParser();p.add_argument('--port',required=True);p.add_argument('--image',type=pathlib.Path,default=pathlib.Path('build-wifi/waveshare-ya.bin'));p.add_argument('--out',type=pathlib.Path,required=True);a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
 d=Device(a.port);result={}
 def rpc(x):return d.request(x)
 def wait(predicate,timeout=35):
  end=time.monotonic()+timeout
  while time.monotonic()<end:
   s=rpc({'op':'status'})
   if predicate(s):return s
   time.sleep(.8)
  raise AssertionError('Expected device state did not arrive')
 def safe(s):return {k:s.get(k) for k in ('version','build_sha256','wifi_connected','wifi_rssi','internal_heap','internal_largest','uptime_ms','pending','reset_reason')}
 try:
  expected=a.image.read_bytes()[176:208].hex();initial=wait(lambda s:not s['pending'] and s['wifi_connected'] and s['build_sha256']==expected and s['uptime_ms']>35000,60);result['initial']=safe(initial)
  w=rpc({'op':'wifi_state'});assert w['current_id'] and w['saved'];identity=w['current_id'];original_ssid=next(n['ssid'] for n in w['saved'] if n['id']==identity)
  result['capabilities']={'ap_k':w['k'],'ap_v':w['v'],'saved_count':len(w['saved'])}
  rpc({'op':'page','id':3});time.sleep(3);d.screenshot(a.out/'networks.png');print('Network list captured',flush=True)
  rpc({'op':'wifi_view','view':1,'id':identity});time.sleep(.6);d.screenshot(a.out/'details.png')
  rpc({'op':'wifi_view','view':2,'id':0});time.sleep(.6);d.screenshot(a.out/'password.png')
  rpc({'op':'page','id':0});time.sleep(.5);result['screenshots']='passed'
  # Wrong credentials must never replace the known-good profile.
  assert rpc({'op':'wifi','ssid':original_ssid,'password':'YA-invalid-validation-password-040'})['ok']
  time.sleep(1);after=wait(lambda s:s['wifi_connected'] and s['wifi_ssid']==original_ssid,45)
  w=rpc({'op':'wifi_state'});assert w['current_id']==identity and len(w['saved'])==result['capabilities']['saved_count']
  result['wrong_password_restored']=safe(after);print('Wrong-password recovery passed',flush=True)
  assert rpc({'op':'set','key':'wifi','value':0})['ok'];wait(lambda s:not s['wifi_connected'],10)
  assert rpc({'op':'set','key':'wifi','value':1})['ok'];after=wait(lambda s:s['wifi_connected'] and s['wifi_ssid']==original_ssid,40)
  result['off_on']=safe(after);print('Wi-Fi off/on passed',flush=True)
  # Reuse stable IDs even after scan ordering changes.
  assert rpc({'op':'wifi_profile','action':'auto','id':identity,'enabled':False})['ok'];time.sleep(.8)
  assert next(n for n in rpc({'op':'wifi_state'})['saved'] if n['id']==identity)['automatic'] is False
  assert rpc({'op':'wifi_profile','action':'auto','id':identity,'enabled':True})['ok'];time.sleep(.8)
  assert next(n for n in rpc({'op':'wifi_state'})['saved'] if n['id']==identity)['automatic'] is True
  result['automatic_toggle']='passed'
  heaps=[]
  for i in range(12):
   rpc({'op':'page','id':3});time.sleep(.35);rpc({'op':'page','id':0});time.sleep(.35);s=rpc({'op':'status'});heaps.append({'internal':s['internal_heap'],'largest':s['internal_largest']})
  result['lifecycle']=heaps
  assert min(v['internal'] for v in heaps[3:])>=heaps[3]['internal']-2048
  final=rpc({'op':'status'});assert final['build_sha256']==initial['build_sha256'] and final['uptime_ms']>initial['uptime_ms'];result['final']=safe(final)
  print('12 page lifecycle cycles passed',flush=True)
 finally:
  (a.out/'regression.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n');d.close()
if __name__=='__main__':main()
