#!/usr/bin/env python3
"""Waveguide engine: dependency-free FFT, spectral and discontinuity regression checks."""
import array,cmath,json,math,pathlib,subprocess,sys,wave
ROOT=pathlib.Path(__file__).resolve().parent
BIN=ROOT/'build/es-synth-host'
def read(path):
 with wave.open(str(path),'rb') as w:
  n=w.getnchannels();a=array.array('h',w.readframes(w.getnframes()))
  if sys.byteorder!='little':a.byteswap()
  return [v/32768 for v in a[::n]]
def fft(x):
 n=len(x);a=[complex(v) for v in x];j=0
 for i in range(1,n):
  bit=n>>1
  while j&bit:j^=bit;bit>>=1
  j^=bit
  if i<j:a[i],a[j]=a[j],a[i]
 m=2
 while m<=n:
  root=cmath.exp(-2j*math.pi/m)
  for k in range(0,n,m):
   z=1
   for j in range(m//2):
    u=a[k+j];v=z*a[k+j+m//2];a[k+j]=u+v;a[k+j+m//2]=u-v;z*=root
  m*=2
 return [abs(v)**2 for v in a[:n//2]]
def spectrum(a,t=2):
 n=32768;x=a[int(t*48000):int(t*48000)+n];assert len(x)==n
 return fft([v*(.5-.5*math.cos(2*math.pi*i/(n-1))) for i,v in enumerate(x)])
def energy(s,lo,hi):return sum(s[int(lo*32768/48000):int(hi*32768/48000)])
def render(p,name,lines):
 script=ROOT/'build'/f'{name}-{p}.txt';script.write_text('\n'.join(lines)+'\n')
 path=ROOT/'build'/f'{name}-{p}.wav'
 subprocess.run([str(BIN),'--out','wav:'+str(path),'--preset',p,'--script',str(script)],check=True)
 return read(path)
def level(a):return math.sqrt(sum(x*x for x in a)/len(a))
def clicks(a):
 # Relative to the surrounding 10ms waveform slope, not a permissive fixed peak.
 bad=0;worst=0
 for i in range(480,len(a)-480,240):
  dif=[abs(a[k]-a[k-1]) for k in range(i-240,i+240)]
  rms=math.sqrt(sum(x*x for x in dif)/len(dif));mx=max(dif);ratio=mx/max(rms,.0001);worst=max(worst,ratio)
  if mx>.035 and ratio>14:bad+=1
 assert bad==0,(bad,worst)
 return worst
def envelope(a,half):
 # 10 ms RMS every 5 ms, energy-averaged over (2 half + 1) x 5 ms, in dB
 hop=240;e=[sum(x*x for x in a[i:i+480])/480 for i in range(0,len(a)-480,hop)]
 return [10*math.log10(sum(e[max(0,k-half):k+half+1])/len(e[max(0,k-half):k+half+1])+1e-12) for k in range(len(e))]
info=json.loads(subprocess.check_output([str(BIN),'--info']))
for p in info['presets']:
 name=p['id'];a=read(ROOT/'samples'/f'{name}.wav')
 assert all(math.isfinite(x) for x in a)
 peak=max(map(abs,a));assert peak<=10**(-.9/20),peak
 assert level(a)>.008,(name,level(a))
 ratio=clicks(a)
 # demo upshifts (gears 1-5 at 9+3.2k s): level continuous through each shift within 3.5 dB of the settled levels
 # before/after (85 ms envelope); this includes the rpm sweep through pipe/muffler colour and the room
 for ts in (12.2,15.4,18.6,21.8):
  e=envelope(a[int((ts-.5)*48000):int((ts+1)*48000)],8);k=lambda t:int((t+.5)*200)
  pre=sorted(e[k(-.35):k(-.05)])[len(e[k(-.35):k(-.05)])//2];post=sorted(e[k(.6):k(.9)])[len(e[k(.6):k(.9)])//2]
  w=e[k(-.05):k(.6)]
  assert min(w)>=min(pre,post)-3.5 and max(w)<=max(pre,post)+3.5,(name,ts,'demo shift gap/onset',min(w)-min(pre,post),max(w)-max(pre,post))
 pitches=[]
 for rpm in (1800,3600):
  lines=[f'{t/20} P {rpm} 0.7 0.8 3 50' for t in range(81)]
  steady=render(name,f'steady{rpm}',lines);s=spectrum(steady);expected=rpm/60*p['cylinders']/2
  # strongest low frequency peak, not just a search restricted around expected.
  lo,hi=int(expected*.55*32768/48000),int(expected*1.45*32768/48000)
  k=max(range(lo,hi),key=s.__getitem__);got=k*48000/32768
  assert abs(got/expected-1)<.05,(name,rpm,got,expected)
  pitches.append(round(got,1))
  if rpm==3600:
   total=energy(s,20,12000)
   centroid=sum(i*48000/32768*v for i,v in enumerate(s))/sum(s)
   cycle=[]
   for h in range(1,p['cylinders']*3):
    b=round(rpm/120*h*32768/48000);cycle.append(sum(s[b-1:b+2]))
   nonfiring=sum(v for h,v in enumerate(cycle,1) if h%p['cylinders'])/sum(cycle)
   if name=='v8':assert nonfiring>.01,'cross-plane bank character lost'
   if name=='i6':assert nonfiring<.01,'inline six lost smoothness'
   print(f"{name}: centroid {centroid:.0f} Hz, 2-6kHz {100*energy(s,2000,6000)/total:.2f}%, non-firing cycle harmonics {100*nonfiring:.2f}%")
 spectra=[]
 for load in (.05,.95):
  a2=render(name,f'load{load}',[f'{t/20} P 3000 {load} {load} 3 50' for t in range(81)])
  s=spectrum(a2);spectra.append(energy(s,1000,6000)/energy(s,60,6000))
 assert spectra[1]>spectra[0]*1.05,(name,'no brightness increase',spectra)
 print(f'{name}: peak {20*math.log10(peak):.2f} dBFS, RMS {level(a):.4f}, slope ratio {ratio:.1f}, firing Hz {pitches}, high-band share {spectra[0]:.3f}->{spectra[1]:.3f}')
# Event transitions, stale watchdog, malformed controls and rapid preset switching.
lines=[]
for i in range(160):
 t=i/20;lines.append(f'{t} P {1200 if t<3 else 5500} .6 .7 3 60')
 if i in (20,21,22,23):lines.append(f'{t} E '+['v8','v10','v12','i6'][i-20])
 if i==40:lines.append(f'{t} S 3 4')
 if i==70:lines.append(f'{t} S 4 3')
 if i==90:lines.append(f'{t} M 1')
 if i==110:lines.append(f'{t} M 0')
lines+=['8 P nan inf nan 3 50','8 E invalid','8 V nan','11 V 0.8','12 Q']
a=render('v8','transitions',lines);clicks(a)
assert level(a[int(10.5*48000):int(11.5*48000)])<.0001,'watchdog failed'
assert level(a[int(5.1*48000):int(5.4*48000)])<.003,'mute failed'
assert level(a[-4800:])<.0001,'quit fade failed'
# Overrun crackle: after a lift-off from load (fuel cut, hot pipe) the exhaust gets impulsive for a short window.
# Compare the crest factor of the first 0.7 s after the lift with steady load at the same rpm.
for p in ('v8','v10'):
 lines=[f'{t/20} P {5000 if t<50 else 5000-600*(t/20-2.5)} {.9 if t<50 else -.8} {.9 if t<50 else 0} 3 70' for t in range(141)]
 a=render(p,'overrun',lines)
 def crest(x):return max(map(abs,x))/level(x)
 vals=[crest(a[48000:110000]),crest(a[121000:156000])]
 assert vals[1]>vals[0]*1.1,(p,'overrun transients missing',vals)
 assert level(a[121000:156000])<level(a[48000:110000]),(p,'overrun louder than load')
 print(f'{p}: crest factor load {vals[0]:.2f} -> after lift-off {vals[1]:.2f}')
# Gear shifts: level continuity across S events.
# (1) The same drive is rendered with and without the S lines (renders are deterministic), so rpm-dependent
#     pipe/muffler colour cancels and only what the shift itself does remains: within -0.1..+0.8 s of the event the
#     10 ms RMS envelope (energy-averaged over 55 ms) may not move more than 3 dB (no cut/gap, no restart onset, no
#     bark or crackle triggered by the shift). Upshift with the rpm ramping down over 250 ms like the car's
#     telemetry, and an overrun downshift with a rev rise.
# (2) An upshift whose rpm arrives as an instantaneous STEP (the synth turns it into a ~20 ms rounded ramp): the
#     85 ms envelope stays within 3 dB of the settled levels before/after (no gap below, no onset above).
def drive(kind,shift,step=False):
 lines=[]
 for i in range(81):
  t=i/20
  if kind=='up':
   k=min(1,max(0,(t-2)/.25));k=k*k*(3-2*k);r=6000+(2800-6000)*k;l,th,g0,g1=.9,.94,3,4
   if step:r=6000 if t<2 else 2800
  else:
   k=min(1,max(0,(t-2)/.2));k=k*k*(3-2*k);r=3000+1000*k;l,th,g0,g1=-.8,0,4,3
  lines.append(f'{t} P {r:.1f} {l} {th} {g1 if t>=2 else g0} 60')
  if shift and i==40:lines.append(f'{t} S {g0} {g1}')
 return lines
def med(x):x=sorted(x);return x[len(x)//2]
worst=0;worst_step=0
for p in ('i6','v8','v10','v12'):
 for kind in ('up','down'):
  a=envelope(render(p,f'shift-{kind}',drive(kind,True)),5)
  b=envelope(render(p,f'shift-{kind}-ref',drive(kind,False)),5)
  w=[abs(a[k]-b[k]) for k in range(int(1.9*200),int(2.8*200))]
  worst=max(worst,max(w))
  assert max(w)<=3,(p,kind,'shift level discontinuity dB',max(w))
 e=envelope(render(p,'shift-step',drive('up',True,True)),8)
 pre=med(e[int(1.65*200):int(1.95*200)]);post=med(e[int(2.6*200):int(2.9*200)]);w=e[int(1.95*200):int(2.6*200)]
 d=max(min(pre,post)-min(w),max(w)-max(pre,post));worst_step=max(worst_step,d)
 assert d<=3,(p,'rpm step: gap/onset dB',d)
print(f'shifts: max envelope deviation caused by S events {worst:.1f} dB, by an rpm step {worst_step:.1f} dB (limit 3)')
# X sound settings (each 0..2, 1 = default): each changes what it should, 0 removes turbo/bov/pops, no clicks.
def db(x):return 10*math.log10(x+1e-20)
def band(a,lo,hi,t0,t1):
 n=16384;e=0;c=0
 for s0 in range(int(t0*48000),int(t1*48000)-n,n):
  s=fft([v*(.5-.5*math.cos(2*math.pi*i/(n-1))) for i,v in enumerate(a[s0:s0+n])]);e+=sum(s[int(lo*n/48000):int(hi*n/48000)]);c+=1
 return e/max(c,1)
def xrender(p,name,setting,value,lines):return render(p,f'x-{name}-{setting}{value}',[f'0 X {setting} {value}']+lines)
steady=lambda r,l,th,dur=4:[f'{t/20} P {r} {l} {th} 3 60' for t in range(int(dur*20)+1)]
lift=[f'{t/20} P {5200 if t<50 else 5200-500*(t/20-2.5)} {.95 if t<50 else -.6} {.95 if t<50 else 0} 3 70' for t in range(101)]
lifts=[]
for t in range(251):
 T=t/20;up=(T%3.5)<2  # 2 s pulling, 1.5 s overrun, three times
 lifts.append(f'{T} P {5200 if up else 5200-600*(T%3.5-2)} {.95 if up else -.6} {.95 if up else 0} 3 70')
res={}
for v in (0,1,2):
 # offgas: idle and coast level scale, full load stays
 res['idle',v]=level(xrender('v8','idle','offgas',v,steady(650,.05,.03))[48000:])
 res['full',v]=level(xrender('v8','full','offgas',v,steady(5000,.95,.95))[48000:])
 # turbo: whistle/air band (3-8 kHz) under boost on the turbo i6
 res['turbo',v]=band(xrender('i6','boost','turbo',v,steady(5500,.95,.95)),3000,8000,1.5,3.5)
 # bov: 1-4 kHz right after a lift from boost (window 0.05-0.45 s after the lift)
 res['bov',v]=band(xrender('i6','lift','bov',v,lift),1000,4000,2.55,2.95)
 # pops: loudest 10 ms in the overrun after three lift-offs on the V8 (from 0.1 s after each lift)
 a=xrender('v8','lifts','pops',v,lifts)
 res['pops',v]=max(level(a[i:i+480]) for L in (2,5.5,9) for i in range(int((L+.1)*48000),int((L+1.4)*48000),240))
 # bass: energy under 120 Hz against 300-3000 Hz
 a=xrender('v12','bass','bass',v,steady(3000,.5,.5));res['bass',v]=db(band(a,30,120,1,3.5))-db(band(a,300,3000,1,3.5))
assert res['idle',0]<res['idle',1]*.2 and res['idle',2]>res['idle',1]*1.6,('offgas idle',[res['idle',v] for v in (0,1,2)])
assert abs(db(res['full',2]**2)-db(res['full',0]**2))<1.5,('offgas must not change full load',[res['full',v] for v in (0,1,2)])
assert res['turbo',0]<res['turbo',1]*.8<res['turbo',2]*.8*.7,('turbo',[res['turbo',v] for v in (0,1,2)])
assert res['bov',0]<res['bov',1]*.7 and res['bov',1]<res['bov',2]*.7,('bov',[res['bov',v] for v in (0,1,2)])
assert db(res['pops',1]**2)>db(res['pops',0]**2)+3 and db(res['pops',2]**2)>db(res['pops',1]**2)+2,('pops',[res['pops',v] for v in (0,1,2)])
assert res['bass',0]<res['bass',1]-4<res['bass',2]-8,('bass',[res['bass',v] for v in (0,1,2)])
print('X: idle offgas 0/1/2 %.1f/%.1f/%.1f dBFS, full load %.1f/%.1f dBFS; turbo band %+.1f/%+.1f dB; bov %+.1f/%+.1f dB; pops peak %.1f/%.1f/%.1f dBFS; bass %+.1f/%+.1f dB'%(
 db(res['idle',0]**2),db(res['idle',1]**2),db(res['idle',2]**2),db(res['full',0]**2),db(res['full',2]**2),
 db(res['turbo',0])-db(res['turbo',1]),db(res['turbo',2])-db(res['turbo',1]),db(res['bov',0])-db(res['bov',1]),db(res['bov',2])-db(res['bov',1]),
 db(res['pops',0]**2),db(res['pops',1]**2),db(res['pops',2]**2),res['bass',0]-res['bass',1],res['bass',2]-res['bass',1]))
# changing every setting between its extremes while driving: smoothed, no clicks
lines=[f'{t/20} P {1500+3500*abs(math.sin(t/40))} {.8 if (t//30)%2 else -.4} {.8 if (t//30)%2 else 0} 3 60' for t in range(161)]
names=('offgas','turbo','bov','pops','bass')
for i in range(40):lines.append(f'{i*.2} X {names[i%5]} {(0,2,1)[(i//5)%3]}')
lines.sort(key=lambda x:float(x.split()[0]));clicks(render('i6','x-changes',lines))
print('PASS: levels, pitch, brightness, transients, shifts, X settings, transitions, mute, stale input and quit')
