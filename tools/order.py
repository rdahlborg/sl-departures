import json, sys, collections
S=sys.argv[1]
sites=json.load(open(f'{S}/sites_x.json')); sp=json.load(open(f'{S}/sp.json'))
area_name={p['stop_area']['id']:p['stop_area']['name'] for p in sp if p.get('stop_area',{}).get('type')=='METROSTN'}
canon={}   # site id -> name, one per metro stop area
for aid,an in area_name.items():
    cands=[s for s in sites if aid in s.get('stop_areas',[])]
    if not cands: print("no site for area",aid,an,file=sys.stderr); continue
    exact=[s for s in cands if s['name']==an] or sorted(cands,key=lambda s:len(s.get('stop_areas',[])))
    canon[exact[0]['id']]=exact[0]['name']
d=json.load(open(f'{S}/journeys.json'))
lines=collections.defaultdict(list)
for (line,dc,jid),v in d['journeys']:
    seq=[int(s) for s,_ in sorted(v.items(), key=lambda kv: kv[1]) if int(s) in canon]
    lines[(line,dc)].append(seq)
out={}
for k,seqs in sorted(lines.items()):
    # merge orders: successor graph from consecutive pairs, then walk from the unique source
    succ=collections.defaultdict(set); nodes=set()
    for s in seqs:
        nodes|=set(s)
        for a,b in zip(s,s[1:]): succ[a].add(b)
    # transitive reduction-ish: order by "number of nodes reachable"
    def reach(n,seen=None):
        seen=seen or set()
        for m in succ[n]:
            if m not in seen: seen.add(m); reach(m,seen)
        return seen
    order=sorted(nodes,key=lambda n:-len(reach(n)))
    ok=all(order.index(a)<order.index(b) for s in seqs for a,b in zip(s,s[1:]))
    out[f'{k[0]}/{k[1]}']=order
    print(k, len(order), 'ok' if ok else 'CONFLICT', ':', ' > '.join(canon[n] for n in order))
json.dump({'canon':canon,'order':out}, open(f'{S}/order.json','w'), ensure_ascii=False)
