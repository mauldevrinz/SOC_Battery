# STM32_SOCPROJECT

Proyek ini adalah prototipe sistem pemantauan (monitoring) berbasis mikrokontroler STM32 (STM32F103). Proyek ini membaca nilai tegangan (voltage) dan arus (current) dari sensor analog, menampilkannya ke layar OLED, serta menyimpan data log ke dalam SD Card.

## Penjelasan `main.c`

File utama dari program ini adalah `Core/Src/main.c`. Berikut adalah alur dan fitur utama dari program:

### 1. Inisialisasi Periferal (Hardware)
Pada awal program (`main()` function), sistem akan melakukan inisialisasi:
- **ADC1 dan ADC2**: Digunakan untuk membaca nilai analog dari sensor (tegangan dan arus).
- **SPI1 dan SPI2**: Digunakan untuk komunikasi dengan perangkat luar. SPI1 kemungkinan besar digunakan untuk OLED SH1107, dan SPI2 digunakan untuk SD Card (ditandai dengan DMA yang diaktifkan untuk SPI2).
- **FATFS**: Sistem file FAT diinisialisasi untuk mengakses SD Card.
- **OLED SH1107**: Layar OLED diinisialisasi dan menampilkan *splash screen* "Voltage Monitor".
- **SD Card**: Sistem akan mencoba *mount* SD Card. Jika berhasil, file `LOG.CSV` akan dibuat/dibuka.

### 2. Membaca Sensor (ADC)
Di dalam *infinite loop* (`while (1)`), program melakukan *polling* ADC secara terus menerus:
- **ADC1** dibaca untuk mendapatkan nilai mentah sensor pertama (Tegangan / Voltage).
- **ADC2** dibaca untuk mendapatkan nilai mentah sensor kedua (Arus / Current).
- Nilai ADC mentah (0-4095) dikonversi ke nilai tegangan nyata (asumsi referensi 3.3V) menggunakan rumus:
  `nilai = (ADC_Read * 3.3) / 4095.0`

### 3. Menampilkan ke OLED
Program tidak memperbarui layar OLED setiap kali *loop* berjalan karena akan memperlambat proses pembacaan ADC. Sebaliknya, layar diperbarui setiap 20 iterasi (diatur oleh `DISPLAY_REFRESH_DIV`).
- Fungsi `UpdateDisplay(voltage, current)` akan mengubah nilai *float* menjadi *string* (menggunakan fungsi kustom `FloatToStr` tanpa standard C-library float-printf) lalu menampilkannya.
- Layar OLED juga akan menampilkan jumlah sampel data yang berhasil disimpan ke SD Card.

### 4. Menyimpan Data (Data Logging) ke SD Card
Data yang dibaca akan disimpan ke SD Card menggunakan fungsi `SD_Logger_AddSample`.
- Untuk mempercepat kinerja dan menghindari keausan modul SD Card, penulisan ke SD Card (via FATFS `f_write`) dilakukan secara *batch*.
- Sistem akan mengumpulkan 20 baris data ke dalam sebuah RAM *buffer* (diatur oleh `LOG_BUFFER_SAMPLES`).
- Setelah terkumpul, fungsi `SD_Logger_Flush` dipanggil untuk menulis ke SD Card sekaligus. Operasi sinkronisasi file (FAT/metadata update dengan `f_sync`) hanya dilakukan setiap 10 kali penulisan *batch* (diatur oleh `LOG_SYNC_EVERY_FLUSH`).

### Kesimpulan Loop
Setiap putaran iterasi pada `while(1)` akan diakhiri dengan `HAL_Delay(10);`, sehingga setiap perulangan (*tick*) memakan waktu sekitar 10 milidetik.
