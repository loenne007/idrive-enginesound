#!/usr/bin/env python3
"""Dependency-free FFT, spectral and discontinuity regression checks."""
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
# Compare negative-load impulsiveness with positive cruise at equal rpm.
for p in ('v8','v10'):
 vals=[]
 for load in (.05,-.8):
  a=render(p,f'overrun{load}',[f'{t/20} P 4000 {load} 0 3 70' for t in range(201)])
  a=a[48000:-48000];r=level(a);vals.append(max(map(abs,a))/r)
 assert vals[1]>vals[0]*1.1,(p,'overrun transients missing',vals)
 print(f'{p}: overrun crest factor {vals[0]:.2f}->{vals[1]:.2f}')
print('PASS: levels, pitch, brightness, transients, transitions, mute, stale input and quit')
