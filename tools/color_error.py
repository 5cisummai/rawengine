#!/usr/bin/env python3
"""Compare matching measured/rendered CIELab patches (same illuminant/observer), no image assumptions."""
import argparse, csv, json, math, statistics

def delta_e_2000(a,b):
    l1,a1,b1=a;l2,a2,b2=b
    c1=math.hypot(a1,b1);c2=math.hypot(a2,b2);cm=(c1+c2)/2
    g=.5*(1-math.sqrt(cm**7/(cm**7+25**7)))
    ap1=(1+g)*a1;ap2=(1+g)*a2
    cp1=math.hypot(ap1,b1);cp2=math.hypot(ap2,b2)
    hp1=math.degrees(math.atan2(b1,ap1))%360;hp2=math.degrees(math.atan2(b2,ap2))%360
    dl=l2-l1;dc=cp2-cp1;dh=hp2-hp1
    if cp1*cp2==0:dh=0
    elif dh>180:dh-=360
    elif dh<-180:dh+=360
    dH=2*math.sqrt(cp1*cp2)*math.sin(math.radians(dh/2))
    lm=(l1+l2)/2;cm=(cp1+cp2)/2
    if cp1*cp2==0:hm=hp1+hp2
    elif abs(hp1-hp2)<=180:hm=(hp1+hp2)/2
    elif hp1+hp2<360:hm=(hp1+hp2+360)/2
    else:hm=(hp1+hp2-360)/2
    t=1-.17*math.cos(math.radians(hm-30))+.24*math.cos(math.radians(2*hm))+.32*math.cos(math.radians(3*hm+6))-.20*math.cos(math.radians(4*hm-63))
    sl=1+.015*(lm-50)**2/math.sqrt(20+(lm-50)**2);sc=1+.045*cm;sh=1+.015*cm*t
    rt=-2*math.sqrt(cm**7/(cm**7+25**7))*math.sin(math.radians(60*math.exp(-((hm-275)/25)**2)))
    x,y,z=dl/sl,dc/sc,dH/sh
    return math.sqrt(max(0,x*x+y*y+z*z+rt*y*z))

def patches(path):
    with open(path,newline='') as f:rows=list(csv.DictReader(f))
    result={}
    for row in rows:
        name=row['patch'];value=tuple(float(row[key]) for key in ['L','a','b'])
        if name in result or not all(math.isfinite(v) for v in value):raise ValueError('Duplicate/nonfinite patch: '+name)
        result[name]=value
    if not result:raise ValueError('No patches')
    return result

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('reference');p.add_argument('rendered');a=p.parse_args()
    reference,rendered=patches(a.reference),patches(a.rendered)
    if reference.keys()!=rendered.keys():raise SystemExit('Patch identifiers do not match')
    values=[{'patch':k,'delta_e_76':math.dist(v,rendered[k]),'delta_e_2000':delta_e_2000(v,rendered[k])} for k,v in reference.items()]
    errors=[v['delta_e_2000'] for v in values]
    print(json.dumps({'metric':'CIEDE2000 (kL=kC=kH=1)','mean':statistics.mean(errors),'maximum':max(errors),'patches':values},indent=2))
