# SifuCoop: delovanje, namestitev in nadaljnji razvoj

Ta dokument razloži SifuCoop v slovenščini. Namenjen je igralcu, preizkuševalcu in
razvijalcu, ki želi razumeti, kaj projekt dejansko počne, česa še ne dokazuje in kako ga
varno nadaljevati.

## 1. Trenutno stanje

SifuCoop je eksperimentalna modifikacija za dva igralca. Izvorna koda, orodja, zaganjalnik
in namestitveni program tvorijo smiselno celoto ter se lahko zgradijo v en distribucijski
paket. To potrjuje, da so programski deli med seboj skladni na ravni prevajanja, povezovanja
in osnovnih kriptografskih testov.

To še ni dokaz, da celotna kampanja brezhibno deluje. Pravi boj lahko potrdita samo dve
ločeni kopiji podprte različice igre Sifu na dveh računalnikih. Posebej je treba preizkusiti
šefe, prehod med vsemi stopnjami, filmske prizore, smrt in ponovno vstajenje ter daljše
boje z veliko sovražniki.

Pravilna oznaka projekta je zato:

- arhitektura je izvedljiva in notranje skladna;
- namestitev in gradnja imata varovalke;
- osnovni omrežni protokol ima določeno avtoriteto in preverjanje paketov;
- popoln prehod skozi igro še ni potrjen;
- projekt ostaja eksperimentalen, dokler ne prestane ponovljivih testov na dveh računalnikih.

## 2. Osnovna zamisel

Vsak igralec poganja svojo običajno enoigralsko kopijo Sifuja. Igra nima vključene
uporabne večigralske strežniške izvedbe, zato SifuCoop ne uporablja običajne Unreal Engine
replikacije. Namesto tega obe kopiji igre izmenjujeta lastne UDP-pakete.

Na vsakem računalniku obstajajo:

1. pravi lokalni igralec;
2. lokalno ustvarjena lutka, ki predstavlja oddaljenega igralca;
3. lokalne kopije vseh sovražnikov in stopnje;
4. SifuCoop, ki odloča, kateri računalnik je avtoritativen za posamezno stanje;
5. predstavitveni sloj, ki prejete rezultate prikaže v lokalni kopiji igre.

Poenostavljen tok:

```text
lokalni igralec
    -> zajem položaja, gibanja, zdravja in dejanj
    -> overjen UDP-paket
    -> druga kopija Sifuja
    -> oddaljena lutka in opazovane kopije sovražnikov
```

Računalnika si ne delita pomnilnika, fizike, umetne inteligence ali zadetkov. Pošiljata si
rezultate in jih poskušata prikazati dovolj dosledno, da oba igralca vidita isti boj.

## 3. Kako se mod naloži

Končni `dsound.dll` je posredniška knjižnica. Sifu jo najde ob svoji izvršni datoteki
in jo naloži ob zagonu. Knjižnica posreduje prave DirectSound izvoze sistemski knjižnici,
zato zvok še vedno deluje, hkrati pa zažene SifuCoop.

Zagon je namenoma obramben:

1. procesna ključavnica prepreči dve aktivni kopiji moda na istem računalniku;
2. preverita se časovni žig in velikost izvršne datoteke Sifu;
3. izbere se tabela naslovov za ustrezno Epic ali Steam različico;
4. mod počaka, da sta pogon in nalaganje dovolj pripravljena;
5. vzpostavi se Unrealov refleksijski most;
6. prebere se `SifuCoop.ini` in zažene omrežje;
7. namestijo se sistemi za igralca, lutko, sovražnike, ukaze in teste;
8. namesti se uporabniški vmesnik in glavna posodobitvena kljuka.

Če prstni odtis igre ni znan ali manjka ključen naslov, se mod ne sme aktivirati. Bolje je,
da ostane nedejaven, kakor da pokliče napačen naslov in poškoduje stanje procesa.

## 4. Omrežje

SifuCoop uporablja neblokirajoči UDP na privzetih vratih `7777`. Ne potrebuje in ne
vključuje nobenega VPN-proizvoda. Namestitveni program ne prenaša dodatne omrežne programske
opreme.

Igralca se lahko dosežeta na enega od naslednjih načinov:

- isti krajevni LAN;
- obstoječ zasebni VPN oziroma navidezni LAN;
- posredovanje UDP-vrat na usmerjevalniku gostitelja;
- vgrajeno sočasno prebijanje NAT, kadar ga oba usmerjevalnika dopuščata.

Zasebni naslov, kot je `192.168.x.x` ali `10.x.x.x`, deluje samo, če ima drugi
računalnik omrežno pot do istega zasebnega omrežja. Prek javnega interneta brez VPN-ja je
običajno potreben javni naslov in posredovanje vrat ali prebijanje NAT.

### Vloge

- Gostitelj posluša na izbranih UDP-vratih in je končna avtoriteta za zdravje, poškodbe in
  smrt sovražnikov.
- Odjemalec se poveže z naslovom gostitelja, pošilja stanje svojega igralca in lahko začasno
  vodi dejanja bližnjih sovražnikov, če je ta možnost omogočena.
- Način `off` izključi omrežno sejo.

Povezava se začne s paketoma `Hello` in `Welcome` ter naključnima vrednostma seje.
Glava vsakega paketa vsebuje podpis projekta, različico protokola, vrsto paketa, zaporedno
številko, čas pošiljanja in 8-bajtno oznako HMAC-SHA256.

Geslo zagotavlja pristnost, ne šifriranja. Vsebina paketov ni skrivna. Prazno geslo uporablja
splošno znan privzeti ključ, zato je primerno le za zaupanja vreden LAN ali zasebni VPN.
Pri javno dostopnih vratih vedno nastavi dolgo, naključno in enako geslo na obeh računalnikih.

## 5. Delitev avtoritete

Najpomembnejše pravilo projekta je, da za eno lastnost ne odločata oba računalnika hkrati.

| Stanje | Avtoriteta | Naloga drugega računalnika |
| --- | --- | --- |
| Vnos lokalnega igralca | računalnik tega igralca | posodobi oddaljeno lutko |
| Položaj in gibanje igralca | računalnik tega igralca | interpolira in prikaže |
| Zdravje lokalnega igralca | računalnik tega igralca | prikaže prejeto vrednost |
| Zdravje in smrt sovražnika | gostitelj | uporabi potrjeno stanje |
| Dejanja sovražnika | natanko en lastnik boja | ustavi konkurenčno lokalno AI |
| Položaj sovražnika | trenutni lastnik boja | interpolira ali nastavi prejeto stanje |
| Sprememba stopnje | gostitelj | odjemalec sprejme povabilo ali samodejno sledi |

Za vsakega sovražnika mora v vsakem trenutku veljati:

- ena avtoritativna evidenca zdravja in smrti;
- en odločevalec napadov;
- en avtoritativni pisec položaja;
- poljubno število opazovalcev, ki ne smejo skrivaj znova zagnati AI.

Če se to pravilo prekrši, nastanejo dvojne poškodbe, drsenje, teleportiranje, napadi v prazno,
stoječa trupla ali sovražniki, ki po smrti znova oživijo.

## 6. Oddaljeni igralec

Oddaljeni igralec ni pravi drugi Unrealov igralec. Je lokalna vizualna lutka. Prejemnik iz
omrežnih posnetkov obnovi njen položaj, rotacijo, hitrost, zdravje, guard, stanje na tleh,
starost in videz, kjer je to podprto.

Posnetki se privzeto pošiljajo 60-krat na sekundo. Prejemnik jih ne sme samo slepo uporabiti;
med zaporednimi stanji interpolira, prilagodljivo upošteva zakasnitev in zavrne nezdružljive
ali zastarele podatke.

Ponovitev napada oddaljenega igralca je predvsem vizualna. Če bi na drugem računalniku znova
ustvarila pravi hitbox, bi se ista akcija lahko razrešila dvakrat ali poškodovala napačnega
igralca. Zato je prikaz oddaljenih napadov v sodelovalnem načinu omejen in ne sme postati
drugi vir resnice o poškodbah.

## 7. Sovražniki, poškodbe in smrt

Sovražniki so najzahtevnejši del. Obe igri sta ustvarili svoje primerke istega sovražnika,
vendar se njuni AI, animacije, trki in čas lahko hitro razidejo.

Gostitelj periodično pošilja identiteto, položaj, hitrost, zdravje, guard, skupne prijavljene
poškodbe, časovno dilatacijo ter zastavice za aktivnost, padec, cilj in smrt. Velike skupine
se razdelijo na več omejeno velikih paketov.

Odjemalčevi udarci se gostitelju ne pošiljajo kot enkratni ukaz »odštej 30«. Pošilja se
naraščajoča skupna vrednost. Gostitelj uporabi samo razliko od že priznane vrednosti in nato
vrne, koliko je upošteval. Tako podvojen paket ne povzroči dvojne poškodbe, izgubljen paket
pa popravi naslednje stanje.

Smrt mora biti končno stanje do potrjenega novega življenjskega cikla igralnega objekta.
Oddaljena kopija mora ustaviti AI, uporabiti pravilen smrtni prehod in zavrniti stare pakete,
ki bi truplo ponovno premaknili ali prebudili. Sama animacija padca ni dovolj; sama nastavitev
zdravja na nič prav tako ni dovolj.

## 8. Stopnje in napredovanje

Gostitelj lahko povabi odjemalca v trenutno stopnjo. Odjemalec povabilo sprejme na zavihku
`Play` ali omogoči samodejni vstop. Prehod mora počistiti stare kazalce na svet, igralce in
sovražnike ter nato po nalaganju znova poiskati lokalne objekte.

Napredovanje ni skupen profil:

- vsak igralec ohrani lastno starost in števec smrti;
- svetišča, nadgradnje, odklepi in shranjena igra ostanejo lokalni;
- zdravje in videz partnerja se lahko zrcalita za prikaz;
- šefi in filmski prizori ostajajo posebej tvegani, ker uporabljajo močno skriptirane dogodke.

»Skupno napredovanje skozi stopnjo« pomeni skupne boje in sledenje gostiteljevi stopnji, ne
združevanja obeh shranjenih iger.

## 9. Namestitev in odstranitev

Oba igralca morata imeti zakonito kopijo enake podprte različice Sifuja.

1. Razširi distribucijski arhiv.
2. Zaženi `SifuCoopInstaller.exe` iz iste mape kot `dsound.dll` in
   `SifuCoop.ini`.
3. Izberi mapo Sifu ali mapo `Sifu\Binaries\Win64`.
4. Pusti obstoječo konfiguracijo, razen če jo želiš namenoma zamenjati.
5. Po namestitvi zaženi igro in odpri meni s tipko F1.

Namestitveni program zavrne spremembe, ko igra teče. Obstoječi `dsound.dll` in po
potrebi konfiguracijo najprej kopira v časovno označeno mapo `SifuCoop-backup`.
Za ročno odstranitev odstrani datoteki moda ali obnovi varnostno kopijo, če je prej obstajala
druga posredniška knjižnica.

Namestitveni program je namenjen samo SifuCoopu. Ne prenaša, ne namešča in ne nastavlja
programov tretjih oseb.

## 10. Prva povezava

1. Na obeh računalnikih nastavi enaka vrata in enako geslo.
2. Gostitelj izbere F1 → `Setup` → `Host the game` → `Connect`.
3. Drugi igralec izbere `Join a game`, vpiše dosegljiv naslov gostitelja in izbere
   `Connect`.
4. Ko oba vidita stanje `Connected` in zakasnitev, gostitelj naloži stopnjo.
5. Gostitelj na zavihku `Play` izbere `Start co-op here`.
6. Odjemalec izbere `Join them` ali uporabi samodejni vstop.

Najprej preveri gibanje obeh igralcev. Nato preveri enega sovražnika, njegovo zdravje in
smrt. Šele nato preizkusi polno sobo in prehod v naslednje območje.

## 11. Znane omejitve

- Podprta sta največ dva igralca.
- Obe strani potrebujeta isto različico protokola in podprt prstni odtis igre.
- Natančna vrsta oddaljenega sovražnikovega udarca se lahko razlikuje, čeprav je čas napada
  približno pravilen.
- Nekatere oddaljene animacije potrebujejo lokalno zajet vzorec napada; po nalaganju stopnje
  lahko pomaga, da lokalni igralec enkrat udari v prazno.
- Napredovanje profila ni sinhronizirano.
- Šefi in filmski prizori niso potrjeni.
- Izguba gostitelja nima prave migracije avtoritete.
- Zlonameren partner s pravim geslom ni del varnostnega modela.

## 12. Kaj pomeni »deluje v teoriji«

Teoretična pot je celovita, če:

1. knjižnica prepozna izvršno datoteko in varno namesti kljuke;
2. oba procesa uspešno opravita overjen pozdrav;
3. lokalni igralec odda veljavne posnetke;
4. druga stran ustvari in posodablja lutko;
5. oba procesa iste lokalne sovražnike povežeta z isto omrežno identiteto;
6. za sovražnika obstaja en lastnik odločitev in gostiteljeva evidenca zdravja;
7. izgubljen ali podvojen paket ne podvoji trajnega dogodka;
8. sprememba stopnje počisti staro stanje;
9. neznana različica igre ostane nedejavna.

Projekt ima za te korake implementirane poti. Največja teoretična nevarnost ni sam UDP,
temveč prehod lastništva sovražnika med dvema že delujočima lokalnima simulacijama. Zato
prevajanje brez napak pomeni »program je sestavljiv«, ne »vsi boji so dokazano pravilni«.

## 13. Razvoj in preverjanje

Za spremembe potrebuješ Windows, C++17 prevajalnik WinLibs/MinGW, Python in podprto namestitev
Sifuja. Hitra gradnja uporablja že ustvarjene tabele naslovov:

```powershell
.\build.ps1 -NoGen
```

Polna gradnja iz PDB-ja obnovi naslove za izbrano izvršno datoteko:

```powershell
.\build.ps1 -GameDir "pot-do\Sifu\Binaries\Win64" -LocalBuildName steam
```

Naslovov v `src/core/offsets.g.h` ne popravljaj ročno. Za vsak nov simbol spremeni
vhode orodja, obnovi tabeli za Epic in Steam ter preveri, da zahtevane vrednosti niso nič.

Minimalni test pred objavo:

- kriptografski testni vektorji uspejo;
- vsi cilji se prevedejo brez opozoril;
- distribucija vsebuje DLL, INI, namestitveni program, zaganjalnik in navodila;
- obe igri v dnevniku prepoznata pravi build in isto različico protokola;
- povezava, gibanje in ponovna povezava delujejo v obe smeri;
- oba igralca izmenično udarita istega sovražnika;
- sovražnik umre enkrat na obeh zaslonih;
- smrt in vstajenje igralca ne pokvarita izbire ciljev;
- prehod stopnje ne uporabi starih objektov;
- daljši boj ne povzroči naraščanja zamika ali napačnega lastništva.

Glavni dnevnik je:

```text
%LOCALAPPDATA%\Sifu\Saved\Logs\SifuCoop.log
```

Vedno primerjaj dnevnika obeh računalnikov od zadnje vrstice
`=== SifuCoop attached` naprej. Brez obeh pogledov je večino napak sinhronizacije
nemogoče zanesljivo razložiti.

## 14. Merilo za stabilno izdajo

Projekt je primeren za označitev kot stabilen šele, ko isti distribucijski paket večkrat
uspešno opravi določeno testno matriko na Epic↔Epic, Steam↔Steam in Epic↔Steam, brez ročnih
popravkov med sejo. Rezultati morajo vključevati različne zakasnitve in izgubo paketov,
navadne sobe, smrt obeh igralcev, ponovno povezavo, prehode stopenj in vsaj enega šefa.

Do takrat naj README in izdaje jasno uporabljajo izraze »experimental«, »known limits« in
»two-PC testing required«. To ni slabost dokumentacije, temveč pošten opis meje med
preverjeno programsko zgradbo in vedenjem zaprte igre v živo.
