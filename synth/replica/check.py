#!/usr/bin/env python3
"""Dependency-free FFT, spectral and discontinuity regression checks."""
import array,cmath,json,math,pathlib,subprocess,sys,wave
ROOT=pathlib.Path(__file__).resolve().parent
BIN=ROOT/'build/es-synth-host'
def read(path):
 with wave.open(str(path),'rb') as w:
  n=w.getnchannels();a=array.array('h',w.readframes(w.getnframes()))
  if sys.byteorder!='little':a.byteswap()
  return [sum(a[i:i+n])/(32768*n) for i in range(0,len(a),n)] if n>1 else [v/32768 for v in a]  # mono = (L+R)/2
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
info=json.loads(subprocess.check_output([str(BIN),'--info']))
for p in info['presets']:
 name=p['id'];a=read(ROOT/'samples'/f'{name}.wav')
 assert all(math.isfinite(x) for x in a)
 peak=max(map(abs,a));assert peak<=10**(-.9/20),peak
 assert level(a)>.008,(name,level(a))
 ratio=clicks(a)
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
   # Engine_Sim itself measures ~3.5 % for the i6 (its sub layer sits on order 1.5) and ~75 % for the cross-plane V8
   if name=='v8':assert nonfiring>.2,'cross-plane bank character lost'
   if name=='i6':assert nonfiring<.06,'inline six lost smoothness'
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
# Compare negative-load impulsiveness with positive cruise at equal rpm.
for p,hi in (('v8',5800),('v10',6600)):
 vals=[]
 for pops in (0,1):
  # a hard run first (pops need a hot pipe: Engine_Sim's exhaust-gas temperature), then three real lifts with
  # full-load runs between; the same drive with X pops 0 is the reference (pops are stochastic in Engine_Sim)
  lines=[f'0 X pops {pops}']+[f'{t/20} P {hi} 1 1 3 70' for t in range(240)];crest=[]
  for k in range(3):
   t0=12+k*4
   lines+=[f'{t0+t/20} P {hi-t*12} -.8 0 3 70' for t in range(50)]
   lines+=[f'{t0+2.5+t/20} P {hi} 1 1 3 70' for t in range(30)]
  a=render(p,f'overrun-pops{pops}',lines)
  for k in range(3):
   w=a[int((12+k*4)*48000):int((14+k*4)*48000)];crest.append(max(map(abs,w))/level(w))
  vals.append(max(crest))
 assert vals[1]>vals[0]*1.1,(p,'overrun pops missing',vals)
 print(f'{p}: overrun crest factor without/with pops {vals[0]:.2f}->{vals[1]:.2f}')
# Automatic-gearbox shifts: an S event must not change the level (85 ms RMS envelope) by more than 3 dB around
# the shift, compared with the same drive without it.
def env85(a):
 n=4080;return [10*math.log10(sum(x*x for x in a[i:i+n])/n+1e-20) for i in range(0,len(a)-n+1,n)]
def smooth(x):x=min(1,max(0,x));return x*x*(3-2*x)
for p in info['presets']:
 name,red=p['id'],p['redline'];worst=[]
 cases={
  'ramp-up':(2,3,lambda t:red*(.70+.02*t) if t<4 else red*(.78-.20*smooth((t-4)/.3)+.02*(t-4)),.9,.94),
  'step-up':(2,3,lambda t:red*(.70+.02*t) if t<4 else red*(.58+.02*(t-4)),.9,.94),
  'overrun-down':(4,3,lambda t:red*(.45-.01*t) if t<4 else red*(.41+.07*smooth((t-4)/.3)-.01*(t-4)),-.5,0),
 }
 for case,(ga,gb,rpm,load,thr) in cases.items():
  envs=[]
  for with_s in (1,0):
   lines=[]
   for i in range(401):
    t=i/50;g=ga if t<4 else gb
    if with_s and i==200:lines.append(f'{t} S {ga} {gb}')
    lines.append(f'{t} P {rpm(t):.1f} {load} {thr} {g} {rpm(t)*.012*g:.1f}')
   envs.append(env85(render(name,f'shift-{case}-{with_s}',lines)))
  k0,k1=int(3.5*48000/4080),int(5.0*48000/4080)
  d=max(abs(a-b) for a,b in zip(envs[0][k0:k1],envs[1][k0:k1]))
  worst.append(f'{case} {d:.1f} dB')
  assert d<=3,(name,case,'shift changes the level',d)
 # every demo upshift (at 12.2, 15.4, 18.6, 21.8 s of the 40 s demo) stays within 3.5 dB of the level around it
 e=env85(read(ROOT/'samples'/f'{name}.wav'));fr=48000/4080;dips=[]
 for ts in (12.2,15.4,18.6,21.8):
  seg=lambda a,b:e[int(a*fr):int(b*fr)]
  before=sum(seg(ts-.6,ts-.1))/len(seg(ts-.6,ts-.1));after=sum(seg(ts+.5,ts+1))/len(seg(ts+.5,ts+1))
  mid=seg(ts-.1,ts+.5);dip=min(before,after)-min(mid);bump=max(mid)-max(before,after)
  dips.append(round(max(dip,bump),1))
  assert dip<=3.5 and bump<=3.5,(name,ts,'demo upshift level jump',before,after,min(mid),max(mid))
 print(f'{name}: shift level change vs no shift: {", ".join(worst)}; demo upshift deviation {dips} dB')
# Response: the sound follows a P rpm step within ~30 ms (firing frequency of the output)
def peak_hz(a,lo,hi):
 n=len(a);s=fft([v*(.5-.5*math.cos(2*math.pi*i/(n-1))) for i,v in enumerate(a)])
 k0,k1=int(lo*n/48000),int(hi*n/48000);k=max(range(k0,k1),key=s.__getitem__);return k*48000/n
a=render('v10','response',[f'{t/50} P {2400 if t<100 else 4800} .5 .55 3 50' for t in range(151)])
got=peak_hz(a[int(2.035*48000):int(2.035*48000)+4096],250,500)
assert abs(got/400-1)<.06,('rpm response too slow: 35 ms after the step the firing is at',got,'not 400 Hz')
print(f'response: 35 ms after a 2400->4800 rpm step the V10 fires at {got:.0f} Hz (target 400)')
# Off-gas balance: idle, light load and coasting about 7-9 dB under full load
def og(p,x=None):
 lines=([f'0 X offgas {x}'] if x is not None else [])
 for i in range(801):
  t=i/50
  st=(p['idle'],.05,.03,0) if t<4 else (2500,.15,.17,3) if t<8 else (3000,-.4,0,3) if t<12 else (.8*p['redline'],1,1,3)
  lines.append(f'{t} P {st[0]:.1f} {st[1]} {st[2]} {st[3]} 50')
 a=render(p['id'],f'offgas{x}',lines);L=lambda s,e:20*math.log10(level(a[int(s*48000):int(e*48000)]))
 return L(14,16),L(2,4),L(6,8),L(10,12)
for p in info['presets']:
 full,idle,light,coast=og(p)
 for name,v in (('idle',idle),('light',light),('coast',coast)):
  assert 5.5<=full-v<=10.5,(p['id'],name,'off-gas level',full-v)
 q=og(p,0.5)
 assert q[1]<idle-4 and q[3]<coast-4 and abs(q[0]-full)<1.5,(p['id'],'X offgas 0.5',q,(full,idle,coast))
 print(f"{p['id']}: off-gas vs full load: idle {idle-full:+.1f}, light {light-full:+.1f}, coast {coast-full:+.1f} dB; X offgas 0.5 -> idle {q[1]-full:+.1f} dB")
# X settings: turbo / bov / pops 0 remove them, bass moves the low end, changes are click-free
def turbo_only(name,lines):
 script=ROOT/'build'/f'{name}.txt';script.write_text('\n'.join(lines)+'\n');path=ROOT/'build'/f'{name}.wav'
 subprocess.run([str(BIN),'--out','wav:'+str(path),'--preset','v8','--script',str(script)],check=True,env={**__import__('os').environ,'ES_BUS':'turbo'})
 return read(path)
pull=[f'{t/50} P {3000+t*8 if t<300 else 2500} {.95 if t<300 else -.5} {1 if t<300 else 0} 3 80' for t in range(401)]
lv={}
for xs in ('','X turbo 0','X bov 0','X turbo 0;X bov 0'):
 pre=[f'0 {c}' for c in xs.split(';') if c]
 a=turbo_only('xturbo'+xs.replace(' ','').replace(';',''),['0 T 1']+pre+pull)
 lv[xs]=(level(a[int(4*48000):int(6*48000)]),max(map(abs,a[int(6*48000):int(6.6*48000)])))
assert lv[''][0]>1e-4 and lv['X turbo 0'][0]<lv[''][0]*0.02,('X turbo 0 must silence the turbo',lv)
assert lv['X turbo 0'][1]>1e-3 and lv['X turbo 0;X bov 0'][1]<lv['X turbo 0'][1]*0.05,('X bov 0 must remove the blow-off',lv)
pl={}
for x in (0,1,2):
 heat=[f'{t/20} P 5000 1 1 3 90' for t in range(200)]
 lift=[f'{10+t/20} P {5000-t*12:.0f} -.6 0 3 90' for t in range(100)]
 a=render('v8',f'xpops{x}',[f'0 X pops {x}']+heat+lift);a=a[10*48000:14*48000]
 pl[x]=max(map(abs,a))/level(a)
assert pl[0]<=pl[1]*1.02 and pl[2]>pl[0]*1.1,('X pops: 0 must remove the pops, 2 make them stronger/more often',pl)
lows=[]
for x in (0,1,2):
 a=render('v12',f'xbass{x}',[f'0 X bass {x}']+[f'{t/20} P 2500 .5 .5 3 50' for t in range(81)]);s=spectrum(a)
 lows.append(10*math.log10(energy(s,40,150)/energy(s,600,3000)))
assert lows[0]<lows[1]-5 and lows[2]>lows[1]+5,('X bass',lows)
lines=[]
for i in range(400):
 t=i/50;lines.append(f'{t} P {2000+1500*math.sin(t)} .6 .7 3 60')
 if i%40==20:lines.append(f'{t} X {["offgas","turbo","bov","pops","bass"][i//40%5]} {(i//40)%3}')
 if i==200:lines.append(f'{t} E i6')
a=render('v8','xchanges',['0 T 1']+lines);clicks(a)
assert not subprocess.run([str(BIN),'--info'],input='',capture_output=True).returncode
print(f'X: turbo 0 {20*math.log10(lv["X turbo 0"][0]/lv[""][0]+1e-12):.0f} dB, bov 0 {20*math.log10(lv["X turbo 0;X bov 0"][1]/lv["X turbo 0"][1]+1e-12):.0f} dB, pops crest {pl[0]:.1f}/{pl[1]:.1f}/{pl[2]:.1f}, bass low/mid {lows[0]:.1f}/{lows[1]:.1f}/{lows[2]:.1f} dB')
print('PASS: levels, pitch, brightness, transients, transitions, mute, stale input, quit, smooth shifts, response, off-gas balance and X settings')
