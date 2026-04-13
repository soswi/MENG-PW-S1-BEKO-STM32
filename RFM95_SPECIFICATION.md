# 📡 SPECYFIKACJA POŁĄCZENIA RFM95 LoRa

## 🎯 MODUŁ RFM95 - CZYM JEST?

```
Producent: HopeRF
Chipset: Semtech SX1276
Standard: LoRa (Long Range)
Zasięg: ~10 km (LOS - Line of Sight)
Moc: 100 mW (20 dBm max)
```

---

## 📊 PARAMETRY BIBLIOTEKI (pyLoraRFM9x)

### 1. CZĘSTOTLIWOŚĆ (FREQUENCY)

```python
# Default w projekcie BEER-TEAM
FREQUENCY_MHZ = 868.0  # MHz

# Opcje:
EU (Europe):   868.0 MHz  ← TWOJE USTAWIENIE
USA:           915.0 MHz
China:         433.0 MHz (SX1278)
Asia:          433.0 / 868.0 MHz
```

**Dlaczego 868 MHz?**
```
✓ ISM Band (Industrial, Scientific, Medical)
✓ Niskie opłaty za licencję
✓ Swobodne użytkowanie w Europie
✓ Długi zasięg + niska moc
```

---

### 2. MODULACJA (MODULATION)

```
RFM95 wspiera 2 rodzaje:
├─ LoRa (default)      ← To co używacie!
└─ FSK (Frequency Shift Keying)

LoRa parametry:
├─ Spreading Factor (SF): 6-12
├─ Bandwidth (BW):       125 kHz - 500 kHz
├─ Coding Rate (CR):     4/5 - 4/8
└─ Preamble Length:      8 bytes (default)
```

**LoRa vs FSK:**
```
LoRa (Long Range):
✓ Długi zasięg (~10 km)
✓ Niska moc
✓ Wolne (~300 bit/s)
✓ Odporna na zakłócenia
✗ Wolniejsza

FSK (Frequency Shift Keying):
✓ Szybka (~20 kbit/s)
✓ Mniejsza moc CPU
✗ Krótszy zasięg (~1 km)
✗ Mniej odporna
```

---

### 3. SPREADING FACTOR (SF)

```
Default: SF = 7

Wartości: 6, 7, 8, 9, 10, 11, 12

SF = 6:  Najszybszy (5.5 kbit/s), krótszy zasięg
SF = 7:  Balance (2.5 kbit/s)        ← TUTAJ
SF = 12: Najwolniejszy (250 bit/s), najdłuższy zasięg

Wzór czasu transmisji:
Time = (2^SF) / Bandwidth

SF 7 @ 125 kHz:
Time = 2^7 / 125,000 = 128 / 125,000 ≈ 1 ms per symbol
```

**Wpływ SF:**
```
SF↑ = Zasięg↑, Prędkość↓, Moc↓
SF↓ = Zasięg↓, Prędkość↑, Moc↑
```

---

### 4. BANDWIDTH (SZEROKOŚĆ PASMA)

```
Default: 125 kHz

Opcje: 7.8, 10.4, 15.6, 20.8, 31.25, 41.7, 62.5, 125, 250, 500 kHz

BW = 125 kHz  ← Standard (balans)
BW = 250 kHz  ← Szybciej
BW = 62.5 kHz ← Bardziej czuły

Wzór data rate:
DataRate = SF * (BW / 2^SF) * CodingRate * 8 bits/byte

Przykład (SF7, BW125, CR4/5):
DataRate = 7 * (125,000 / 128) * 0.8 * 8 ≈ 5.5 kbit/s
```

---

### 5. CODING RATE (KODOWANIE)

```
Default: CR = 5 (czyli 4/5)

Opcje: 5, 6, 7, 8 (oznaczają 4/5, 4/6, 4/7, 4/8)

CR = 4/5 (5): Najmniej redundancji, szybciej (187.5%)
CR = 4/6 (6): Balance                   (200%)    ← TUTAJ
CR = 4/7 (7): Więcej redundancji        (233%)
CR = 4/8 (8): Najwyższa odporność      (266%)

Wskaźnik = Original_Data / Transmitted_Data
```

**Wpływ CR:**
```
CR↓ = Prędkość↑, Odporność↓
CR↑ = Prędkość↓, Odporność↑ (lepszy w szumie)
```

---

## 📋 OBECNA KONFIGURACJA (TVÓJ PROJEKT)

### config.py

```python
# Frequency
FREQUENCY = 868.0  MHz  ← ISM Band Europa

# LoRa Settings (teoretyczne, sprawdź kod)
SPREADING_FACTOR = 7    ← Balance (nie najszybciej, nie najdalej)
BANDWIDTH = 125000      ← 125 kHz (standard)
CODING_RATE = 5         ← 4/5 (balans)

# TX Settings
TX_POWER = 20 dBm       ← Maximum (100 mW)
```

---

## 🔍 CO ROBI BIBLIOTEKA (ADAFRUIT RFM9X)

### Init RFM95

```python
import adafruit_rfm9x

rfm9x = adafruit_rfm9x.RFM9x(spi, cs, board.D4, board.D17)

# Domyślne wartości:
rfm9x.frequency_mhz = 868.0
rfm9x.tx_power = 13  # dBm (zmienia się)
rfm9x.spreading_factor = 7
rfm9x.signal_bandwidth = 125000  # Hz
rfm9x.coding_rate = 5

# Opcjonalne ModemConfig
# (predefiniowane zestawy parametrów)
```

---

## 📦 PACKET STRUCTURE (STRUKTURA PAKIETU)

```
RFM95 Packet:
┌─────────────────────────────────────┐
│ Preamble (8 bytes)                  │ Synchronizacja
├─────────────────────────────────────┤
│ Header (4 bytes)                    │ Info o pakiecie
├─────────────────────────────────────┤
│ Payload (0-255 bytes)               │ Twoje dane ← Tutaj!
├─────────────────────────────────────┤
│ CRC (2 bytes)                       │ Sprawdzenie
└─────────────────────────────────────┘
```

**Maksymalny rozmiar:**
```
255 bytes payload
Twoje wiadomości: max 255 znaków
```

---

## 📡 AIR TIME (CZAS TRANSMISJI)

```
Dla SF7, BW125, CR4/5:

Payload 100 bytes:
AirTime ≈ 200 ms

Payload 255 bytes:
AirTime ≈ 500 ms

Czyli wysyłasz 1 pakiet co 2-5 sekund
```

---

## 🔊 POWER LEVELS (POZIOMY MOCY)

```
TX_POWER range: -4 do 20 dBm

-4 dBm  = 0.4 mW   (bardzo niska)
0 dBm   = 1 mW
5 dBm   = 3 mW
10 dBm  = 10 mW
15 dBm  = 32 mW
20 dBm  = 100 mW   ← MAXIMUM (twoja ustawienie)

Wyższe = Zasięg↑, Pobór prądu↑, Ciepło↑
```

---

## 📊 RECEIVER SENSITIVITY (CZUŁOŚĆ ODBIORNIKA)

```
RFM95 typowe czułości:
SF6:  -121 dBm
SF7:  -126 dBm  ← TUTAJ
SF8:  -129 dBm
...
SF12: -137 dBm (najczulszy)

Oznacza: Słyszysz sygnały do -126 dBm siły
```

---

## 🎛️ REGULATORY LIMITS (OGRANICZENIA PRAWNE)

```
Europa (868 MHz ISM):
├─ Moc TX: max 20 dBm (100 mW)           ✓ Twoja ustawienie
├─ Duty Cycle: 1% (na najniższym poziomie)
└─ Kanały: 868.0, 868.3, 868.5 MHz

Duty Cycle = (AirTime) / (Czas całkowity) ≤ 1%

Przykład:
- Wysyłasz 100 byte co 2 sekundy (200 ms air time)
- 200 ms / 2000 ms = 10% ← ZA WIELE!
- Musisz czekać co 20 sekund ← OK (1%)
```

---

## 🔄 SYNCHRONIZACJA MIĘDZY URZĄDZENIAMI

```
Aby się łączyć, oba urządzenia muszą mieć:
✓ Tę samą frequency:      868.0 MHz
✓ Ten sam SF:             7
✓ To samo BW:             125 kHz
✓ Tę samą CR:             4/5
✓ Tę samą preamble length: 8

Różne:
- TX Power (może być inny)
- Device ID (może być inny)
- Payload (może być inny)
```

---

## 📋 CHECKLIST - TWOJE USTAWIENIA

```
✓ Frequency:        868.0 MHz      (Europa ISM)
✓ Modulation:       LoRa           (długi zasięg)
✓ Spreading Factor: 7              (balance)
✓ Bandwidth:        125 kHz        (standard)
✓ Coding Rate:      4/5            (balans)
✓ TX Power:         20 dBm         (maximum)
✓ Max Payload:      255 bytes

Wynikowa prędkość:  ~5.5 kbit/s
Air Time (100B):    ~200 ms
Zasięg (LOS):       ~5-10 km
```

---

## 🎓 GDZIE ZNALEŹĆ WIĘCEJ INFO

### Datasheet (SX1276 chipset)
```
https://cdn-shop.adafruit.com/datasheets/sx1276.pdf

Sekcje:
- Register descriptions
- LoRa mode settings
- Frequency table
- Modulation settings
```

### Adafruit RFM9x Library
```
https://github.com/adafruit/Adafruit_CircuitPython_RFM9x

Patrz:
- adafruit_rfm9x.py
- examples/
```

### LoRa Alliance Spec
```
https://lora-alliance.org/

- LoRa Technical Overview
- Regional Parameters
```

---

## 🔬 JAK SPRAWDZIĆ PARAMTERY NA SWOIM PI

```bash
# W Python:
python3 << 'EOF'
import board
import busio
import digitalio
import adafruit_rfm9x

spi = busio.SPI(board.SCK, MOSI=board.MOSI, MISO=board.MISO)
cs = digitalio.DigitalInOut(board.CE0)
cs.switch_to_output()

rfm9x = adafruit_rfm9x.RFM9x(spi, cs, board.D4, board.D17)

print(f"Frequency: {rfm9x.frequency_mhz} MHz")
print(f"TX Power: {rfm9x.tx_power} dBm")
print(f"Spreading Factor: {rfm9x.spreading_factor}")
print(f"Signal Bandwidth: {rfm9x.signal_bandwidth} Hz")
print(f"Coding Rate: {rfm9x.coding_rate}")
print(f"Enable CRC: {rfm9x.enable_crc}")
print(f"Preamble Length: {rfm9x.preamble_length}")

# RSSI (Received Signal Strength Indicator)
print(f"\nRSSI (czułość): {rfm9x.last_rssi} dBm")
EOF
```

---

## 📊 PORÓWNANIE KONFIGURACJI

```
╔═══════════════╦═════════╦══════════╦═══════════╗
║ Parametr      ║ SF6     ║ SF7      ║ SF12      ║
╠═══════════════╬═════════╬══════════╬═══════════╣
║ Data Rate     ║ 11 kb/s ║ 5.5 kb/s ║ 250 b/s   ║
║ Zasięg        ║ 3 km    ║ 5 km     ║ 10 km     ║
║ Air Time 100B ║ 100 ms  ║ 200 ms   ║ 3000 ms   ║
║ Power CPU     ║ High    ║ Medium   ║ Low       ║
║ Odporność     ║ Słaba   ║ Dobra    ║ Bardzo    ║
╚═══════════════╩═════════╩══════════╩═══════════╝
```

---

## 🎯 TWOJA KONFIGURACJA - CO OZNACZA

```
868.0 MHz, SF7, BW125, CR4/5, 20dBm
         ↓     ↓    ↓    ↓    ↓
      Europe Balance    Good   Maximum
               ↓    ↓    ↓    ↓
            Medium Speed, Good Range, Full Power

Użycie: Idealny do testów, komunikacji na dystansie
        Dobry balance między zasięgiem a szybkością
```

---

## 📞 PYTANIA?

Czego chcesz wiedzieć więcej?

1. **Jak zmienić SF na bardziej odległy zasięg?** (SF12)
2. **Jak sprawdzić rzeczywiste parametry na Pi?**
3. **Jak zsynchronizować 2 Pi z innymi parametrami?**
4. **Jak mierzyć RSSI (siłę sygnału)?**
5. **Coś innego?**

**Daj znać!** 📡
