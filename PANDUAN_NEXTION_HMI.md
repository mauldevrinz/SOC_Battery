# 🖥️ PANDUAN LENGKAP: Membuat & Memasang GUI Nextion/TJC untuk BIMA SOC LFP

## Proyek: SOC & Battery Analyzer LiFePO4
- **MCU:** STM32F103C8T6
- **Display:** Nextion / TJC 2.4" atau 2.8" (320×240 pixel)
- **UART:** 9600 baud, 8N1

---

## ⚠️ KENAPA LAYAR MENAMPILKAN DEMO PABRIK?

Layar Nextion/TJC punya **chip prosesor sendiri + memori flash sendiri**.
Kalau belum diisi file project (.tft), layar akan menjalankan **demo bawaan pabrik** — itulah yang muncul saat Anda sentuh layar.

**Solusinya:** Upload file `.tft` project kita ke layar via MicroSD. Proses ini **hanya 5-10 menit** dan cuma perlu dilakukan **1× saja seumur hidup layar**.

---

## 📋 LANGKAH 1: Download Software Editor

### Jika Layar Merek **NEXTION** (Nextion NX3224T024 dll):
- Download: **Nextion Editor** dari [nextion.tech/nextion-editor](https://nextion.tech/nextion-editor/)
- Gratis, Windows only

### Jika Layar Merek **TJC** (Versi China):
- Download: **USART HMI Editor** dari [tjc1688.com](http://tjc1688.com/)
- Gratis, Windows only

> **Catatan:** Kedua software cara kerjanya 99% identik. Tutorial ini berlaku untuk keduanya.

---

## 📋 LANGKAH 2: Buat Project Baru

1. Buka Nextion Editor / USART HMI
2. Klik **File → New**
3. Pilih **model layar** yang sesuai:
   - Untuk Nextion: Pilih seri "Basic" → model terdekat (misal `NX3224T024_011`)
   - Untuk TJC: Pilih model 2.4" atau 2.8" basic
4. **Resolusi:** Pastikan terpilih **320 × 240** (landscape/horizontal)
5. **Encoding:** Pilih `ascii`
6. Klik OK → Save project (misal: `BIMA_SOC_LFP.HMI`)

---

## 📋 LANGKAH 3: Setting Global (Program.s)

1. Di panel kiri bawah, cari tab **"Program.s"** atau **"page Event → Preinitialize"**
2. Klik di area **Program.s** dan ketik kode berikut:

```
dim=100
baud=9600
thup=1
page 0
```

> **Penjelasan:**
> - `dim=100` = Kecerahan layar 100%
> - `baud=9600` = Baudrate UART sama dengan STM32
> - `thup=1` = Kirim koordinat sentuhan saat jari diangkat
> - `page 0` = Mulai dari halaman Home

---

## 📋 LANGKAH 4: Buat 7 Halaman (Page 0 – Page 6)

Di panel kanan bawah ada **"Page"** window.
- Sudah ada `page0` secara default
- Klik tombol **"Add"** atau **"+"** sebanyak **6 kali** untuk membuat total 7 halaman

Daftar halaman:
| Page | Nama Internal | Fungsi |
|------|---------------|--------|
| page0 | page0 | HOME MONITOR |
| page1 | page1 | MAIN MENU |
| page2 | page2 | QUICK CHECK |
| page3 | page3 | BATTERY ANALYZE |
| page4 | page4 | BATTERY SETUP |
| page5 | page5 | DATA LOGGER |
| page6 | page6 | HISTORY & TREND |

**Untuk setiap halaman:**
- Klik halaman → Di panel **Attribute** (kanan), set `bco` (background color) = **0** (Hitam)

---

## 📋 LANGKAH 5: Pasang Komponen di Setiap Halaman

### 🔧 Cara Menambah Komponen:
1. Klik halaman yang ingin diedit di panel Page
2. Di panel **Toolbox** (kiri), drag komponen ke canvas:
   - **t** = Text
   - **j** = Progress Bar
   - **b** = Button (Dual-State Button)
   - **n** = Number
3. Setelah komponen ditaruh, **klik komponen** → ubah properti di panel **Attribute** (kanan):
   - **objname** = nama objek (WAJIB SESUAI!)
   - **x, y, w, h** = posisi dan ukuran
   - **txt** = teks default
   - **pco** = warna teks (foreground color)
   - **bco** = warna background
   - **font** = ID font (0 = kecil, 1 = medium, 2 = besar)
   - **xcen** = alignment horizontal (0=kiri, 1=tengah, 2=kanan)
   - **ycen** = alignment vertikal (0=atas, 1=tengah)

### ⭐ PENTING: Pengaturan "Send Component ID" pada Tombol

Setiap tombol (Button) **WAJIB** diaktifkan event-nya:
1. Klik tombol di canvas
2. Di panel kanan, cari tab **"Event"**
3. Di bagian **"Touch Release Event"**, **centang** ✅ **"Send Component ID"**

Ini agar STM32 menerima notifikasi (packet 0x65) saat tombol disentuh.

---

## 📋 LANGKAH 5A: Detail Komponen Per Halaman

### ═══ PAGE 0: HOME MONITOR ═══

**Set background: bco = 0 (hitam)**

| # | Tipe | objname | x | y | w | h | txt/val | pco | bco | font | xcen | Keterangan |
|---|------|---------|---|---|---|---|---------|-----|-----|------|------|------------|
| 1 | Text | `tTitle` | 5 | 2 | 160 | 28 | "BIMA SOC LFP" | 65535 | 0 | 1 | 0 | Judul app |
| 2 | Text | `tCharge` | 168 | 2 | 40 | 28 | "BAT" | 2016 | 0 | 0 | 2 | Status cas |
| 3 | Text | `tVbatInt` | 210 | 2 | 50 | 14 | "4.12" | 2016 | 0 | 0 | 2 | V baterai internal |
| 4 | Progress | `jBatInt` | 210 | 18 | 50 | 10 | val=85 | 2016 | 16904 | - | - | Bar baterai internal |
| 5 | Text | `tStatus` | 262 | 2 | 55 | 28 | "READY" | 2016 | 0 | 0 | 2 | Status sistem |
| 6 | Text | `tVlbl` | 12 | 38 | 140 | 20 | "VOLTAGE (LFP)" | 31 | 0 | 0 | 0 | Label |
| 7 | Text | `tV` | 12 | 60 | 140 | 50 | "0.00" | 65535 | 0 | 2 | 1 | **Tegangan utama** |
| 8 | Text | `tIlbl` | 167 | 38 | 140 | 20 | "CURRENT" | 31 | 0 | 0 | 0 | Label |
| 9 | Text | `tI` | 167 | 60 | 140 | 50 | "0.00" | 65535 | 0 | 2 | 1 | **Arus utama** |
| 10 | Text | `tSOClbl` | 12 | 118 | 80 | 20 | "SOC (EKF)" | 31 | 0 | 0 | 0 | Label |
| 11 | Text | `tSOC` | 95 | 118 | 70 | 20 | "0.0" | 65535 | 0 | 1 | 1 | **Nilai SOC** |
| 12 | Text | `tSOHlbl` | 175 | 118 | 40 | 20 | "SOH" | 31 | 0 | 0 | 0 | Label |
| 13 | Text | `tSOH` | 218 | 118 | 70 | 20 | "100.0" | 65535 | 0 | 1 | 1 | **Nilai SOH** |
| 14 | Progress | `jSOC` | 12 | 142 | 296 | 16 | val=0 | 2016 | 16904 | - | - | **Bar gauge SOC** |
| 15 | Text | `tTemp` | 62 | 164 | 60 | 18 | "25.0" | 65535 | 0 | 0 | 0 | Suhu |
| 16 | Text | `tSD` | 130 | 164 | 80 | 18 | "READY" | 2016 | 0 | 0 | 0 | Status SD Card |
| 17 | Text | `tLog` | 218 | 164 | 90 | 18 | "0" | 65535 | 0 | 0 | 2 | Counter log |

**Tombol Navigasi (WAJIB centang "Send Component ID"):**

| # | Tipe | objname | Component ID | x | y | w | h | txt | Aksi |
|---|------|---------|-------------|---|---|---|---|-----|------|
| 18 | Button | `bMenu` | **3** | 10 | 190 | 95 | 42 | "MENU" | Buka Main Menu |
| 19 | Button | `bQuick` | **4** | 112 | 190 | 95 | 42 | "QUICK" | Buka Quick Check |
| 20 | Button | `bAnalyze` | **5** | 214 | 190 | 96 | 42 | "ANALYZE" | Buka Analyze |

> **⚠️ PENTING tentang Component ID:**
> Di Nextion, Component ID (id) otomatis diberikan berurutan mulai dari 0 sesuai urutan komponen ditambahkan.
> Tombol MENU **harus** punya id = **3**, QUICK = **4**, ANALYZE = **5**.
> Artinya, sebelum menambah 3 tombol ini, Anda perlu sudah menambahkan tepat 3 komponen lain terlebih dahulu (id 0, 1, 2).
>
> **Cara termudah:** Tambahkan komponen sesuai urutan tabel di atas. Atau, setelah semua ditambahkan, cek properti `id` di panel Attribute dan pastikan cocok.

---

### ═══ PAGE 1: MAIN MENU ═══

| # | Tipe | objname | Comp ID | x | y | w | h | txt | "Send Component ID" |
|---|------|---------|---------|---|---|---|---|-----|---------------------|
| 1 | Text | `tMenuTitle` | 0 | 5 | 2 | 310 | 30 | "MAIN MENU" | - |
| 2 | Button | `bM1` | **1** | 15 | 38 | 290 | 30 | "1. HOME MONITOR" | ✅ YA |
| 3 | Button | `bM2` | **2** | 15 | 71 | 290 | 30 | "2. QUICK CHECK" | ✅ YA |
| 4 | Button | `bM3` | **3** | 15 | 104 | 290 | 30 | "3. BATTERY ANALYZE" | ✅ YA |
| 5 | Button | `bM4` | **4** | 15 | 137 | 290 | 30 | "4. BATTERY SETUP" | ✅ YA |
| 6 | Button | `bM5` | **5** | 15 | 170 | 290 | 30 | "5. DATA LOGGER" | ✅ YA |
| 7 | Button | `bM6` | **6** | 15 | 203 | 290 | 30 | "6. HISTORY & TREND" | ✅ YA |

**Warna tombol:** bco = 16904 (abu gelap), pco = 65535 (putih)

---

### ═══ PAGE 2: QUICK CHECK ═══

**Komponen Teks (data dari STM32):**

| # | Tipe | objname | x | y | w | h | txt default | Keterangan |
|---|------|---------|---|---|---|---|-------------|------------|
| 1 | Text | `tQCtitle` | 5 | 2 | 310 | 28 | "QUICK CHECK" | Judul halaman |
| 2 | Text | `tVnow` | 110 | 36 | 80 | 22 | "0.00" | Tegangan sekarang |
| 3 | Text | `tVmin` | 240 | 36 | 70 | 22 | "0.00" | Tegangan minimum |
| 4 | Text | `tVmax` | 110 | 60 | 80 | 22 | "0.00" | Tegangan maksimum |
| 5 | Text | `tInow` | 110 | 88 | 80 | 22 | "0.00" | Arus sekarang |
| 6 | Text | `tIpeak` | 245 | 88 | 65 | 22 | "0.00" | Arus puncak |
| 7 | Text | `tSocOcv` | 110 | 116 | 80 | 22 | "0.0" | SOC dari OCV |
| 8 | Text | `tHold` | 110 | 140 | 60 | 22 | "RUN" | Status Hold |
| 9 | Text | `tIzero` | 230 | 140 | 80 | 22 | "1.690" | Zero offset |

**Tombol (centang "Send Component ID"):**

| Tipe | objname | Comp ID | x | y | w | h | txt |
|------|---------|---------|---|---|---|---|-----|
| Button | `bBack` | **1** | 5 | 195 | 100 | 40 | "BACK" |
| Button | `bHold` | **2** | 110 | 195 | 100 | 40 | "HOLD" |
| Button | `bReset` | **3** | 215 | 195 | 100 | 40 | "RESET" |

> **Tips Component ID:** Tambahkan teks judul (id=0) dulu, lalu tombol BACK (jadi id=1), HOLD (id=2), RESET (id=3). Kemudian baru tambahkan komponen Text lainnya (id 4, 5, dst). Komponen Text tidak perlu ID tertentu, hanya **objname** yang harus benar.

---

### ═══ PAGE 3: BATTERY ANALYZE ═══

| # | Tipe | objname | x | y | w | h | txt default |
|---|------|---------|---|---|---|---|-------------|
| 1 | Text | `tANtitle` | 5 | 2 | 310 | 28 | "BATTERY ANALYZE" |
| 2 | Text | `tSocOcv` | 165 | 40 | 140 | 24 | "0.0" |
| 3 | Text | `tSocEkf` | 165 | 68 | 140 | 24 | "0.0" |
| 4 | Text | `tSoh` | 165 | 96 | 140 | 24 | "100.0" |
| 5 | Text | `tR0` | 165 | 124 | 140 | 24 | "0.00" |
| 6 | Text | `tPwr` | 90 | 152 | 70 | 24 | "0.0" |
| 7 | Text | `tEnergy` | 220 | 152 | 90 | 24 | "0.0" |

**Tombol:**

| Tipe | objname | Comp ID | x | y | w | h | txt |
|------|---------|---------|---|---|---|---|-----|
| Button | `bBack` | **1** | 80 | 195 | 160 | 40 | "BACK" |

> Tambahkan judul (id=0) dulu, lalu tombol BACK (id=1), kemudian sisa teks.

---

### ═══ PAGE 4: BATTERY SETUP ═══

| # | Tipe | objname | x | y | w | h | txt default | Keterangan |
|---|------|---------|---|---|---|---|-------------|------------|
| 1 | Text | `tSUtitle` | 5 | 2 | 310 | 28 | "BATTERY SETUP" | Judul |
| 2 | Text | `lNomV` | 10 | 45 | 200 | 24 | "NOMINAL VOLTAGE:" | Label |
| 3 | Text | `tNomV` | 30 | 72 | 260 | 35 | "12.8" | Nilai volt nominal |
| 4 | Text | `lCapAh` | 10 | 115 | 200 | 24 | "CAPACITY RATING:" | Label |
| 5 | Text | `tCapAh` | 30 | 140 | 260 | 35 | "100.0" | Nilai kapasitas |

**Tombol (centang "Send Component ID"):**

| Tipe | objname | Comp ID | x | y | w | h | txt | Aksi |
|------|---------|---------|---|---|---|---|-----|------|
| Button | `bBack` | **1** | 80 | 195 | 160 | 40 | "BACK" | Kembali ke Home |
| Button | `bCycleV` | **2** | 30 | 72 | 260 | 35 | "12.8 V (4S)" | Ganti tegangan nominal |
| Button | `bCycleCap` | **3** | 30 | 140 | 260 | 35 | "100.0 Ah" | Ganti kapasitas |

> **Catatan:** Tombol bCycleV dan bCycleCap bisa ditumpuk di atas Text `tNomV` dan `tCapAh`, atau gunakan tombol saja (STM32 akan update teks tombol via `tNomV.txt="25.6"` dll).

---

### ═══ PAGE 5: DATA LOGGER ═══

| # | Tipe | objname | x | y | w | h | txt/val default |
|---|------|---------|---|---|---|---|-----------------|
| 1 | Text | `tDLtitle` | 5 | 2 | 310 | 28 | "DATA LOGGER" |
| 2 | Text | `tLogState` | 135 | 45 | 170 | 24 | "STOPPED" |
| 3 | Number | `tInterval` | 135 | 73 | 100 | 24 | val=1000 |
| 4 | Text | `tCount` | 135 | 101 | 170 | 24 | "0" |
| 5 | Text | `tWh` | 135 | 129 | 170 | 24 | "0.00" |
| 6 | Text | `tTime` | 135 | 157 | 170 | 24 | "00:00" |

> **Penting:** `tInterval` harus bertipe **Number** (n), bukan Text, karena STM32 mengirim `tInterval.val=1000`.

**Tombol:**

| Tipe | objname | Comp ID | x | y | w | h | txt |
|------|---------|---------|---|---|---|---|-----|
| Button | `bBack` | **1** | 5 | 195 | 150 | 40 | "BACK" |
| Button | `bToggle` | **2** | 165 | 195 | 150 | 40 | "START/STOP" |

---

### ═══ PAGE 6: HISTORY & TREND ═══

| # | Tipe | objname | x | y | w | h | txt default |
|---|------|---------|---|---|---|---|-------------|
| 1 | Text | `tHStitle` | 5 | 2 | 310 | 28 | "HISTORY & TREND" |
| 2 | Text | `tHistV` | 110 | 94 | 70 | 22 | "0.00" |
| 3 | Text | `tHistI` | 245 | 94 | 65 | 22 | "0.00" |
| 4 | Text | `tHistSoc` | 70 | 120 | 80 | 22 | "0.0" |
| 5 | Text | `tHistSoh` | 215 | 120 | 80 | 22 | "100.0" |
| 6 | Text | `tHistStat` | 80 | 148 | 220 | 22 | "OK" |

**Tombol:**

| Tipe | objname | Comp ID | x | y | w | h | txt |
|------|---------|---------|---|---|---|---|-----|
| Button | `bBack` | **1** | 80 | 195 | 160 | 40 | "BACK" |

---

## 📋 LANGKAH 6: Generate Font (JANGAN LUPA!)

Nextion butuh minimal 1 font file. Jika belum ada:

1. Klik menu **Tools → Font Generator**
2. Setting:
   - **Height:** 16 (untuk font 0), 24 (untuk font 1), 32 (untuk font 2)
   - **Encoding:** ascii
   - **Font:** Pilih font monospace (misal: Consolas, Courier New, atau Arial)
3. Klik **Generate Font** → Simpan sebagai `font0.zi`, `font1.zi`, `font2.zi`
4. Font otomatis masuk ke daftar font project

> Anda butuh minimal **3 font:**
> - Font ID 0 (kecil ~16px) untuk label
> - Font ID 1 (medium ~24px) untuk nilai data
> - Font ID 2 (besar ~32px) untuk angka utama (V, A)

---

## 📋 LANGKAH 7: Compile & Export File .TFT

1. Klik menu **Compile** (atau tekan `Ctrl+R` atau ikon ▶️)
2. Pastikan **0 Error** di panel output bawah
3. Klik **File → TFT File Output**
4. Pilih folder tujuan → Klik **Output**
5. File `.tft` akan tersimpan (misal: `BIMA_SOC_LFP.tft`)

---

## 📋 LANGKAH 8: Upload ke Layar Nextion via MicroSD

### Alat yang Dibutuhkan:
- 1× MicroSD card (FAT32, ukuran 2GB – 32GB)
- MicroSD reader / adapter

### Langkah-langkah:

```
┌─────────────────────────────────────────────────┐
│  1. FORMAT MicroSD ke FAT32                     │
│  2. COPY file .tft ke MicroSD                   │
│     ⚠️ HANYA 1 file .tft di dalam SD!           │
│  3. MATIKAN power layar Nextion                 │
│  4. MASUKKAN MicroSD ke slot belakang layar     │
│  5. NYALAKAN power layar                        │
│  6. Tunggu proses update (progress bar merah)   │
│     Sampai muncul "Update Successed!"           │
│  7. MATIKAN power layar                         │
│  8. CABUT MicroSD                               │
│  9. NYALAKAN lagi → SELESAI! ✅                  │
└─────────────────────────────────────────────────┘
```

### Detail Penting:
- **JANGAN** cabut power atau MicroSD saat proses upload berlangsung!
- Jika gagal, coba format ulang MicroSD dan pastikan hanya ada 1 file `.tft`
- Slot MicroSD ada di **belakang** PCB layar Nextion (balik layar, lihat slot kecil)

---

## 📋 LANGKAH 9: Hubungkan ke STM32 & Test

### Wiring:
```
Nextion/TJC          STM32F103C8T6
─────────────        ─────────────
TX  ──────────────── RX (PA10 / USART1_RX)
RX  ──────────────── TX (PA9  / USART1_TX)
GND ──────────────── GND
5V  ──────────────── 5V (dari power supply, BUKAN dari 3.3V STM32!)
```

> **⚠️ Layar Nextion butuh 5V dengan arus minimal 500mA.** Jangan power dari pin 3.3V STM32 — akan kurang daya dan layar bisa blank/reboot terus.

### Test:
1. Flash firmware `SOC_PROTO.elf` ke STM32 via ST-Link
2. Nyalakan power → Layar langsung menampilkan **HOME MONITOR**
3. Sentuh tombol **MENU** → Pindah ke halaman Main Menu
4. Sentuh item menu → Pindah ke halaman terkait
5. Data tegangan, arus, SOC akan ter-update live!

---

## 🎨 Referensi Warna (RGB565 Format)

| Warna | Nilai | Digunakan untuk |
|-------|-------|-----------------|
| Hitam | 0 | Background |
| Putih | 65535 | Teks utama |
| Biru | 31 | Label, border, aksen |
| Hijau | 2016 | Nilai OK, charging, SOC bar |
| Merah | 63488 | Warning, error |
| Abu Gelap | 16904 | Background tombol |
| Abu Terang | 33808 | Teks muted |

---

## 🔧 Troubleshooting

| Masalah | Solusi |
|---------|--------|
| Layar blank / putih | Cek wiring 5V dan GND. Pastikan power supply cukup arus (500mA+) |
| Muncul demo pabrik lagi | Upload .tft belum berhasil. Ulangi proses MicroSD |
| Tombol tidak merespons | Cek apakah "Send Component ID" sudah dicentang di setiap tombol |
| Data tidak ter-update | Cek objname apakah sudah sesuai tabel (case-sensitive!) |
| "Update Failed" di layar | Format ulang MicroSD ke FAT32. Pastikan hanya ada 1 file .tft |
| Layar reboot terus-menerus | Power supply kurang arus. Ganti ke supply 5V 1A |
| Font error saat compile | Generate font dulu (Tools → Font Generator) sebelum compile |

---

## 📁 File Pendukung

File-file berikut sudah di-generate di folder `nextion_project/`:
- `HMI_SPECIFICATION.txt` — Spesifikasi lengkap semua komponen
- `Program_s.txt` — Kode Program.s siap copy-paste
- `hmi_project.json` — Data JSON untuk referensi
- `generate_hmi.py` — Script Python generator

---

**Setelah upload .tft berhasil, layar Anda akan langsung menampilkan GUI BIMA SOC Analyzer dengan tombol-tombol yang berfungsi! 🎉**
