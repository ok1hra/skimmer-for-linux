// cont.js — a call's continent (EU AS AF NA SA OC), as the DX cluster has
// it (cty.dat): the call's DXCC from the map's prefix labels (SkMap.locate),
// its continent from the list below. The list was made from the labels'
// places (map/map50.js, 342 DXCC) and set by hand where the DXCC lists
// differ from the globe: Cyprus, Georgia, Spratly in AS; Dodecanese,
// Gibraltar, Franz Josef Land in EU; Aruba in SA; Grenada, Cocos I. in NA;
// Antarctica in SA. Turkey is AS, but TA1 (Thrace) EU. A new copy of
// map50.js with a new DXCC needs its line here (else the call has none).
// Part of skimmer-for-linux. GPL-3.0-or-later.
'use strict';
window.Cont = (() => {
  const LIST = {
  AF: '3B6 3B8 3B9 3C 3C0 3DA 3V 3X 3Y0 5A 5H 5N 5R 5T 5U 5V 5X 5Z 6O 6W 7P 7Q 7X 9G 9J 9L 9Q 9U 9X A2 C5 C9 CN CT3 D2 D4 D6 E3 EA8 EA9 EL ET FH FR FT FT/G FT/J FT/T FT5W FT5X FT5Z J2 J5 S0 S7 S9 ST SU TJ TL TN TR TT TU TY TZ V5 VK0H VQ9 XT Z2 Z8 ZD7 ZD8 ZD9 ZS ZS8',
  AS: '1A1 4J 4L 4S 4X 5B 7O 8Q 9K 9M 9M2 9N 9V A4 A5 A6 A7 A9 AP BS7 BV BV9P BY E4 EK EP EX EY EZ HL HS HZ JA JD1/O JT JY OD P5 S2 T6 TA UA/AS UK UN VR2 VU VU4 VU7 XU XV XW XX9 XZ YI YK ZC4',
  EU: '1A 3A 4O 4U1ITU 9A 9H C3 CT CU DL E7 EA EA6 EI ER ES EU F G GD GI GJ GM GU GW HA HB HB0 HV I IS0 JW JX LA LX LY LZ OE OH OH0 OJ0 OK OM ON OY OZ PA R1FJ S5 SM SP SV SV/A SV5 SV9 T7 TF TK UA/E UA2 UR YL YO YU Z3 Z6 ZA ZB2',
  NA: '4U1UN 6Y 8P C6 CO CY0 CY9 FG FJ FM FO/C FP FS HH HI HK0A HP HR J3 J6 J7 J8 K KG4 KL KP1 KP2 KP4 KP5 OX PJ5 PJ7 TG TI TI9 V2 V3 V4 VE VP2E VP2M VP2V VP5 VP9 XA4 XE YN YS YV0 ZF',
  OC: '3D2 3D2/C 3D2/R 4W 5W 9M6 A3 C2 DU E5 E51N E51S E6 FK FK/C FO FO/A FO/M FW H4 H40 JD1/M KH0 KH1 KH2 KH3 KH4 KH5 KH6 KH7K KH8 KH8/S KH9 P2 T3 T30 T31 T32 T33 T8 V6 V7 V8 VK VK0M VK9C VK9L VK9M VK9N VK9W VK9X VP6 VP6/D YB YJ ZK3 ZL ZL7 ZL8 ZL9',
  SA: '3Y/P 8R 9Y CE CE0X CE0Y CE0Z CP CX FY HC HC8 HK HK0M LU OA P4 PJ2 PJ4 PY PY0F PY0S PY0T PZ VP0G VP0H VP0O VP0S VP8 YV ZP',
  };
  const OF = {};
  for (const [c, ps] of Object.entries(LIST)) for (const p of ps.split(' ')) OF[p] = c;
  const cache = new Map();
  // the continent of a call, or '' when its DXCC is not known
  function of(call) {
    const k = String(call).toUpperCase();
    if (cache.has(k)) return cache.get(k);
    let c = '';
    if (/^TA1/.test(k)) c = 'EU';
    else if (typeof SkMap !== 'undefined') { const d = SkMap.locate(k); c = (d && OF[d.prefix]) || ''; }
    if (cache.size > 5000) cache.clear();
    cache.set(k, c);
    return c;
  }
  return { of };
})();
