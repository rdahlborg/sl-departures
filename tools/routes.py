import json, sys, time, urllib.request, collections, datetime as dt
S=sys.argv[1]
sites=json.load(open(f'{S}/sites_x.json')); sp=json.load(open(f'{S}/sp.json'))
metro_areas={p['stop_area']['id'] for p in sp if p.get('stop_area',{}).get('type')=='METROSTN'}
msites={s['id']:s['name'] for s in sites if metro_areas & set(s.get('stop_areas',[]))}
print('metro sites', len(msites), file=sys.stderr)
J=collections.defaultdict(dict)   # (line, dir, journey) -> {site: time}
for i,(sid,name) in enumerate(sorted(msites.items())):
    url=f'https://transport.integration.sl.se/v1/sites/{sid}/departures?transport=METRO&forecast=90'
    try: deps=json.load(urllib.request.urlopen(url, timeout=15))['departures']
    except Exception as e: print('fail',sid,name,e,file=sys.stderr); continue
    for x in deps:
        if x['line']['transport_mode']!='METRO': continue
        k=(x['line']['designation'],x['direction_code'],x['journey']['id'])
        J[k][sid]=x.get('expected') or x['scheduled']
    time.sleep(0.15)
json.dump({'sites':msites,'journeys':[[list(k),v] for k,v in J.items()]}, open(f'{S}/journeys.json','w'), ensure_ascii=False)
