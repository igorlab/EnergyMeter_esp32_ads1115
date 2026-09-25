/*
 * Сторінки веб-інтерфейсу як літерали у флеші.
 *
 * Нічого не тягнеться з інтернету: у режимі точки доступу браузер до мережі
 * не має доступу взагалі, тож увесь CSS і JS лежать тут. Кольори ті самі, що
 * на панелі — напруга синя, струм зелений (ТЗ §34.4).
 */
#pragma once

#include <Arduino.h>

namespace ui {

static const char CHART_JS[] PROGMEM = R"rawliteral(
// Межі шкали, вирівняні на круглий крок, завжди рівно 4 інтервали.
// Круглі межі потрібні, щоб підписи стояли на місці: 3.880/3.885/3.890
// замість 3.879/3.884/3.890, які змінюються від кожного відліку.
// Кольори canvas беруться з :root у style.css — щоб лінія напруги на графіку
// й цифра напруги в картці не могли розійтися. Читаємо один раз: у дорозі
// getComputedStyle коштує layout, а малюємо ми 4 рази на секунду.
let PAL=null;
function pal(){
 if(!PAL){const s=getComputedStyle(document.documentElement),g=k=>s.getPropertyValue('--'+k).trim();
  PAL={volt:g('volt'),curr:g('curr'),dim:g('dim'),muted:g('muted'),
       line:g('line'),grid:g('grid'),tip:g('tip')};}
 return PAL;
}
function nice4(lo,hi,minSpan){
 let span=Math.max(hi-lo,minSpan);
 for(let k=0;k<10;k++){
  const raw=span/4, mag=Math.pow(10,Math.floor(Math.log10(raw))), n=raw/mag;
  const step=(n<=1?1:n<=2?2:n<=5?5:10)*mag;
  const l=Math.floor(lo/step)*step, h=l+4*step;
  if(h>=hi-1e-9) return [l,h,step];
  span*=1.5;
 }
 return [lo,hi,(hi-lo)/4||1];
}
// Липка шкала: тримаємо попередню, поки дані в ній і займають хоч 35% висоти.
// Без цього графік перемасштабовується 4 рази на секунду, і лінії стрибають.
function sticky(cv,key,lo,hi,minSpan){
 const prev=cv[key];
 if(prev){
  const fill=(hi-lo)/(prev[1]-prev[0]);
  if(lo>=prev[0]-1e-9&&hi<=prev[1]+1e-9&&fill>=0.35) return prev;
 }
 return (cv[key]=nice4(lo,hi,minSpan));
}
function drawChart(cv, pts, opt){
 const d=cv.getContext('2d'), P=pal(), W=cv.width=cv.clientWidth*2, H=cv.height=cv.clientHeight*2;
 d.clearRect(0,0,W,H);
 const L=88,R=88,T=24,B=44, w=W-L-R, h=H-T-B;
 cv._pts=pts; cv._opt=opt;
 if(!pts.length){d.fillStyle=P.dim;d.font='24px system-ui';d.fillText('no data',L,T+h/2);return;}
 const t0=pts[0].t, t1=pts[pts.length-1].t||t0+1;
 function mm(k){let a=Infinity,b=-Infinity;for(const p of pts){if(p[k]<a)a=p[k];if(p[k]>b)b=p[k];}return [a,b];}
 const mv=mm('v'), mi=mm('i');
 const [v0,v1,vs]=sticky(cv,'_sv',mv[0],mv[1],opt.padV||0.02);
 const [i0,i1,is]=sticky(cv,'_si',mi[0],mi[1],opt.padI||0.01);
 const X=t=>L+(t-t0)/((t1-t0)||1)*w, YV=v=>T+h-(v-v0)/((v1-v0)||1)*h, YI=i=>T+h-(i-i0)/((i1-i0)||1)*h;
 cv._g={X:X,YV:YV,YI:YI,L:L,T:T,w:w,h:h};
 // Розрядність підпису — рівно та, що потрібна кроку шкали, але не більше
 // трьох знаків: дані на графік ідуть квантовані до мілівольта й міліампера,
 // тому четвертий знак у підписі завжди нуль.
 const dec=st=>Math.min(3,Math.max(0,Math.ceil(-Math.log10(st))));
 const dv=dec(vs), di=dec(is);
 d.strokeStyle=P.grid; d.lineWidth=2; d.font='22px system-ui'; d.textBaseline='middle';
 for(let k=0;k<=4;k++){const y=T+h*k/4;
  d.beginPath();d.moveTo(L,y);d.lineTo(L+w,y);d.stroke();
  d.fillStyle=P.volt;d.textAlign='right';d.fillText((v1-vs*k).toFixed(dv),L-10,y);
  d.fillStyle=P.curr;d.textAlign='left';d.fillText((i1-is*k).toFixed(di),L+w+10,y);}
 d.fillStyle=P.dim;d.textAlign='center';d.textBaseline='top';
 for(let k=0;k<=4;k++){const t=t0+(t1-t0)*k/4;d.fillText(opt.fmt(t),X(t),T+h+12);}
 function line(k,Y,c){d.strokeStyle=c;d.lineWidth=3;d.beginPath();
  pts.forEach((p,n)=>{const x=X(p.t),y=Y(p[k]);n?d.lineTo(x,y):d.moveTo(x,y);});d.stroke();}
 line('v',YV,P.volt); line('i',YI,P.curr);
 d.textAlign='left';d.textBaseline='alphabetic';
 d.fillStyle=P.volt;d.fillText('V',L,18); d.fillStyle=P.curr;d.fillText('A',L+w-14,18);
 if(cv._hx!=null) cursor(cv);
}
function cursor(cv){
 const d=cv.getContext('2d'), P=pal(), g=cv._g, pts=cv._pts, opt=cv._opt;
 if(!g||!pts||!pts.length)return;
 let n=0,best=1e9;
 for(let k=0;k<pts.length;k++){const dx=Math.abs(g.X(pts[k].t)-cv._hx);if(dx<best){best=dx;n=k;}}
 const p=pts[n], x=g.X(p.t);
 d.strokeStyle=P.dim;d.lineWidth=2;d.setLineDash([6,6]);
 d.beginPath();d.moveTo(x,g.T);d.lineTo(x,g.T+g.h);d.stroke();d.setLineDash([]);
 d.fillStyle=P.volt;d.beginPath();d.arc(x,g.YV(p.v),7,0,6.284);d.fill();
 d.fillStyle=P.curr;d.beginPath();d.arc(x,g.YI(p.i),7,0,6.284);d.fill();
 const bw=196,bh=104, bx=(x+16+bw>g.L+g.w)?x-16-bw:x+16, by=g.T+8;
 d.fillStyle=P.tip;d.strokeStyle=P.line;d.lineWidth=2;
 d.beginPath();d.roundRect(bx,by,bw,bh,10);d.fill();d.stroke();
 d.font='24px ui-monospace,monospace';d.textAlign='left';d.textBaseline='alphabetic';
 d.fillStyle=P.muted;d.fillText(opt.fmt(p.t),bx+14,by+32);
 d.fillStyle=P.volt;d.fillText(p.v.toFixed(3)+' V',bx+14,by+64);
 d.fillStyle=P.curr;d.fillText(p.i.toFixed(3)+' A',bx+14,by+94);
}
function chartHover(cv){
 cv.style.cursor='crosshair';
 const at=e=>{const r=cv.getBoundingClientRect();
  cv._hx=((e.touches?e.touches[0].clientX:e.clientX)-r.left)*(cv.width/r.width);
  drawChart(cv,cv._pts,cv._opt);};
 cv.addEventListener('mousemove',at);
 cv.addEventListener('touchmove',e=>{e.preventDefault();at(e);},{passive:false});
 const off=()=>{cv._hx=null;drawChart(cv,cv._pts,cv._opt);};
 cv.addEventListener('mouseleave',off);
 cv.addEventListener('touchend',off);
}
function hms(s){s=Math.max(0,Math.round(s));const p=n=>String(n).padStart(2,'0');
 return p(s/3600|0)+':'+p((s/60|0)%60)+':'+p(s%60);}
function chg(x,u){const a=Math.abs(x);
 if(a>=1)return x.toFixed(1)+' '+u;
 if(a>=0.1)return (x*1000).toFixed(0)+' m'+u;
 return (x*1000).toFixed(1)+' m'+u;}
// На графік передаємо мілівольти й міліампери: четвертий знак — це шум
// приладу, і в кривій він лише мохнатить лінію.
function q3(x){return Math.round(x*1000)/1000;}
)rawliteral";

static const char STYLE_CSS[] PROGMEM = R"rawliteral(
/* Палітра взята з сусіднього проєкту kh-station: нейтральні сірі замість
   синювато-чорних і семантичні кольори GitHub-типу. Синього в тій палітрі
   немає взагалі, а напруга має бути синьою (ТЗ §34.4) — тому --volt доданий
   із тієї ж родини, що й --curr / --warn / --bad.
   Кольори живуть ЛИШЕ тут: CHART_JS читає їх звідси через getComputedStyle,
   інакше canvas і HTML розходяться при першій же правці. */
:root{
 color-scheme:dark;
 --bg:#2c2c2c;--card:#333;--surface-2:#3b3b3b;--fill:#4a4a4a;--line:#555;
 --fg:#fff;--muted:#b9b9b9;--dim:#a2a2a2;
 --volt:#58A6FF;--curr:#3FB950;--warn:#E3A008;--bad:#F05252;--bad-strong:#DA3633;
 --chart:#262626;--grid:#3f3f3f;--tip:rgba(24,24,24,.94)
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.55 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
header{display:flex;align-items:baseline;gap:16px;flex-wrap:wrap;padding:14px 20px;border-bottom:1px solid var(--line)}
header h1{font-size:16px;font-weight:500;margin:0;letter-spacing:.04em}
header a{color:var(--muted);text-decoration:none;margin-right:14px}
header a:hover{color:var(--volt)}
main{padding:20px;max-width:1100px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px;margin-bottom:20px}
.cell{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px 14px}
.cell .k{font-size:12px;color:var(--dim);text-transform:uppercase;letter-spacing:.08em}
.cell .v{font-size:26px;font-variant-numeric:tabular-nums;margin-top:2px}
.volt{color:var(--volt)}.curr{color:var(--curr)}.warn{color:var(--warn)}.bad{color:var(--bad)}.dim{color:var(--dim)}
.bar{display:flex;align-items:center;gap:12px;flex-wrap:wrap;margin:0 0 16px}
button{font:15px/1 system-ui;color:var(--fg);background:var(--surface-2);border:1px solid var(--line);
 border-radius:8px;padding:11px 20px;cursor:pointer}
button:hover{background:var(--fill);border-color:#6d6d6d}
button:active{transform:scale(.985)}
/* Тонована заливка замість власного фону — так само, як бейджі стану в
   kh-station: колір статусу читається, а кнопка лишається кнопкою. */
button.rec{background:rgba(63,185,80,.14);border-color:rgba(63,185,80,.45);color:var(--curr)}
button.rec:hover{background:rgba(63,185,80,.22);border-color:rgba(63,185,80,.6)}
button.on{background:var(--bad-strong);border-color:var(--bad-strong);color:#fff}
button.on:hover{background:#c22d2b;border-color:#c22d2b}
select{margin-left:auto;font:14px/1 system-ui;color:var(--fg);background:var(--surface-2);
 border:1px solid var(--line);border-radius:8px;padding:9px 12px}
canvas{width:100%;height:340px;background:var(--chart);border:1px solid var(--line);border-radius:10px}
table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums;font-size:13px}
th,td{padding:5px 10px;text-align:right;border-bottom:1px solid var(--fill)}
th{color:var(--dim);font-weight:500;text-align:right;position:sticky;top:0;background:var(--bg)}
th:first-child,td:first-child{text-align:left}
.wrap{max-height:460px;overflow:auto;border:1px solid var(--line);border-radius:10px;margin-top:16px}
.files{list-style:none;padding:0;margin:0}
.files li{display:flex;align-items:center;gap:14px;padding:11px 14px;border-bottom:1px solid var(--fill)}
.files a{color:var(--volt);text-decoration:none}
.sec{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px 18px;margin:0 0 16px}
.sec h2{font-size:14px;font-weight:500;letter-spacing:.06em;text-transform:uppercase;
 color:var(--muted);margin:0 0 12px}
.row{display:flex;align-items:center;gap:14px;padding:6px 0}
.row label{width:150px;color:var(--muted);font-size:14px}
.ro{font-variant-numeric:tabular-nums;color:var(--fg)}
input,select{font:14px/1 ui-monospace,monospace;color:var(--fg);background:var(--surface-2);
 border:1px solid var(--line);border-radius:7px;padding:8px 10px;width:190px}
input:focus,select:focus{outline:none;border-color:var(--volt)}
.hint{font-size:13px;color:var(--dim);margin:8px 0 0;line-height:1.5}
.cal{display:flex;align-items:center;gap:12px;flex-wrap:wrap;margin-top:12px;
 padding-top:12px;border-top:1px solid var(--line)}
.cal b{color:var(--fg);font-variant-numeric:tabular-nums}
code{background:var(--surface-2);padding:2px 6px;border-radius:4px;font-size:12px}
.files .sz{color:var(--dim);font-size:13px;margin-left:auto;font-variant-numeric:tabular-nums}
.files button.rm{font-size:13px;padding:6px 12px;background:rgba(240,82,82,.12);
 border-color:rgba(240,82,82,.4);color:var(--bad)}
.files button.rm:hover{background:rgba(240,82,82,.2);border-color:rgba(240,82,82,.6)}
.files li>a:first-child{min-width:150px}
)rawliteral";

static const char INDEX_HTML[] PROGMEM = R"rawliteral(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>Battery Tester</title>
<link rel=stylesheet href=/style.css>
<header><h1>BATTERY TESTER</h1><nav><a href=/>dashboard</a><a href=/tests>tests</a>
<a href=/settings>settings</a></nav>
<span id=st class=dim></span></header>
<main>
<div class=grid>
 <div class=cell><div class=k>voltage</div><div class="v volt" id=v>&mdash;</div></div>
 <div class=cell><div class=k id=sock>charge</div><div class=v id=soc>&mdash;</div></div>
 <div class=cell><div class=k>current</div><div class="v curr" id=i>&mdash;</div></div>
 <div class=cell><div class=k>power</div><div class=v id=p>&mdash;</div></div>
 <div class=cell><div class=k>capacity</div><div class="v curr" id=ah>&mdash;</div></div>
 <div class=cell><div class=k>energy</div><div class=v id=wh>&mdash;</div></div>
 <div class=cell><div class=k>internal R</div><div class=v id=rint>&mdash;</div></div>
 <div class=cell><div class=k>time</div><div class=v id=t>&mdash;</div></div>
</div>
<div class=bar>
 <button id=rec class=rec>start recording</button>
 <button id=rst>reset</button>
 <span id=recst class=dim></span>
 <select id=win>
  <option value=30>30 seconds</option>
  <option value=60 selected>1 minute</option>
  <option value=300>5 minutes</option>
  <option value=900>15 minutes</option>
  <option value=0>all</option>
 </select>
</div>
<canvas id=c></canvas>
</main>
<script src=/chart.js></script>
<script>
const pts=[]; let t0=null, running=false, busy=false;
async function post(p){
 if(busy)return; busy=true;
 try{ await fetch(p,{method:'POST'}); await tick(); }
 catch(e){ st.textContent='command failed'; st.className='bad'; }
 busy=false;
}
rec.onclick=()=>post(running?'/api/stop':'/api/start');
rst.onclick=()=>post('/api/reset');
// Вікно тримає шкалу біля поточних значень: старий пусковий стрибок виїжджає
// за край сам, і вертикальна шкала стягується без жодного втручання.
function view(){
 const wsec=+win.value;
 if(!wsec||!pts.length)return pts;
 const edge=pts[pts.length-1].t-wsec;
 let a=0; while(a<pts.length-2&&pts[a].t<edge)a++;
 return pts.slice(a);
}
function redraw(){drawChart(c,view(),{fmt:hms});}
win.onchange=redraw;
async function tick(){
 try{
  const r=await fetch('/api/status',{cache:'no-store'}); const s=await r.json();
  v.textContent=s.voltage.toFixed(3)+' V';
  // Показуємо величину без знака: напрямок уже сказаний словом CHARGE або
  // DISCHARGE. У /api/status, у CSV і в лозі знак лишається (§28).
  i.textContent=Math.abs(s.current).toFixed(3)+' A';
  p.textContent=Math.abs(s.power).toFixed(2)+' W';
  ah.textContent=chg(Math.abs(s.Ah),'Ah');
  wh.textContent=chg(Math.abs(s.Wh),'Wh');
  t.textContent=hms(s.elapsed);
  // Міліоми — звична одиниця для внутрішнього опору елемента; в омах воно
  // читалось би як 0.356 і плуталось із рештою дробових величин.
  rint.textContent=s.rint>0?(s.rint*1000).toFixed(0)+' m\u03A9':'\u2014';
  if(s.soc>=0){
   soc.textContent=s.soc.toFixed(0)+' %';
   soc.className='v '+(s.soc<15?'bad':s.soc<30?'warn':'curr');
   sock.textContent='charge \u00B7 from voltage';
  }else{soc.textContent='\u2014';soc.className='v dim';sock.textContent='charge';}
  const bits=[s.running?'RUNNING':'PAUSED', s.mode];
  if(s.fault!=='NONE')bits.push('FAULT '+s.fault);
  if(s.simulated)bits.push('SIM');
  bits.push(s.sd?'SD':'no SD');
  if(s.profile&&s.profile!=='?')bits.push(s.profile+' · cutoff '+s.cutoff.toFixed(2)+' V');
  running=s.running;
  rec.textContent=running?'stop recording':'start recording';
  rec.className=running?'on':'rec';
  rst.disabled=running;
  recst.textContent=s.sd
   ? (s.logging?('recording TEST_'+String(s.test).padStart(4,'0')+'.BIN'):'card ready')
   : 'no card - memory only';
  recst.className=s.logging?'curr':'dim';
  st.textContent=bits.join(' · ');
  st.className=s.fault!=='NONE'?'bad':(s.running?'curr':'dim');
  if(t0===null)t0=s.uptime;
  pts.push({t:s.uptime-t0,v:q3(s.voltage),i:q3(Math.abs(s.current))});
  if(pts.length>7200)pts.shift();   // 30 хв при опитуванні 4 Гц
  redraw();
 }catch(e){st.textContent='no connection';st.className='bad';}
}
chartHover(c); setInterval(tick,250); tick();
addEventListener('resize',redraw);
</script>)rawliteral";

static const char SETTINGS_HTML[] PROGMEM = R"rawliteral(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>Settings</title>
<link rel=stylesheet href=/style.css>
<header><h1>SETTINGS</h1><nav><a href=/>dashboard</a><a href=/tests>tests</a>
<a href=/settings>settings</a></nav><span id=st class=dim></span></header>
<main>

<div class=sec><h2>Voltage calibration</h2>
 <div class=row><label>divider K</label><input id=div type=number step=0.000001></div>
 <div class=row><label>gain</label><span id=vgain class=ro></span></div>
 <div class=row><label>offset, V</label><span id=voffset class=ro></span></div>
 <div class=row><label>ceiling</label><span id=ceiling class=ro></span></div>
 <p class=hint>The ceiling is set by the ADC supply, not by the PGA range:
  a single-ended measurement cannot read above its own supply.</p>
 <div class=cal>
  <span class=dim>uncalibrated now: <b id=vraw>&mdash;</b> V</span>
  <input id=vref type=number step=0.001 placeholder="reference, V">
  <button data-ch=v data-slot=0>point 1</button>
  <button data-ch=v data-slot=1>point 2</button>
 </div>
 <p class=hint>The two points must differ by at least 1 V.</p>
</div>

<div class=sec><h2>Current calibration</h2>
 <div class=row><label>shunt, ohm</label><input id=shunt type=number step=0.000001></div>
 <div class=row><label>gain</label><input id=igain type=number step=0.000001></div>
 <div class=row><label>offset, A</label><input id=ioffset type=number step=0.000001></div>
 <p class=hint>A negative gain means reversed wiring polarity.
  Positive current is discharge.</p>
 <div class=cal>
  <span class=dim>uncalibrated now: <b id=iraw>&mdash;</b> A</span>
  <input id=iref type=number step=0.0001 placeholder="reference, A">
  <button data-ch=i data-slot=0>point 1</button>
  <button data-ch=i data-slot=1>point 2</button>
 </div>
 <p class=hint>Points must span at least 50 mA. Below 500 mA the scale is
  weakly determined - verify at the working current.</p>
</div>

<div class=sec><h2>Battery profile</h2>
 <div class=row><label>chemistry</label><select id=chem>
  <option value=li>Li-ion, 4.20 / 3.00 V</option>
  <option value=lfp>LiFePO4, 3.65 / 2.50 V</option></select></div>
 <div class=row><label>cells</label><input id=cells type=number min=1 max=8 step=1></div>
 <div class=row><label>capacity, Ah</label><input id=cap type=number step=0.001></div>
 <div class=row><label>Imax, A</label><input id=imax type=number step=0.1></div>
 <div class=row><label>Tmax, &deg;C</label><input id=tmax type=number step=1></div>
 <div class=row><label>internal R, ohm</label><input id=rint type=number step=0.001></div>
 <div class=row><label>pack cutoff</label><span id=cutoff class=ro></span></div>
 <div class=row><label>fault below</label><span id=floor class=ro></span></div>
 <div class=row><label>internal R in use</label><span id=rnow class=ro></span></div>
 <div class=row><label>open-circuit voltage</label><span id=ocv class=ro></span></div>
 <p class=hint>Cutoff stops nothing - it only marks the capacity in the result.
  The protection board breaks the discharge at its own threshold.</p>
 <p class=hint>Internal R is needed for the charge percentage: the curve
  describes the open-circuit voltage, while the terminal under load sits higher
  (charge) or lower (discharge) by I&middot;R. Zero here means "use the measured
  value" - the device catches it on a current step out of rest.</p>
</div>

<div class=sec><h2>Measurement</h2>
 <div class=row><label>averaging</label><input id=avg type=number min=1 max=32 step=1></div>
 <div class=row><label>cycle</label><span id=cycle class=ro></span></div>
 <div class=row><label>V range now</label><span id=pga class=ro></span></div>
 <p class=hint>A window of 16 at 32 SPS is 500 ms with 234 ms of group delay.
  Going wider buys nothing: what remains is source drift, not noise.</p>
</div>

<div class=sec><h2>Status</h2>
 <div class=row><label>network</label><span id=net class=ro></span></div>
 <div class=row><label>microSD card</label><span id=sd class=ro></span></div>
 <div class=row><label>next test</label><span id=nxt class=ro></span></div>
 <div class=row><label>RAM</label><span id=heap class=ro></span></div>
 <p class=hint>The network is set with the <code>wifi</code> console command:
  the password never travels over unencrypted HTTP.</p>
</div>

<div class=bar>
 <button id=apply class=rec>apply</button>
 <button id=save>save to NVS</button>
 <button id=load>load from NVS</button>
 <button id=erase class=on>erase NVS</button>
</div>
</main>
<script>
function say(t,bad){st.textContent=t;st.className=bad?'bad':'curr';
 setTimeout(()=>{st.textContent='';st.className='dim';},4000);}
async function load(){
 const s=await (await fetch('/api/settings',{cache:'no-store'})).json();
 div.value=s.div; shunt.value=s.shunt; igain.value=s.igain; ioffset.value=s.ioffset;
 cells.value=s.cells; cap.value=s.cap; imax.value=s.imax; tmax.value=s.tmax;
 avg.value=s.avg; chem.value=s.chem; rint.value=s.rprof;
 vgain.textContent=s.vgain.toFixed(6); voffset.textContent=s.voffset.toFixed(6);
 ceiling.textContent=s.ceiling.toFixed(2)+' V';
 cutoff.textContent=s.cutoff.toFixed(2)+' V';
 floor.textContent=s.floor.toFixed(2)+' V';
 rnow.textContent=s.rint>0?(s.rint.toFixed(3)+' ohm'+(s.rprof>0?' (from profile)':' (measured)')):'not measured yet';
 ocv.textContent=s.ocv>0?(s.ocv.toFixed(4)+' V'):'—';
 cycle.textContent=s.cycle.toFixed(1)+' ms, max '+s.cycleMax.toFixed(1);
 pga.textContent=s.pga+'  (±'+s.pgaFs.toFixed(3)+' V)';
 net.textContent=(s.ap?'access point ':'')+s.ssid+'  ·  '+s.ip;
 sd.textContent=s.sd?(s.card+' MB  ·  used '+s.used+'  ·  free '+s.free+' MB'):'none';
 nxt.textContent=s.sd?('#'+String(s.test+1).padStart(4,'0')):'—';
 heap.textContent=s.heap+' kB free of '+s.heapTotal+' kB';
 vraw.textContent=s.vraw.toFixed(5); iraw.textContent=s.iraw.toFixed(5);
}
async function post(url){const r=await fetch(url,{method:'POST'});
 const t=await r.text(); await load(); return t;}
apply.onclick=async()=>{
 const q='div='+div.value+'&shunt='+shunt.value+'&igain='+igain.value+
  '&ioffset='+ioffset.value+'&avg='+avg.value+'&chem='+chem.value+
  '&cells='+cells.value+'&cap='+cap.value+'&imax='+imax.value+'&tmax='+tmax.value+
  '&rint='+rint.value;
 say(await post('/api/settings?'+q));};
save.onclick=async()=>say(await post('/api/nvs?op=save'));
load.onclick=async()=>say(await post('/api/nvs?op=load'));
erase.onclick=async()=>{if(confirm('Erase calibration from NVS?'))say(await post('/api/nvs?op=erase'),1);};
document.querySelectorAll('.cal button').forEach(b=>b.onclick=async()=>{
 const ch=b.dataset.ch, ref=(ch=='v'?vref:iref).value;
 if(ref===''){say('enter a reference first',1);return;}
 b.disabled=true;
 say(await post('/api/cal?ch='+ch+'&slot='+b.dataset.slot+'&ref='+ref));
 b.disabled=false;});
load(); setInterval(()=>{fetch('/api/settings',{cache:'no-store'}).then(r=>r.json())
 .then(s=>{vraw.textContent=s.vraw.toFixed(5);iraw.textContent=s.iraw.toFixed(5);});},1000);
</script>)rawliteral";

static const char TEST_HTML[] PROGMEM = R"rawliteral(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>Test</title>
<link rel=stylesheet href=/style.css>
<header><h1 id=h>TEST</h1><nav><a href=/>dashboard</a><a href=/tests>tests</a>
<a href=/settings>settings</a><a id=dl href=#>download CSV</a></nav>
<span id=st class=dim></span></header>
<main><canvas id=c></canvas><div class=wrap><table id=tb>
<thead><tr><th>time</th><th>V</th><th>A</th><th>W</th><th>Ah</th><th>Wh</th><th>&deg;C</th><th>state</th></tr></thead>
<tbody></tbody></table></div></main>
<script src=/chart.js></script>
<script>
const id=new URLSearchParams(location.search).get('id')||'1';
h.textContent='TEST '+String(id).padStart(4,'0'); dl.href='/csv?id='+id+'&dl=1';
fetch('/csv?id='+id).then(r=>r.text()).then(txt=>{
 const rows=txt.trim().split('\n').slice(1).map(l=>l.split(','));
 const pts=rows.map(r=>({t:+r[0],v:q3(+r[1]),i:q3(Math.abs(+r[2]))}));
 const o={fmt:hms,padV:0.02,padI:0.01};
 drawChart(c,pts,o); chartHover(c);
 addEventListener('resize',()=>drawChart(c,pts,o));
 const b=tb.tBodies[0]; const frag=document.createDocumentFragment();
 const ab=x=>Math.abs(+x).toFixed(4);
 for(const r of rows){const tr=document.createElement('tr');
  tr.innerHTML='<td>'+hms(+r[0])+'</td><td class=volt>'+r[1]+'</td><td class=curr>'+ab(r[2])+
   '</td><td>'+ab(r[3])+'</td><td>'+ab(r[4])+'</td><td>'+ab(r[5])+'</td><td>'+r[6]+'</td><td class=dim>'+r[7]+'</td>';
  frag.appendChild(tr);}
 b.appendChild(frag);
 const last=rows[rows.length-1]||[];
 st.textContent=rows.length+' records · '+chg(Math.abs(+(last[4]||0)),'Ah')+
  ' · '+chg(Math.abs(+(last[5]||0)),'Wh');
}).catch(e=>{st.textContent='could not read the file';st.className='bad';});
</script>)rawliteral";

}  // namespace ui
