# Skimmer for Linux: slabé CW signály, srovnání s literaturou a plán zlepšení

> Analýza konvenčního dekodéru **v2** (`src/engine/decode_cw_v2.c`, commit `463a5d7`, 2026-10-25) projektu
> [ok1hra/skimmer-for-linux](https://github.com/ok1hra/skimmer-for-linux). DeepCW je záměrně vynechán.
> Vložil Dan OK1HRA, 2026-11-05. Analýza vychází ze čtení kódu, nic se **nesestavovalo ani nespouštělo**. Odhady v dB jsou teoretické a musí je potvrdit replay.

### Definice SNR použité v textu

Všechna SNR jsou výkon nosné při stisknutém klíči ku výkonu šumu v uvedené šířce pásma. Liší se jen
referenční šířkou, takže se převádějí konstantou (T_dot = 1.2 / WPM s; pro 25 WPM T_dot = 48 ms):

| zápis | definice | převod |
|---|---|---|
| **SNR_2500** | šum v 2500 Hz (hlavní referenční škála) | – |
| **SNR500** | šum v 500 Hz (škála syntetického benche v WEAK-SIGNAL.md; ne `snr_db` dekodéru, to je poměr obálek peak/floor) | SNR500 = SNR_2500 + 7.0 dB |
| **SNR125** | šum v 125 Hz (šířka kanálu v2) | SNR125 = SNR_2500 + 13.0 dB = SNR500 + 6.0 dB |
| **Es/N0_dot** | energie tečky ku spektrální hustotě šumu | Es/N0_dot = SNR_2500 + 10·log10(2500·T_dot) = SNR500 + 13.8 dB při 25 WPM |
| **R-SNR** | SNR, které hlásí CW Skimmer Server (R) ve spotech | definice R není zveřejněná; podle projektu (WEAK-SIGNAL.md) je otevřené, zda jde o SNR500. Zde se **předpokládá R-SNR ≈ SNR500**, převody R-SNR jsou proto orientační |

Kde je v textu číslo z dokumentů projektu, nese jejich škálu (bench: SNR500, live: R-SNR).
Odhady zisku „+N dB“ jsou rozdíly a na škále nezávisí.

---

## 1. Shrnutí

v2 je výrazně dál než „klasický“ dekodér. Už je to **měkký semi-Markovův Viterbi** s log-normálními
apriory délek (Bellova linie), s učením rukopisu a s převerifikací slova proti MASTER.SCP vynuceným
zarovnáním na stejné LLR. Projekt navíc má výbornou měřicí infrastrukturu: replay nahraného IQ, srovnání
s CW Skimmer Server (R) a RBN jako rozhodčí.

Slabé signály neztrácí v Viterbiho mřížce. Ztrácí je **před ní a kolem ní**, na místech, kde stále
padají tvrdá rozhodnutí nebo heuristiky nastavené na silné signály:

1. **Tvrdý squelch / gate** na široké obálce rozhoduje dřív, než má mřížka šanci (≈ 12 dB peak/mean).
2. **Tvrdý bootstrap hodin** (Schmittův trigger + shlukování 4–8 značek). Při nízkém SNR učí špatnou rychlost.
3. **Předdetekční šířka pásma** je pevných 45 Hz bez ohledu na rychlost. Přizpůsobený filtr pro 25 WPM je ~20 Hz.
4. **Nekalibrované LLR**: saturuje na ±2.5 nezávisle na SNR. Při slabém signálu tak šum dostává stejnou váhu jako silný signál.
5. **Jazykový/formátový model až po rozhodnutí**: re-read se spouští jen z 1-best textu do 2 editací.
   Podle LIVE-AIR.md pod 10 dB (R-SNR, tj. SNR_2500 ≈ +3 dB) „nikdy nevystřelí“.
6. **Nevyužitá redundance skimmeru**: stanice volá CQ se značkou 2–3× za over a v mnoha overech.
   Důkazy se mezi opakováními nesčítají.

Vlastní data projektu (LIVE-AIR.md) říkají totéž: ~25 % zmeškaných stanic má v textu „nic podobného
značce, většinou pod 10 dB“ (R-SNR) a ~14 % je 1–2 editace vedle. Závěr dokumentu: *„That needs the decoder to read
more of them — not a policy.“*

**Rozpočet (25 WPM):**

| | SNR500 | SNR_2500 | Es/N0_dot |
|---|---|---|---|
| bench v2, 50 % recall (kanál uprostřed) | 0 … +2 dB | −7 … −5 dB | +13.8 … +15.8 dB |
| live: L zachytí jen 20 % R-stanic pod R-SNR 5 dB | < 5 dB (≈ R-SNR) | < −2 dB | < 18.8 dB |
| sekvenční dekodér s priory (teoretický odhad) | −7 … −5 dB | −14 … −12 dB | +7 … +9 dB |

**Prostor je zhruba 5–8 dB.** Většina z něj leží v bodech 1, 3 a 6.

---

## 2. Srovnání metod z literatury s implementací v2

| Metoda (zdroj) | Stav v v2 | Kde v kódu | Mezera pro slabé signály |
|---|---|---|---|
| Přizpůsobený filtr ~1/T_dot (teorie detekce) | **Částečně.** Derotace na nosnou + Butterworth 2. řádu 45 Hz pevně; 3-tap MA na 250 Hz | `decode_cw_v2.c:2048–2095` | Pro 20–30 WPM je šum ~2× širší než optimum. Ztráta nekoherentní integrace ~1.5–3 dB |
| Banka filtrů / bez ztráty mezi biny (teorie detekce) | **Ne.** Pevné kanály 125 Hz, prototyp −6 dB na hraně | `channelizer.c`, WEAK-SIGNAL.md | Až 6 dB na hraně kanálu. Narrow path s tone_splitterem byl live horší a vrácen |
| Detekce integrací v čase (Welch, CFAR) | **Jen v tone_split** (opt-in, bez live verdiktu) | `tone_split.c` | Hlavní gate je okamžitá obálka. Integrace přes sekundy by našla nosnou o 5–10 dB slabší |
| Měkké rozhodování místo prahu (VE3NEA → AG1LE) | **Ano** v mřížce. **Ne** v gate, keying-duty a bootstrapu | `:2116–2127`, `:2273–2330` | Tvrdá rozhodnutí na vstupu mřížky |
| Kalibrované LLR (Rice vs. Rayleigh) | **Heuristika** span + Rayleigh, clamp ±2.5. Rician zkoušen a zamítnut (QSB 73 → 44 %) | `:1735–1840` | LLR neškáluje se SNR. Při slabém signálu převažuje šum nad priory. Viz §4, proč Rician selhal |
| Bellův semi-Markov / Viterbi (Bell 1977, AG1LE) | **Ano**, kvalitně: DIT/DAH × ESP/CSP/WSP, log-normální priory, učený rukopis, lag-commit | `:1073–1140`, `:1568` | Jediná hypotéza rychlosti. Jen max-path, bez posteriorů znaků |
| Rychlost jako skrytý stav (Bell) | **Ne.** Hodiny z bootstrapu + EMA + watchdogy (dah/dit streak) | `:2017–2045`, `:1474` | Špatné hodiny se poznají až po 16–24 značkách |
| Trie znaků + jazykový model v prohledávání (Bell, ASR praxe) | **Ne.** Znaky se skládají po commitu. LM jen jako re-read slova po uzavření | `:1319` | Re-read potřebuje 1-best do 2 editací, ten pod R-SNR 10 dB (SNR_2500 ≈ +3 dB) neexistuje |
| Formátový model (CQ / TEST / DE / 5NN, gramatika značky) | **Jen v extraktoru** (`callsign.c`) nad textem | `callsign.c` | Neovlivňuje čtení prvků |
| Squelch podle spolehlivosti (GRITTY) | **Ne.** Squelch podle amplitudy, `confidence = 1 − 1.5·elem_err` | `:2328` | Gate „signál vs. šum“ podle věrohodnosti by otevřel dřív a falešně méně |
| Soft-combining opakování (EME/RSCW praxe, specifické pro skimmer) | **Ne** | – | Dvě čtení = +3 dB, tři = +4.8 dB na značku |
| Noise blanker před úzkou filtrací (standardní DSP praxe) | **Není** | – | Důležité hlavně na 160/80 m |

---

## 3. Analýza úzkých míst v kódu

### 3.1 Gate a keying-duty (`decode_cw_v2.c:2108–2127`)
```c
st->gate_open = st->env_hi > 4.0 * st->env_lo;   // otevře na 12 dB peak/mean
st->keying    = st->p_high > 0.15;               // podíl vzorků nad env_lo+0.7·span
```
- Čte **raw kanál** (125 Hz, ne narrow obálku), aby našel nosnou ještě necentrovanou.
- `env_hi` je okamžitý peak s 0.8 s doběhem. Na Rayleighově šumu sám vyskakuje vysoko,
  a slabý signál proto musí mít amplitudu ≳ 3σ. Hrubý odhad: SNR125 ≈ +7 dB, tedy **SNR500 ≈ +1 dB, SNR_2500 ≈ −6 dB, Es/N0_dot ≈ +14.8 dB** (25 WPM),
  což souhlasí s bench 50% bodem SNR500 0.2–2.4 dB.
- Co gate neotevře, mřížka nikdy nevidí. Pre-roll ring (2 s) pomáhá jen s hlavou overu,
  nepomůže signálu, který gate neotevře vůbec.

### 3.2 Bootstrap hodin (`:2273–2330`)
Schmittův trigger s prahy 0.30/0.55 mezi `env_lo` a `env_hi`, pak shlukování 4–8 značek. Při SNR blízko
gate jsou délky běhů z velké části šum a vznikne špatný `dit`. Pak se uplatní heuristiky
(watchdog 16 dahů / 24 ditů, `elem_err`, re-lock ring), ale mezitím text padá jako „nic podobného značce“.

### 3.3 Pozorovací model (`:1735–1840`)
```c
x        = (m - mid) / (µm - µs);
span_llr = clamp(5·x, ±2.5);
ray_llr  = clamp(3.3·(½z² − ln z − 2.89), ±2.5);
llr      = ½·span + ½·ray;
```
- Velikost LLR **nezávisí na SNR**. Šumový vzorek při slabém signálu má stejnou „jistotu“ jako vzorek při
  SNR500 30 dB. Duration priory (α = 10, `SEG_COST` 0.5) jsou proto při slabém signálu relativně slabé
  a mřížka sleduje šum (krátké falešné prvky, rozbité znaky).
- `µm` je peak tracker s okamžitým náběhem. Při slabém signálu jede po šumových špičkách, `mid` je vysoko
  a slabé značky padají pod něj.
- Vzorky jsou silně korelované (250 Hz po 45 Hz LP a 3-tap MA ≈ 2.5–3 vzorky na nezávislý),
  takže součet v `cum[]` nadhodnocuje důkaz.

### 3.4 Re-read proti MASTER.SCP (`:1319`)
Výborný mechanismus: vynucené zarovnání kandidáta na tytéž LLR proti volnému čtení. Je ale omezený
**generováním kandidátů**: SCP značky do 2 editací od 1-best slova, slovo ≥ 3 znaky, obsahuje číslici i písmeno.
Pod ~R-SNR 10 dB (SNR_2500 ≈ +3 dB) je 1-best tak rozbitý, že správný kandidát se do sady nedostane.

### 3.5 Kanálová struktura a sousedé (`pipeline.c:1010`)
`ghost_suppressed` zahodí dekód, když soused do ±2 kanálů má +6 dB. Pro slabou stanici vedle silné je to
tvrdá ztráta (WEAK-SIGNAL.md: +10 dB soused 200 Hz = 19 % značek). Narrow path to řešil, ale live nevyšel.

---

## 4. Proč minulé pokusy nevyšly, a co z toho plyne

- **Synthetic bench nepředpověděl live** (WEAK-SIGNAL.md, revert). Syntetika má špatný rukopis, únik, QRM
  i statistiku šumu. → Každá změna se musí rozhodnout na **replay nahrávek**. Pro slabé signály
  doporučuju **semi-syntetiku z reálných nahrávek** (krok 0.3).
- **Rician/Rayleigh LLR zhoršil QSB.** Pravděpodobné příčiny: (a) pevné nebo pomalu sledované `A` ve fade,
  kdy Rician s A z vrcholu vidí tlumenou značku jako mezeru; (b) korelované vzorky bez dekorelace;
  (c) chybějící zbytková váha priory. → Nezkoušet znovu „Rician místo heuristiky“, ale **LLR s marginalizací
  přes amplitudu** (zobecněný LR, směs několika úrovní A nebo Rayleighův prior na A) a **SNR-závislé škálování**
  existující heuristiky. Ověřit kalibrací (reliability diagram), ne jen recallem.
- **Narrower focus cutoff 15/20/25 Hz = bez rozdílu.** Test běžel s 1.3×WPM nad podlahou, tj. podlaha se nikdy
  neuplatnila. Navíc gate čte raw kanál, takže zúžení soft cesty nepomůže tam, kde gate neotevře.
  → Úzký filtr má smysl teprve spolu s krokem 1 (gate).

---

## 5. Plán zlepšení (pořadí = poměr přínos / riziko)

Konvence projektu: každá změna za přepínačem `SKIM_*` (výchozí off), golden gate byte-identický při vypnutí,
rozhodnutí na `skimmer-replay` → `tap-score.py` a „L of R“ v binech R-SNR (< 5, 5–10, 10–15 dB),
pak noc v skimmer-compare. Měřit vždy **recall slabých binů a busty zvlášť**.

### Krok 0 — Rozložit ztráty (měření, bez změny chování). Předpoklad všeho.
0.1 **Oracle experimenty** na 2–3 nahrávkách (30/20/40 m), jen v replay nástroji:
   - *oracle gate*: gate vynuceně otevřený na frekvencích, kde R spotoval → kolik značek navíc se přečte,
   - *oracle clock*: `dit` z WPM, které hlásí R → podíl chyb způsobených bootstrapem,
   - *oracle frekvence*: přesné `foff` → podíl AFC.
   Výsledek říká, kam patří kroky 1–3 a kolik dB lze čekat. Bez něj se ladí naslepo.
   **Hotovo 2026-11-06, výsledek v §7** (gate stojí čtení pod 10 dB, clock a AFC ne).

0.2 **Metrika slabých signálů**: L of R v binech R-SNR < 5 / 5–10 / 10–15 dB, plus CER značky
   (editační vzdálenost dekódovaného tokenu od R-značky v okně spotu), ne jen binární recall.
   **Hotovo jako `skimmer-compare/read-vs-feed.py`** (spot / čteno / blízko / nic po binech, CQ zvlášť), §7.

0.3 **Semi-syntetická sada** (klíčové pro slabé signály): z nahrávek vyber silné stanice (R-SNR ≥ 25 dB, R i L shodně),
   vyřízni jejich kanál, přičti kalibrovaný šum tak, aby SNR500 bylo −8 … +10 dB (SNR_2500 −15 … +3 dB, Es/N0_dot +5.8 … +23.8 dB při 25 WPM), a přehraj přes celý pipeline.
   Reálný rukopis, reálný únik a QRM, známá pravda, řízené SNR. Tím se odstraní rozpor bench vs. live.

### Krok 1 — Měkká detekce místo tvrdého gate (odhad +2–4 dB)
> Oracle (§7): pod 10 dB gate stojí ~10 bodů přečtených CQ značek, do feedu z nich ale projdou 0–2 body.
> Má smysl až spolu s krokem 6 (nebo 3/5), sám ne.

1.1 **Detektor nosné integrací**: na raw kanálu (nebo pásmově) Welchův periodogram s 1–3 s integrací,
    CFAR proti mediánu, sledování keyingu přes modulaci výkonu v binu. Integrace přes sekundy zachytí
    keyed nosnou o 5–10 dB slabší než okamžitá obálka. Lze vyjít z detekce v `tone_split.c`
    (FFT 64, EMA 2 s, median floor), ale jako **gate**, ne jako router slotů.
    (Pozn.: WEAK-SIGNAL.md hlásí „carrier detector as v2's gate: 2–5 dB worse“. Šlo o detektor ladění
    na split topologii s prahem 8 dB a vyhodnocením 1×/s. Tady jde o detektor s nižším prahem
    a delší integrací, ověřený krokem 0.1.)
1.2 **Gate podle věrohodnosti** (princip GRITTY): mřížka běží „nanečisto“ už při detekci kandidáta,
    a výstup se pustí, až log-odds „signál vs. jen šum“ přes 3–5 s okno překročí práh
    (srovnání skóre HMM s nulovým modelem „všechny vzorky = mezera“). Šum navíc neprojde ani při otevřeném gate.
1.3 **Delší pre-roll** (2 → 5–6 s), aby pomalejší detektor neztratil hlavu overu (CQ + první značku).
    Stávající `preroll_replay` to umí, jde jen o velikost ringu a fence.
1.4 Keying-duty test převést na měkký (modulační index výkonu v binu místo podílu vzorků nad prahem).

### Krok 2 — Šířka pásma podle rychlosti, jen pro slabé signály (odhad +1–3 dB)
2.1 Cutoff soft cesty `fc = clamp(k·1/T_dot, 12, 45) Hz`, k ≈ 0.6–0.8, **aktivní jen při nízkém µm/µs**.
    Silné signály zůstanou na 45 Hz, protože dřívější zkušenost (F5IN, N8RYH → T8RYH) ukázala slévání ditů.
    Hystereze stejná jako u `edge_now`.
2.2 Nahradit 3-tap MA klouzavým součtem ~0.5 T_dot (integrate-and-dump charakter) nebo raised-cos oknem.
2.3 (Oracle §7: přesná nosná nic nemění — odloženo.) **AFC pro úzký filtr**: `foff` z interpolovaného vrcholu integrovaného spektra (krok 1.1), ne jen
    z fázového sklonu na „solid“ vzorcích (`:2238`). Ten při slabém signálu nedostane skoro žádné vzorky.
    Bez toho by úzký filtr signál ztrácel.
2.4 Ověřit dvojí cestou: semi-syntetika (0.3) i replay. Akceptace: zlepšení v binu R-SNR < 10 dB bez nárůstu bustů
    a bez ztráty v binu R-SNR ≥ 20 dB.

### Krok 3 — Kalibrované LLR (odhad +0.5–2 dB, hlavně méně „nic podobného značce“)
3.1 Ponechat heuristiku span + Rayleigh, ale **škálovat ji odhadem SNR**:
    `llr' = g(µm/µs) · llr`, kde g → malé při nízkém poměru. Clamp také závislý na SNR.
    Priory pak při slabém signálu automaticky převáží.
3.2 Dekorelační faktor `w = 1 / (fs · τ_corr)` (≈ 0.35–0.4 při 250 Hz a 45 Hz LP) na příspěvek do `cum[]`.
3.3 Volitelně LLR s marginalizací přes amplitudu (viz §4). Zkoušet až po 3.1 a jen proti 3.1.
3.4 **Kalibrační test** na semi-syntetice: reliability diagram `sigmoid(llr)` vs. pravý stav klíče
    po binech SNR. Cíl: odchylka < 5 % v každém binu.
3.5 `µm` jako robustní odhad (např. 80.–90. percentil vzorků s posteriorem „mark“) místo peak trackeru
    s okamžitým náběhem. Zachovat rychlé sledování QSB (0.35 s).

### Krok 4 — Měkký odhad rychlosti (odhad +1–2 dB v hlavě overu, méně „mis-clock“ úseků)
> Oracle (§7): dit z WPM od R je horší než vlastní hodiny v2. Odloženo.

4.1 Bootstrap z **autokorelace / spektra narrow obálky** přes 2–4 s místo Schmittových běhů
    (první vrchol autokorelace ≈ T_dot, resp. základní frekvence klíčování ve spektru obálky). Funguje i tam, kde jednotlivé běhy nejsou čitelné.
4.2 **Paralelní hypotézy rychlosti**: 3 mřížky (dit ×0.8, ×1, ×1.25) běží současně a po N značkách vyhrává
    nejvyšší kumulativní skóre. To je praktická náhrada Bellovy rychlosti jako stavu.
    Paměť: mřížka je ~50 kB na kanál a alokuje se jen na aktivních kanálech.
    CPU: ~3× jen na aktivních kanálech během bootstrapu.
4.3 Stávající watchdogy ponechat jako pojistku.

### Krok 5 — Lepší kandidáti pro re-read a formátový model v prohledávání (odhad +1–3 dB na značkách)
5.1 **Kandidáti z mřížky, ne z textu**: N-best (nebo forward-backward posteriory znaků) z úseku slova
    a kandidáti = SCP značky kompatibilní s N-best mřížkou znaků (edit distance váhovaná posteriory),
    nejen 2 editace od 1-best. Navíc prefix/suffix shody (značka rozbitá na dvě slova: „torn“ ~15 % misses).
5.2 **Vynucené zarovnání celé fráze**: `rs_forced` rozšířit z jedné značky na šablony
    `CQ <CALL>`, `CQ TEST <CALL>`, `DE <CALL>`, `<CALL> <CALL>`, `<CALL> TEST`.
    Silná, známá slova (CQ, TEST, DE) kotví časování a rychlost, takže značka mezi nimi se čte s menší nejistotou.
5.3 Rozhodovací pravidla (`fit` / `sep`) převést na **posteriorní pravděpodobnost kandidáta**
    (softmax skóre přes kandidáty + volné čtení + „neznámá značka“ prior). Feed gate ji dostane jako feature.
    Zachovat pravidlo projektu: nikdy nevymyslet značku mimo SCP bez opakovaného čtení.

### Krok 6 — Soft-combining opakování (specifické pro skimmer, odhad +3–5 dB pro CQ stanice)
6.1 Pro každou stanici (frekvence ± 30 Hz, okno 2–5 min) uchovávej **LLR úseky** slov, která mohou být
    značkou (délka, pozice za CQ/DE, počet znaků), ne jen text.
6.2 Pro kandidáta C (z 5.1, ze všech čtení dohromady) sečti skóre vynuceného zarovnání přes **všechna**
    opakování: `S(C) = Σ_i [forced_i(C) − free_i]`. Dvě opakování dávají ~+3 dB, tři ~+4.8 dB,
    a to přesně v pásmu, kde dnes vzniká „nic podobného značce“.
6.3 Rozhodnutí: C se spotuje, když `S(C)` překročí práh a odstup od druhého kandidáta ≥ sep.
    Unknown značka (mimo SCP) jen při shodném 1-best ve ≥ 2 čteních (stávající politika).
6.4 Paměť: LLR úseky jsou malé (~250 vzorků/s, float, jen pro kandidátní slova), řádově kB na stanici.
6.5 Napojení: station table (`station.c`) už drží stanici na frekvenci, takže úložiště LLR patří tam.
    Dekodér musí vystavit LLR úsek slova (nové pole v `decode.h`, opt-in).

### Krok 7 — Hrany kanálů a silní sousedé (odhad až +6 dB pro stanice na hraně)
7.1 Pro kandidáty z detektoru 1.1 počítat narrow obálku z **dedikovaného down-mixu na nosnou**
    (z band IQ nebo syntézou ze dvou sousedních kanálů banky), ne z jednoho kanálu s prototypem −6 dB na hraně.
7.2 `ghost_suppressed` měnit až po 7.1 a měřit na nahrávkách se silným sousedem: slabá stanice
    100–300 Hz od silné. Pravidlo +6 dB nahradit testem „je signál v mém kanálu nezávislý“
    (korelace keyingu se sousedem, princip OFF-gated testu z `tone_split.c`).
7.3 Pozor: podobný pokus (narrow path) live prohrál. Bez kroku 0.1 a semi-syntetiky nezačínat.

### Krok 8 — Noise blanker (dolní pásma)
Blanker na band IQ před channelizerem (MAD práh, krátké nulování), za přepínačem, měřit na 160/80 m.

---

## 6. Akceptační kritéria a pořadí prací pro agenty

| Krok | Agent | Závislost | Rozhodnutí podle | Hlavní riziko |
|---|---|---|---|---|
| 0.1–0.3 | měření | – | – (jen data) | – |
| 1 | gate | 0 | L of R v binech R-SNR < 5 a 5–10 dB, busty, falešné řádky na šumu (30 min) | falešné spoty, CPU |
| 2 | filtr + AFC | 1 | bin R-SNR < 10 dB ↑, R-SNR ≥ 20 dB beze změny | slévání ditů u silných |
| 3 | LLR | 0.3 | kalibrace + replay | QSB regrese (viz §4) |
| 4 | rychlost | 0 | podíl „mis-clock“ úseků, hlava overu | CPU |
| 5 | re-read | 3 | misreads 1–2 edity ↓, busty | busty z „torn“ značek |
| 6 | combining | 5 | L of R v binech R-SNR < 10 dB, busty | falešné SCP shody |
| 7 | kanály | 1, 0.3 | stanice u hrany / u silného souseda | regrese live jako minule |

Obecná pravidla:
- Golden gate byte-identický s vypnutým přepínačem. Výchozí hodnotu měnit až po replay a noci v skimmer-compare.
- U každé změny reportovat L of R po R-SNR binech, bust %, CPU na pásmo a falešné řádky na čistém šumu.
- Každé tvrzení v dB doložit měřením a uvést, na čem bylo naměřeno (semi-syntetika / replay / live).

**Očekávané pořadí přínosu** (přepsáno po kroku 0, viz §7): nad 10 dB je největší rezerva v převodu
přečtené značky na spot (krok 6 a extraktor/feed), pod 10 dB ve čtení samotném (kroky 3 a 5) a v gate
(krok 1), který ale sám nic nepřinese, dokud jedno čtení navíc neprojde do feedu. Kroky 2.3 (AFC)
a 4 (rychlost) oracle nepotvrdil, jsou na konci. Krok 7 je nejrizikovější.

---

## 7. Výsledek kroku 0.1 a 0.2 (2026-11-06)

**Metoda.** Nahrávky 1 a 2 (`hd-*-20261015-210146Z`, `hd-*-20261016-124545Z`, 30/20/40 m, 14 h každá),
celý replay přes `skimmer-replay` → `skimmer-tap-replay` (živá politika) → `tap-score.py`, R = session
20261013-183853. Oracle je jen měřicí kód za `SKIM_ORACLE=<soubor>` + `SKIM_ORACLE_MODE` (`src/engine/oracle.h`,
bez nich golden gate byte-identický). Pravdu staví `skimmer-compare/oracle-truth.py`: každý R spot je interval
[t − 30 s, t + 5 s] a nosná se dohledá v IQ (1 Hz periodogram přes 30 s, vrchol v ±55 Hz od R, protože R hlásí
jen 0.1 kHz). Nosné s prominencí < 6 dB se vynechají: 20 m 12–17 % spotů (hustý provoz), 30 m 11–14 %, 40 m 3 %.
Varianty: *gate* (gate a keying-duty vynuceně otevřené v okně R), *gatesoft* (v okně jen poloviční prahy,
2.0× a 0.07, gate se v mezerách zavírá), *clock* (dit z WPM od R místo bootstrapu, reset při odchylce > 25 %),
*freq* (`foff` = změřená nosná, bez sledování), *ghost* (silnější soused kanál nepotlačí).
Biny jsou na škále R (R-SNR ≈ SNR500, tj. R-SNR 10 dB ≈ SNR_2500 3 dB; měření ve větvi snr-shadow dává
R ≈ SNR_2500 + 8 dB).

**Feed (tap-score), recall / bust % / L of R po binech R-SNR:**

| varianta | rec 1 recall | bust | < 10 | 10–15 | 15–20 | 20–25 | rec 2 recall | bust | < 10 | 10–15 | 15–20 | 20–25 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| base | 62.9 | 3.3 | 26.4 | 51.7 | 56.6 | 73.3 | 68.0 | 5.8 | 34.3 | 56.4 | 65.9 | 80.3 |
| gate | 62.9 | 3.7 | 29.8 | 52.5 | 57.6 | 72.2 | 67.4 | 4.8 | 34.3 | 55.3 | 64.6 | 78.6 |
| gatesoft | 63.0 | 4.4 | 28.9 | 53.4 | 57.6 | 70.0 | 67.4 | 5.2 | 35.5 | 55.9 | 64.6 | 78.6 |
| clock | 58.8 | 2.3 | 19.8 | 46.6 | 48.5 | 68.9 | 65.0 | 6.1 | 30.8 | 51.3 | 63.8 | 76.9 |
| freq | 62.4 | 3.3 | 26.4 | 50.8 | 56.6 | 71.9 | 68.1 | 5.7 | 34.9 | 54.9 | 67.1 | 79.5 |
| ghost | 63.0 | 3.5 | 26.4 | 51.7 | 56.6 | 73.3 | 68.4 | 5.5 | 34.3 | 57.1 | 65.9 | 81.2 |

Base sedí s dřívějšími běhy na týchž nahrávkách (rec 1 62.7 / 26.4, rec 2 67.8 / 33.7), měření je tedy
srovnatelné s `LIVE-AIR.md`.

**Čtení vs. feed (`skimmer-compare/read-vs-feed.py`, krok 0.2).** Jen stanice, které R viděl volat CQ
(L do feedu jiné nedává), všechna tři pásma. *spot* = L ji poslal do feedu (do ±1.5 kHz, tolerance compare
`df_khz`; první verze měřila ±200 Hz a „čteno“ o 1–3 body nadsazovala); *čteno* = značka je v textu
dekodéru u nosné v okně R, ale do feedu nešla; *blízko* = token 1–2 editace od značky; *nic* = nic podobného.

| R-SNR | n (rec 1 / 2) | base spot / čteno / blízko / nic | gatesoft spot / čteno / blízko / nic |
|---|---|---|---|
| < 10 | 133 / 178 | 30.1 / 18.0 / 10.5 / 41.4 · 30.3 / 15.7 / 10.7 / 43.3 | 29.3 / 31.6 / 9.0 / 30.1 · 32.0 / 25.3 / 8.4 / 34.3 |
| 10–15 | 137 / 232 | 49.6 / 22.6 / 4.4 / 23.4 · 59.9 / 20.7 / 5.6 / 13.8 | 53.3 / 22.6 / 5.8 / 18.2 · 60.3 / 23.3 / 3.4 / 12.9 |
| 15–20 | 142 / 223 | 57.0 / 25.4 / 5.6 / 12.0 · 66.4 / 18.4 / 6.3 / 9.0 | 58.5 / 23.2 / 7.0 / 11.3 · 64.6 / 19.3 / 6.3 / 9.9 |
| 20+ | 327 / 439 | 69.4 / 16.2 / 3.7 / 10.7 · 78.1 / 11.2 / 3.2 / 7.5 | 68.2 / 18.0 / 3.7 / 10.1 · 75.4 / 13.9 / 3.2 / 7.5 |

(procenta, rec 1 · rec 2)

**Závěry.**
1. **Nad 10 dB L značku přečte, ale nespotuje.** 11–25 % CQ stanic má značku přesně v textu a do feedu nejde,
   „nic“ je jen 7–23 %. To je práce pro extraktor, feed a krok 6, ne pro DSP (rozpad v §8).
2. **Pod 10 dB je ztráta ve čtení:** ~43 % CQ stanic nemá v textu nic podobného značce. Gate tu stojí čtení:
   s oracle gate klesne „nic“ o 9–11 bodů a „čteno“ stejně stoupne. Spotů se změní −1 až +2 body, protože
   jedno čtení navíc feed nepustí. Krok 1 sám nestačí; má smysl spolu s krokem 6 nebo s lepším čtením (3, 5).
   Odhad +2–4 dB pro krok 1 data nepotvrzují.
3. **Hodiny (krok 4) nejsou úzké místo.** Dit z WPM od R je horší ve všech binech (recall −3 až −4, „nic“
   +2 až +14 bodů): vlastní bootstrap + EMA v2 jsou lepší hodiny než WPM, které hlásí R.
4. **AFC (krok 2.3) nejsou úzké místo.** Změřená nosná místo sledované nemění nic; v2 si nosnou stanic,
   které R slyší, odhaduje správně.
5. **Ghost** (krok 7.2): +0.1 až +0.4 recallu. Pro stanice vedle silného souseda to není hlavní ztráta.
6. Busty se s oracle gate mění oběma směry (rec 1 +0.4 až +1.1, rec 2 −0.6 až −1.0) — vynucený gate čte šum
   v pauzách mezi overy, gatesoft méně.

**Výhrady.** Okno R je jen 35 s kolem spotu (R spotuje stanici zhruba jednou za 10 min), oracle tedy pomáhá
jen v něm. Oracle *gate* nese i šum z pauz. Na 20 m může freq oracle vzít nosnou silnějšího souseda
v ±55 Hz. Clock oracle bere WPM od R za pravdu; jiná definice WPM u R by ho poškodila (to, že v2 hlásí
stejné WPM jako R u čitelných stanic, ukázal pilot).

---

## 8. Kde se ztratí přečtená značka, a opravy za dekodérem (2026-11-10)

**Metoda.** `read-vs-feed.py --why` rozdělí každou CQ stanici R, kterou L přečetl a nespotoval, podle místa,
kde ji cesta za dekodérem ztratila: `c` řádky tap-replaye (kandidát extraktoru, záznam stanice, verdikt hand
gate) a události feedu (hold, drop, fold, send, spot). Nahrávky 1 a 2, 30/20/40 m, base, 310 případů.

| kde se ztratí | n | % | z toho R-SNR < 10 / ≥ 10 |
|---|---|---|---|
| extraktor značku nenabídl: roztržená mezerami („DF 2 SJ“, „Y U 7 B“) | 51 | 16.5 | 7 / 44 |
| — slepená s dalším textem („RN3QBGRN3QBG“, „IU7RQCK“) | 39 | 12.6 | 8 / 31 |
| — samostatné slovo | 7 | 2.3 | 2 / 5 |
| kandidát bez záznamu stanice | 25 | 8.1 | 6 / 19 |
| stanice nevolá: CQ/TEST/QRZ v textu je, okno ho ke značce nedotáhne | 71 | 22.9 | 11 / 60 |
| — v textu žádný volací marker (R ji má jako CQ, L ne: „N4RSS TESTING“) | 47 | 15.2 | 9 / 38 |
| hand gate: score / jedno čtení / near-call | 9 / 16 / 19 | 14.2 | 6 / 38 |
| feed: drop po holdu / fold / dedup spot_out | 1 / 0 / 3 | 1.3 | 1 / 3 |
| spot vyšel, ale přes 1.5 kHz od R / jiné | 7 / 15 | 7.1 | 2 / 20 |

**Závěry.**
1. **Politika feedu skoro nic nestojí:** hold, fold, `min_score`, near-call dohromady 15 %. Knoflíky mezeru nezavřou.
2. **69 % je čtení textu v extraktoru:** roztržené a slepené značky a CQ, které se ke značce nedostane přes
   rozbitá slova mezi nimi. Plošné povolení (delší okno, šumová písmena okno nestárnou, slepené TEST) už bylo
   zkoušeno a odmítnuto (`LIVE-AIR.md`, ~+1 recall za +0.6–1.3 bodu bustů). Textová heuristika je u konce;
   roztrženou značku musí potvrdit zvuk — vynucené zarovnání (krok 5) a součet přes opakování (krok 6).
3. **Frekvence spotu:** záznam stanice drží polohu nejsilnějšího čtení, jaké kdy bylo, a stejnou značku
   slučuje v ±2.5 kHz (`SKIM_STATION_SAMECALL_HZ`, kvůli ghostům). Stanice, která se posune o pár set Hz
   (odpověď S&P, pak CQ na své frekvenci), se spotuje na staré poloze: 2–6 % spotů nahrávek a 3–9 % živých
   tapů leží přes 200 Hz od dekódů, 10.5 % spotů je přes 300 Hz od R. Compare je počítá jako chycené
   (`df_khz` 1.5), uživatel RBN ale dostane špatnou frekvenci.

**Opravy (opt-in, bez nich golden byte-identický).** Měřeno tap-replayem nahrávek 1 a 2 a 5 dní živých tapů
(2026-10-25 15:40 – 10-30 17:44, 7 pásem, 11 176 stanic R, compare session 20261022-200129):

| přepínač | rec 1 recall / busty | rec 2 recall / busty | živé tapy recall / busty | spoty > 300 Hz od R |
|---|---|---|---|---|
| — | 62.9 / 15 | 68.0 / 40 | 65.1 / 370 (4.6 %) | 10.5 % |
| `SKIM_ST_REPOS=20` | 62.8 / 15 | 67.9 / 39 | 65.2 / 366 (4.6 %) | 8.8 % |
| `SKIM_CALL_CQWORDS=1` | 63.3 / 16 | 68.5 / 40 | 65.5 / 377 (4.7 %) | — |
| obojí | 63.2 / 16 | 68.3 / 39 | 65.5 / 374 (4.6 %) | — |

- `SKIM_ST_REPOS=<s>` (`station.c`): poloha, kterou žádné čerstvé čtení (±100 Hz) nepotvrdilo déle než
  `s` sekund, ustoupí jistému čerstvému čtení jinde hned, ne EMA krokem silnějšího. Ghosty živého signálu
  ji nepohnou, jeho kanál polohu stále potvrzuje. 10 / 20 / 40 s vychází stejně. Čistá oprava kvality:
  recall beze změny, busty o 1–4 méně, spotů daleko od R o šestinu méně.
- `SKIM_CALL_CQWORDS=1` (`callsign.c`): DX/POTA/SOTA/WWFF/IOTA a kontinenty za CQ nestárnou volací okno,
  slepené („CQPOTA“, „CQCQSOTA“) se odloupnou jako DX a TEST, v otevřeném okně je osamělé „D“ + „E“/„X“
  roztržené DE/DX („CQ D X D E EA6ACA“). +0.4 recallu za +7 bustů na 5 dnech (přidané spoty ~10 % bustů).
- Doporučení: REPOS zapnout jako výchozí (vlastní commit s `golden --update`); CQWORDS je malý obchod,
  nechat opt-in do živé zkoušky.

## 9. Soft-combining opakování: oracle před stavbou (krok 6, 2026-11-10)

**Metoda.** `SKIM_ORACLE_MODE=combine` (`decode_cw_v2.c`, `oc_word`): u každého slova, které dekodér uzavře
v intervalu R, se na LLR úsek slova (a slova s předchozím, kvůli roztrženým značkám) vynutí značka R
a porovná s volným čtením úseku (`dC` ≤ 0) a s nejlepší slovníkovou značkou poblíž značky i slova
(`dComp`) — totéž, co dělá re-read (`SKIM_V2_RESCORE`: fit 5, sep 3, ev 2). Dekódy zůstávají
byte-identické. `combine-oracle.py` pak u každé CQ stanice R vezme nepřekrývající se úseky od nejlepšího
(jen ty, které mohou být kopií značky: `dC` ≥ −15) a ptá se, kolik stanic projde s jedním, dvěma
a třemi čteními. Nahrávky 1 a 2, 30/20/40 m, base a s oracle gatesoft (krok 1 + 6 dohromady).

| CQ stanice R, třída v base | R-SNR | n | úsek v okně R base / gatesoft | projde 1 čtení base / gatesoft | 2–3 čtení přidají |
|---|---|---|---|---|---|
| miss | < 10 | 132 | 32 / 61 | 1 / 6 | 0 / 0 |
| miss | ≥ 10 | 169 | 67 / 83 | 7 / 11 | 0 / 0 |
| near | < 10 | 33 | 16 / 22 | 1 / 11 | 0 / 0 |
| read | < 10 | 52 | 37 / 42 | 14 / 21 | 0 / 0 |
| spot | vše | 1100 | 853 / 873 | 502 / 521 | 0 / 0 |

Ani bez fitu (jen součet marginu proti sousedům, bez ochrany proti špatnému sousedovi) kombinace nic
nepřidá: miss < 10 dB 1 → 1 (base) a 8 → 8 (gatesoft).

**Závěry.**
1. **Sčítat není co.** Pod 10 dB nemá 3/4 zmeškaných stanic v okně R žádné dekódované slovo (gate
   zavřený); s měkkým gate polovina. Kde slovo je, značka R na něm sedí o medián 350–560 jednotek (podle pásma)
   log-věrohodnosti hůř než volné čtení (práh re-readu −5) — slovo je jiný úsek, ne zašuměná kopie značky.
2. **Druhá věrohodná kopie téže značky je vzácná** (~9 % intervalů R má dva nepřekrývající se úseky
   s `dC` ≥ −15) a kde je, projde už první. Fit je průměr přes úseky brané od nejlepšího, takže další
   čtení ho nezvednou; může pomoct jen marginu, a tam se neukázal ani jeden případ.
3. **Odhad +3–5 dB z §5 krok 6 pro sčítání slovních úseků neplatí.** Měkký gate (krok 1) dá úseky, ale ty
   projdou samy (miss+near < 10 dB 2 → 17 stanic), ne součtem.
4. **Co zůstává otevřené:** úseky jsou segmentace dekodéru (k ≤ 2 slova). Sčítání, které na ní nezávisí —
   klouzavé vynucené zarovnání celé značky na LLR prstenec přes opakování CQ — tento oracle neměří.
   Je to krok 5 (vynucené zarovnání) rozšířený o součet, a měřit ho má až po kroku 5.

## Zdroje
- Repozitář: https://github.com/ok1hra/skimmer-for-linux (`src/engine/decode_cw_v2.c`, `pipeline.c`,
  `channelizer.c`, `tone_split.c`, `docs/WEAK-SIGNAL.md`, `docs/LIVE-AIR.md`)
- E. L. Bell, NPS 1977 – https://apps.dtic.mil/sti/pdfs/ADA046503.pdf ; DTIC ADA051001
- AG1LE: Bayesovský dekodér a rady VE3NEA – https://ag1le.blogspot.com/2013/01/towards-bayesian-morse-decoder.html
- VE3NEA GRITTY (squelch podle pravděpodobnosti chyby) – https://www.dxatlas.com/Gritty
