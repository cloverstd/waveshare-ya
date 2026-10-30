import importlib.util
import json
import pathlib
import uuid
import pytest
ROOT=pathlib.Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('data_gateway', ROOT/'tools/data_gateway.py')
gateway=importlib.util.module_from_spec(spec);spec.loader.exec_module(gateway)
def request(resource='system.health',op='get'):
 return dict(proto='ya.data',v=1,type='request',id=str(uuid.uuid4()),session=str(uuid.uuid4()),service='ya-local',scope='default',resource=resource,schema=1,params={},op=op)
def test_health_and_identity(monkeypatch):
 monkeypatch.delenv('YA_UPSTREAM_URL',raising=False)
 p=gateway.Provider();r=request();o=p.exchange(r)
 assert o['status']=='succeeded' and o['reply_to']==r['id'] and o['data']['commands'] is False
 r['scope']='wrong'
 with pytest.raises(PermissionError):p.exchange(r)
def test_weather_actual_adapter(monkeypatch):
 monkeypatch.delenv('YA_UPSTREAM_URL',raising=False)
 monkeypatch.setattr(gateway,'fetch',lambda *_:{'current':{'temperature_2m':21.5,'relative_humidity_2m':50,'weather_code':3,'time':1704067200},'daily':{'time':[1704067200],'temperature_2m_min':[10],'temperature_2m_max':[24],'weather_code':[3]}})
 p=gateway.Provider();r=request('weather.current');r['params']={'latitude':39.9,'longitude':116.4};o=p.exchange(r)
 assert o['data']['temperature_c']==21.5 and len(o['data']['forecast'])==1
 assert o['updated_at']==1704067200  # not re-stamped as current on cache reads
 assert p.exchange(r)['updated_at']==1704067200
def test_no_fabricated_stock_or_control(monkeypatch):
 monkeypatch.delenv('YA_UPSTREAM_URL',raising=False);monkeypatch.delenv('YA_RESOURCES_FILE',raising=False)
 p=gateway.Provider();assert p.exchange(request('market.quote'))['error']['code']=='missing'
 assert p.exchange(request('device.switch','command'))['error']['code']=='unsupported'
@pytest.mark.parametrize('raw',[b'{"a":1,"a":2}',b'{"a":NaN}',b'{}'*3000,b'{"a":[[[[[[1]]]]]]}',b'{"x":"\xff"}'])
def test_reject_invalid(raw):
 with pytest.raises((ValueError,UnicodeError)):gateway.decode(raw)

def test_quote_adapter_preserves_source_time_and_market(monkeypatch):
 monkeypatch.delenv('YA_UPSTREAM_URL',raising=False);monkeypatch.setenv('TWELVEDATA_API_KEY','test-only')
 monkeypatch.setattr(gateway,'fetch',lambda *_:{'close':'123.45','percent_change':'1.2','currency':'USD','mic_code':'XNAS','timestamp':1704067200,'is_market_open':False})
 p=gateway.Provider();r=request('market.quote');r['params']={'market':'XNAS','symbol':'AAPL'}
 out=p.exchange(r)
 assert out['data']['price']=='123.45' and out['data']['delay_s'] is None
 assert out['updated_at']==1704067200
 r['params']['market']='XSHG'
 assert p.exchange(r)['status']=='failed'

def test_browser_module_dependencies_are_served():
    import re
    pending = ['/app.mjs']
    seen = set()
    while pending:
        path = pending.pop()
        if path in seen:
            continue
        seen.add(path)
        handler = gateway.Handler.__new__(gateway.Handler)
        handler.path = path
        response = []
        handler.respond = lambda code, body, mime=None: response.append((code, body, mime))
        handler.do_GET()
        code, body, mime = response[0]
        assert code == 200, path
        assert mime == 'text/javascript'
        for dependency in re.findall(r"from\s*['\"]\./([^'\"]+)['\"]", body.decode()):
            pending.append('/' + dependency)
