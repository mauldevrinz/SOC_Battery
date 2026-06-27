# STM32 Voltage & Current Monitor (STM32_SOCPROJECT)

This project is a prototype monitoring system based on the STM32 microcontroller (STM32F103). It reads voltage and current analog values from sensors, displays them on an SH1107 OLED screen, and logs the data into an SD Card in CSV format.

## Project Structure

This project is generated using STM32CubeMX and follows the standard STM32 HAL project hierarchy:

* **`Core/`**: Contains the main application code.
  * `Src/main.c`: The main program body containing initialization and the main loop.
  * `Inc/`: Header files for the application logic.
* **`Drivers/`**: Contains the STM32F1xx HAL (Hardware Abstraction Layer) drivers and CMSIS files provided by STMicroelectronics.
* **`Middlewares/`**: Contains third-party libraries. In this project, it includes the **FatFs** generic FAT file system module used to interface with the SD Card.
* **`FATFS/`**: Contains the FATFS target user code (`user_diskio.c`) linking the FatFs library to the physical SPI SD card driver.
* **`SOC_PROTO.ioc`**: The STM32CubeMX configuration file. This file can be opened in STM32CubeMX or STM32CubeIDE to reconfigure pins, clocks, or peripherals.

## `main.c` Explanation

The core logic of the application resides in `Core/Src/main.c`. Here is a breakdown of how the program works:

### 1. Hardware Initialization
At the start of the `main()` function, the system initializes all required peripherals:
- **ADC1 and ADC2**: Configured to read analog signals from the voltage and current sensors.
- **SPI1 and SPI2**: SPI interfaces used for external communication. SPI1 is used for the SH1107 OLED, and SPI2 is used for the SD Card (with DMA enabled for efficient data transfer).
- **FATFS**: Initializes the file system to interact with the SD card.
- **OLED SH1107**: The display is initialized and a "Voltage Monitor" splash screen is rendered.
- **SD Card Setup**: The system attempts to mount the SD card and open a file named `LOG.CSV`. If successful, the file remains open for continuous logging.

### 2. Sensor Polling (ADC)
Inside the main infinite loop (`while (1)`), the program continuously polls the ADCs:
- **ADC1** is read to get the raw voltage sensor value.
- **ADC2** is read to get the raw current sensor value.
- The raw 12-bit ADC values (0-4095) are converted to actual voltage measurements (assuming a 3.3V reference) using the formula:
  `value = (ADC_Read * 3.3) / 4095.0`

### 3. OLED Display Update
To prevent the display drawing process from blocking or slowing down the ADC polling, the OLED is not updated on every single loop iteration.
- The screen is refreshed once every 20 loop ticks (controlled by the `DISPLAY_REFRESH_DIV` macro).
- A custom `FloatToStr` function is used to convert floating-point numbers into strings (avoiding the overhead of standard C-library `printf` float support).
- The `UpdateDisplay()` function redraws the voltage, current, and the total number of logged samples on the screen.

### 4. SD Card Data Logging
The sensor readings are logged to the SD Card using the `SD_Logger_AddSample` function.
- To optimize SD Card lifespan and write speeds, the data is **batched** into RAM before writing.
- The system accumulates 20 lines of CSV data (controlled by `LOG_BUFFER_SAMPLES`).
- Once the buffer is full, `SD_Logger_Flush()` writes the entire chunk to the SD Card using a single `f_write` operation.
- The FAT metadata is forcefully synchronized (`f_sync`) every 10 batch writes (controlled by `LOG_SYNC_EVERY_FLUSH`) to ensure data is safely stored without closing the file.

### Main Loop Timing
The end of each `while(1)` iteration has a `HAL_Delay(10);`, meaning the main loop runs roughly every 10 milliseconds.
