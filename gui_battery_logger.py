"""
=============================================================================
 BIMA SOC LFP ANALYZER - MULTIPAGE ACQUISITION DASHBOARD
 Sesuai Brief Penelitian Fundamental BIMA 2026 (LCD_BRIEF.pptx)
=============================================================================
 Struktur Menu / Tab:
 1. 🏠 Home Screen        : Live Gauge (V, I, P, SOC, SOH, Bat), status SD Card
 2. ⚡ Quick Check       : Voltage Check (Now, Min, Max, Hold), Current Check (Dir, Peak, Offset), Quick SOC
 3. 🔬 Battery Analyze   : Mode (Normal/Accurate), SOC Comparison (OCV vs AEKF), SOH & R0, Energy Wh
 4. ⚙️ Battery Setup     : Nominal Voltage (12V, 24V, 36V), Nominal Capacity (12Ah, 20Ah, 30Ah, 50Ah, 100Ah)
 5. 💾 Data Logger       : Start/Stop PC Log, Sampling Rate, Logged Parameters Summary, SD Samples
 6. 📜 History           : Session Measurement Log & Historical Trend Analyzer
=============================================================================
"""

import tkinter as tk
from tkinter import ttk, messagebox, filedialog
import threading
import time
import csv
import re
import math
from datetime import datetime

try:
    import serial
    import serial.tools.list_ports
    SERIAL_AVAILABLE = True
except ImportError:
    SERIAL_AVAILABLE = False

try:
    from pyocd.core.helpers import ConnectHelper
    PYOCD_AVAILABLE = True
except ImportError:
    PYOCD_AVAILABLE = False


# Addresses from Debug/SOC_PROTO.map. Refresh these after every firmware link.
RAM_VARIABLES = {
    "soh_pct": (0x20000000, "float"),
    "sys_bat_pct": (0x20000004, "uint8"),
    "sdStatusStr": (0x2000000C, "char[10]"),
    "voltage": (0x20000248, "float"),
    "current": (0x2000024C, "float"),
    "soc_pct": (0x20000250, "float"),
    "v_internal_bat": (0x20000254, "float"),
    "sdReady": (0x20000494, "uint8"),
    "logTotalSamples": (0x200004F0, "uint32"),
}


class BimaBatteryDashboard(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("BIMA SOC LFP - Battery Analyzer Dashboard (Penelitian BIMA 2026)")
        self.geometry("1100x720")
        self.minsize(950, 640)
        self.configure(bg="#0B132B")

        # Serial Connection
        self.ser = None
        self.is_connected = False
        self.serial_thread = None
        self.stop_thread = False
        self.source_var = tk.StringVar(value="Serial")
        self.pyocd_session = None
        self.connection_generation = 0
        self.data_queue = __import__("queue").Queue()
        self.after(50, self.process_data_queue)

        # Live Real-time Data
        self.voltage = 0.0
        self.current = 0.0
        self.power = 0.0
        self.soc = 0.0
        self.soh = 100.0
        self.sys_bat = 100
        self.sd_status = "UNKNOWN"
        self.run_status = "IDLE"
        self.sd_logged_samples = 0

        # Quick Check Data
        self.v_min = 999.0
        self.v_max = -999.0
        self.i_peak = 0.0
        self.zero_offset = 1.690
        self.hold_reading = False
        self.held_v = 0.0
        self.held_i = 0.0

        # Battery Analyze Data
        self.analysis_mode = "Normal Mode"
        self.soc_ocv = 0.0
        self.v_internal_bat = 0.0
        self.soc_aekf = 0.0
        self.soc_error = 0.0
        self.r0_estimate = 0.010  # Ohm
        self.dynamic_r = 0.012    # Ohm
        self.energy_wh = 0.0
        self.accumulated_energy = 0.0
        self.last_energy_tick = None

        # Battery Setup Profile (Slide 7)
        self.setup_nominal_v = tk.StringVar(value="12.8 V (4S LFP)")
        self.setup_capacity = tk.StringVar(value="100 Ah")
        self.setup_chemistry = tk.StringVar(value="LiFePO4 (LFP)")

        # PC Data Logging
        self.pc_logging = False
        self.csv_file = None
        self.csv_writer = None
        self.log_start_time = None
        self.total_pc_logged = 0

        self.setup_custom_styles()
        self.setup_main_ui()
        self.refresh_ports()

    def setup_custom_styles(self):
        style = ttk.Style()
        style.theme_use("clam")

        # Notebook tabs
        style.configure("TNotebook", background="#0B132B", borderwidth=0)
        style.configure("TNotebook.Tab", background="#1C2541", foreground="#CBD5E0",
                        font=("Segoe UI", 10, "bold"), padding=[16, 8])
        style.map("TNotebook.Tab",
                  background=[("selected", "#3A506B")],
                  foreground=[("selected", "#6FFFE9")])

        # Treeview table
        style.configure("Treeview", background="#1C2541", foreground="#F0F0F0",
                        fieldbackground="#1C2541", rowheight=24, font=("Segoe UI", 9))
        style.configure("Treeview.Heading", background="#3A506B", foreground="#6FFFE9",
                        font=("Segoe UI", 9, "bold"))
        style.map("Treeview", background=[("selected", "#48CAE4")], foreground=[("selected", "#0B132B")])

    def setup_main_ui(self):
        # ── TOP BAR (Koneksi & Status) ──────────────────────────────
        top_bar = tk.Frame(self, bg="#1C2541", height=54)
        top_bar.pack(side=tk.TOP, fill=tk.X, padx=12, pady=(10, 6))

        title_frame = tk.Frame(top_bar, bg="#1C2541")
        title_frame.pack(side=tk.LEFT, padx=14, pady=8)

        tk.Label(title_frame, text="⚡ BIMA SOC LFP", font=("Segoe UI", 14, "bold"),
                 fg="#6FFFE9", bg="#1C2541").pack(side=tk.LEFT)
        tk.Label(title_frame, text=" | Portable Battery Analyzer", font=("Segoe UI", 10),
                 fg="#A0AEC0", bg="#1C2541").pack(side=tk.LEFT, padx=(5, 0))

        # Right control: Port selection & Connect
        conn_frame = tk.Frame(top_bar, bg="#1C2541")
        conn_frame.pack(side=tk.RIGHT, padx=12, pady=6)

        tk.Label(conn_frame, text="Source:", font=("Segoe UI", 9), fg="#CBD5E0", bg="#1C2541").pack(side=tk.LEFT, padx=2)
        self.source_combobox = ttk.Combobox(conn_frame, width=9, state="readonly",
                                            textvariable=self.source_var, values=("Serial", "ST-Link SWD"))
        self.source_combobox.pack(side=tk.LEFT, padx=4)
        self.source_combobox.bind("<<ComboboxSelected>>", self.on_source_changed)

        tk.Label(conn_frame, text="Port:", font=("Segoe UI", 9), fg="#CBD5E0", bg="#1C2541").pack(side=tk.LEFT, padx=2)
        self.port_combobox = ttk.Combobox(conn_frame, width=12, state="readonly")
        self.port_combobox.pack(side=tk.LEFT, padx=4)

        self.btn_refresh = tk.Button(conn_frame, text="🔄", font=("Segoe UI", 9),
                                     command=self.refresh_ports, bg="#3A506B", fg="white", relief=tk.FLAT, bd=0)
        self.btn_refresh.pack(side=tk.LEFT, padx=2)

        self.btn_connect = tk.Button(conn_frame, text="Connect", font=("Segoe UI", 10, "bold"),
                                     command=self.toggle_connection, bg="#48CAE4", fg="#0B132B",
                                     padx=14, relief=tk.FLAT, bd=0, cursor="hand2")
        self.btn_connect.pack(side=tk.LEFT, padx=6)

        # ── STATUS RIBBON ───────────────────────────────────────────
        status_ribbon = tk.Frame(self, bg="#111B38", height=32)
        status_ribbon.pack(side=tk.TOP, fill=tk.X, padx=12, pady=(0, 6))

        self.lbl_ribbon_status = tk.Label(status_ribbon, text="● SYSTEM: DISCONNECTED", font=("Segoe UI", 9, "bold"),
                                          fg="#FF4D4D", bg="#111B38")
        self.lbl_ribbon_status.pack(side=tk.LEFT, padx=14, pady=4)

        self.lbl_ribbon_bat = tk.Label(status_ribbon, text="🔋 Baterai Alat: --%", font=("Segoe UI", 9, "bold"),
                                       fg="#48CAE4", bg="#111B38")
        self.lbl_ribbon_bat.pack(side=tk.LEFT, padx=20, pady=4)

        self.lbl_ribbon_mode = tk.Label(status_ribbon, text="Mode: Normal (LiFePO4 4S 100Ah)", font=("Segoe UI", 9),
                                        fg="#A0AEC0", bg="#111B38")
        self.lbl_ribbon_mode.pack(side=tk.LEFT, padx=20, pady=4)

        self.lbl_ribbon_sd = tk.Label(status_ribbon, text="💾 SD Card: --", font=("Segoe UI", 9, "bold"),
                                      fg="#FFD166", bg="#111B38")
        self.lbl_ribbon_sd.pack(side=tk.RIGHT, padx=14, pady=4)

        # ── NOTEBOOK TABS (MULTIDISPLAY DARI PPT BRIEF) ─────────────
        self.notebook = ttk.Notebook(self)
        self.notebook.pack(side=tk.TOP, fill=tk.BOTH, expand=True, padx=12, pady=4)

        # Create 6 Tabs
        self.tab_home = tk.Frame(self.notebook, bg="#0B132B")
        self.tab_quick = tk.Frame(self.notebook, bg="#0B132B")
        self.tab_analyze = tk.Frame(self.notebook, bg="#0B132B")
        self.tab_setup = tk.Frame(self.notebook, bg="#0B132B")
        self.tab_logger = tk.Frame(self.notebook, bg="#0B132B")
        self.tab_history = tk.Frame(self.notebook, bg="#0B132B")

        self.notebook.add(self.tab_home, text="  🏠 Home Screen  ")
        self.notebook.add(self.tab_quick, text="  ⚡ Quick Check  ")
        self.notebook.add(self.tab_analyze, text="  🔬 Battery Analyze  ")
        self.notebook.add(self.tab_setup, text="  ⚙️ Battery Setup  ")
        self.notebook.add(self.tab_logger, text="  💾 Data Logger  ")
        self.notebook.add(self.tab_history, text="  📜 History & Trends  ")

        # Build each tab UI
        self.build_tab_home()
        self.build_tab_quick()
        self.build_tab_analyze()
        self.build_tab_setup()
        self.build_tab_logger()
        self.build_tab_history()

    # ═════════════════════════════════════════════════════════════════
    # TAB 1: HOME SCREEN
    # ═════════════════════════════════════════════════════════════════
    def build_tab_home(self):
        parent = self.tab_home

        # Top Big Cards (Voltage, Current, Power, SOC)
        cards_frame = tk.Frame(parent, bg="#0B132B")
        cards_frame.pack(side=tk.TOP, fill=tk.X, pady=(10, 5))
        for i in range(4):
            cards_frame.grid_columnconfigure(i, weight=1)

        self.lbl_home_v = self.create_metric_card(cards_frame, 0, 0, "VOLTAGE (LFP)", "0.000", "V", "#00B4D8")
        self.lbl_home_i = self.create_metric_card(cards_frame, 0, 1, "CURRENT", "0.000", "A", "#06D6A0")
        self.lbl_home_p = self.create_metric_card(cards_frame, 0, 2, "POWER", "0.00", "W", "#FFD166")
        self.lbl_home_soc = self.create_metric_card(cards_frame, 0, 3, "STATE OF CHARGE", "0.0", "%", "#6FFFE9")

        # SOC Progress Gauge Bar
        gauge_box = tk.Frame(parent, bg="#1C2541", height=45)
        gauge_box.pack(side=tk.TOP, fill=tk.X, pady=8, padx=5)

        tk.Label(gauge_box, text="SOC LEVEL:", font=("Segoe UI", 9, "bold"), fg="#A0AEC0", bg="#1C2541").pack(side=tk.LEFT, padx=12, pady=10)
        self.home_soc_bar = ttk.Progressbar(gauge_box, orient=tk.HORIZONTAL, length=450, mode="determinate")
        self.home_soc_bar.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=10, pady=10)
        self.lbl_home_soc_num = tk.Label(gauge_box, text="0.0%", font=("Segoe UI", 11, "bold"), fg="#6FFFE9", bg="#1C2541")
        self.lbl_home_soc_num.pack(side=tk.RIGHT, padx=15, pady=10)

        # Bottom Row: SOH card + System Status
        bottom_frame = tk.Frame(parent, bg="#0B132B")
        bottom_frame.pack(side=tk.TOP, fill=tk.BOTH, expand=True, pady=5)
        bottom_frame.grid_columnconfigure(0, weight=1)
        bottom_frame.grid_columnconfigure(1, weight=1)

        # SOH Card
        soh_box = tk.LabelFrame(bottom_frame, text=" State of Health (SOH) ", font=("Segoe UI", 10, "bold"),
                                fg="#6FFFE9", bg="#1C2541", padx=15, pady=10)
        soh_box.grid(row=0, column=0, sticky="nsew", padx=(5, 5), pady=5)

        self.lbl_home_soh = tk.Label(soh_box, text="100.0%", font=("Segoe UI", 32, "bold"), fg="#06D6A0", bg="#1C2541")
        self.lbl_home_soh.pack(anchor="w")
        self.lbl_home_soh_desc = tk.Label(soh_box, text="Condition: GOOD (Zero Loss)", font=("Segoe UI", 10),
                                          fg="#A0AEC0", bg="#1C2541")
        self.lbl_home_soh_desc.pack(anchor="w", pady=(5, 0))

        # Instrument Status Card
        diag_box = tk.LabelFrame(bottom_frame, text=" Instrument Diagnostics ", font=("Segoe UI", 10, "bold"),
                                 fg="#6FFFE9", bg="#1C2541", padx=15, pady=10)
        diag_box.grid(row=0, column=1, sticky="nsew", padx=(5, 5), pady=5)

        self.lbl_diag_sd = tk.Label(diag_box, text="• SD Card Storage: UNKNOWN (LOG.CSV)", font=("Segoe UI", 10), fg="#E0E0E0", bg="#1C2541")
        self.lbl_diag_sd.pack(anchor="w", pady=2)
        self.lbl_diag_samp = tk.Label(diag_box, text="• Logged Packets: 0 samples", font=("Segoe UI", 10), fg="#E0E0E0", bg="#1C2541")
        self.lbl_diag_samp.pack(anchor="w", pady=2)
        self.lbl_diag_baud = tk.Label(diag_box, text="• Baudrate: 9600 bps (USART1 Full-Duplex)", font=("Segoe UI", 10), fg="#E0E0E0", bg="#1C2541")
        self.lbl_diag_baud.pack(anchor="w", pady=2)

    # ═════════════════════════════════════════════════════════════════
    # TAB 2: QUICK CHECK (Slide 5 Brief)
    # ═════════════════════════════════════════════════════════════════
    def build_tab_quick(self):
        parent = self.tab_quick

        # 3 Column Sections
        grid_frame = tk.Frame(parent, bg="#0B132B")
        grid_frame.pack(fill=tk.BOTH, expand=True, padx=5, pady=10)
        for c in range(3):
            grid_frame.grid_columnconfigure(c, weight=1)

        # Column 1: Voltage Check
        box_v = tk.LabelFrame(grid_frame, text=" Voltage Check ", font=("Segoe UI", 11, "bold"),
                              fg="#00B4D8", bg="#1C2541", padx=15, pady=12)
        box_v.grid(row=0, column=0, sticky="nsew", padx=5)

        tk.Label(box_v, text="Voltage Now:", font=("Segoe UI", 10), fg="#A0AEC0", bg="#1C2541").pack(anchor="w")
        self.lbl_qc_vnow = tk.Label(box_v, text="0.000 V", font=("Segoe UI", 24, "bold"), fg="#00B4D8", bg="#1C2541")
        self.lbl_qc_vnow.pack(anchor="w", pady=(0, 10))

        tk.Label(box_v, text="Min Voltage recorded:", font=("Segoe UI", 9), fg="#A0AEC0", bg="#1C2541").pack(anchor="w")
        self.lbl_qc_vmin = tk.Label(box_v, text="-- V", font=("Segoe UI", 12, "bold"), fg="white", bg="#1C2541")
        self.lbl_qc_vmin.pack(anchor="w", pady=(0, 6))

        tk.Label(box_v, text="Max Voltage recorded:", font=("Segoe UI", 9), fg="#A0AEC0", bg="#1C2541").pack(anchor="w")
        self.lbl_qc_vmax = tk.Label(box_v, text="-- V", font=("Segoe UI", 12, "bold"), fg="white", bg="#1C2541")
        self.lbl_qc_vmax.pack(anchor="w", pady=(0, 15))

        self.btn_hold = tk.Button(box_v, text="❄️ Hold Reading: OFF", font=("Segoe UI", 9, "bold"),
                                  command=self.toggle_hold, bg="#3A506B", fg="white", relief=tk.FLAT, pady=4)
        self.btn_hold.pack(fill=tk.X)

        # Column 2: Current Check
        box_i = tk.LabelFrame(grid_frame, text=" Current Check ", font=("Segoe UI", 11, "bold"),
                              fg="#06D6A0", bg="#1C2541", padx=15, pady=12)
        box_i.grid(row=0, column=1, sticky="nsew", padx=5)

        tk.Label(box_i, text="Current Now:", font=("Segoe UI", 10), fg="#A0AEC0", bg="#1C2541").pack(anchor="w")
        self.lbl_qc_inow = tk.Label(box_i, text="0.000 A", font=("Segoe UI", 24, "bold"), fg="#06D6A0", bg="#1C2541")
        self.lbl_qc_inow.pack(anchor="w", pady=(0, 10))

        tk.Label(box_i, text="Direction:", font=("Segoe UI", 9), fg="#A0AEC0", bg="#1C2541").pack(anchor="w")
        self.lbl_qc_idir = tk.Label(box_i, text="STANDBY / IDLE", font=("Segoe UI", 11, "bold"), fg="#FFD166", bg="#1C2541")
        self.lbl_qc_idir.pack(anchor="w", pady=(0, 6))

        tk.Label(box_i, text="Peak Current:", font=("Segoe UI", 9), fg="#A0AEC0", bg="#1C2541").pack(anchor="w")
        self.lbl_qc_ipeak = tk.Label(box_i, text="0.000 A", font=("Segoe UI", 12, "bold"), fg="white", bg="#1C2541")
        self.lbl_qc_ipeak.pack(anchor="w", pady=(0, 6))

        tk.Label(box_i, text="Zero Offset Calibrated:", font=("Segoe UI", 9), fg="#A0AEC0", bg="#1C2541").pack(anchor="w")
        self.lbl_qc_offset = tk.Label(box_i, text="1.690 V", font=("Segoe UI", 11), fg="#CBD5E0", bg="#1C2541")
        self.lbl_qc_offset.pack(anchor="w")

        # Column 3: Quick SOC Estimate
        box_soc = tk.LabelFrame(grid_frame, text=" Quick SOC Estimate ", font=("Segoe UI", 11, "bold"),
                                fg="#6FFFE9", bg="#1C2541", padx=15, pady=12)
        box_soc.grid(row=0, column=2, sticky="nsew", padx=5)

        tk.Label(box_soc, text="SOC by OCV Curve:", font=("Segoe UI", 10), fg="#A0AEC0", bg="#1C2541").pack(anchor="w")
        self.lbl_qc_soc_ocv = tk.Label(box_soc, text="-- %", font=("Segoe UI", 24, "bold"), fg="#6FFFE9", bg="#1C2541")
        self.lbl_qc_soc_ocv.pack(anchor="w", pady=(0, 10))

        tk.Label(box_soc, text="SOC Confidence Level:", font=("Segoe UI", 9), fg="#A0AEC0", bg="#1C2541").pack(anchor="w")
        self.lbl_qc_conf = tk.Label(box_soc, text="HIGH (EKF Converged)", font=("Segoe UI", 11, "bold"), fg="#06D6A0", bg="#1C2541")
        self.lbl_qc_conf.pack(anchor="w", pady=(0, 15))

        btn_reset_stats = tk.Button(box_soc, text="🔄 Reset Min/Max Records", font=("Segoe UI", 9),
                                    command=self.reset_stats, bg="#3A506B", fg="white", relief=tk.FLAT, pady=4)
        btn_reset_stats.pack(fill=tk.X)

    # ═════════════════════════════════════════════════════════════════
    # TAB 3: BATTERY ANALYZE (Slide 6 Brief)
    # ═════════════════════════════════════════════════════════════════
    def build_tab_analyze(self):
        parent = self.tab_analyze

        grid_frame = tk.Frame(parent, bg="#0B132B")
        grid_frame.pack(fill=tk.BOTH, expand=True, padx=5, pady=10)
        grid_frame.grid_columnconfigure(0, weight=1)
        grid_frame.grid_columnconfigure(1, weight=1)

        # Left Panel: SOC Estimation Analysis
        box_soc = tk.LabelFrame(grid_frame, text=" SOC Estimation Breakdown ", font=("Segoe UI", 11, "bold"),
                                fg="#6FFFE9", bg="#1C2541", padx=15, pady=12)
        box_soc.grid(row=0, column=0, sticky="nsew", padx=5, pady=5)

        tk.Label(box_soc, text="• OCV-based SOC (Static Curve):", font=("Segoe UI", 10), fg="#CBD5E0", bg="#1C2541").pack(anchor="w", pady=2)
        self.lbl_an_ocv_soc = tk.Label(box_soc, text="-- %", font=("Segoe UI", 14, "bold"), fg="#FFD166", bg="#1C2541")
        self.lbl_an_ocv_soc.pack(anchor="w", padx=15)

        tk.Label(box_soc, text="• AEKF-based SOC (Adaptive Kalman):", font=("Segoe UI", 10), fg="#CBD5E0", bg="#1C2541").pack(anchor="w", pady=2)
        self.lbl_an_aekf_soc = tk.Label(box_soc, text="-- %", font=("Segoe UI", 14, "bold"), fg="#6FFFE9", bg="#1C2541")
        self.lbl_an_aekf_soc.pack(anchor="w", padx=15)

        tk.Label(box_soc, text="• SOC Error Indicator (ΔSOC):", font=("Segoe UI", 10), fg="#CBD5E0", bg="#1C2541").pack(anchor="w", pady=2)
        self.lbl_an_err = tk.Label(box_soc, text="0.00 %", font=("Segoe UI", 12, "bold"), fg="#06D6A0", bg="#1C2541")
        self.lbl_an_err.pack(anchor="w", padx=15)

        tk.Label(box_soc, text="• Analysis Mode:", font=("Segoe UI", 10), fg="#CBD5E0", bg="#1C2541").pack(anchor="w", pady=(10, 2))
        mode_btn_frame = tk.Frame(box_soc, bg="#1C2541")
        mode_btn_frame.pack(anchor="w", padx=15)
        self.btn_normal_mode = tk.Button(mode_btn_frame, text="Normal Mode", bg="#06D6A0", fg="#0B132B",
                                         font=("Segoe UI", 8, "bold"), relief=tk.FLAT, command=lambda: self.set_mode("Normal Mode"))
        self.btn_normal_mode.pack(side=tk.LEFT, padx=2)
        self.btn_acc_mode = tk.Button(mode_btn_frame, text="Accurate Mode (AEKF)", bg="#3A506B", fg="white",
                                      font=("Segoe UI", 8), relief=tk.FLAT, command=lambda: self.set_mode("Accurate Mode (AEKF)"))
        self.btn_acc_mode.pack(side=tk.LEFT, padx=4)

        # Right Panel: SOH & Internal Resistance
        box_soh = tk.LabelFrame(grid_frame, text=" SOH & Resistance Estimation ", font=("Segoe UI", 11, "bold"),
                                fg="#06D6A0", bg="#1C2541", padx=15, pady=12)
        box_soh.grid(row=0, column=1, sticky="nsew", padx=5, pady=5)

        tk.Label(box_soh, text="• Internal Resistance (R0 Estimate):", font=("Segoe UI", 10), fg="#CBD5E0", bg="#1C2541").pack(anchor="w", pady=2)
        self.lbl_an_r0 = tk.Label(box_soh, text="10.0 mΩ", font=("Segoe UI", 14, "bold"), fg="#00B4D8", bg="#1C2541")
        self.lbl_an_r0.pack(anchor="w", padx=15)

        tk.Label(box_soh, text="• Dynamic Resistance (R_dyn):", font=("Segoe UI", 10), fg="#CBD5E0", bg="#1C2541").pack(anchor="w", pady=2)
        self.lbl_an_rdyn = tk.Label(box_soh, text="12.4 mΩ", font=("Segoe UI", 14, "bold"), fg="#CBD5E0", bg="#1C2541")
        self.lbl_an_rdyn.pack(anchor="w", padx=15)

        tk.Label(box_soh, text="• Degradation Status:", font=("Segoe UI", 10), fg="#CBD5E0", bg="#1C2541").pack(anchor="w", pady=2)
        self.lbl_an_deg = tk.Label(box_soh, text="Optimal (< 5% Loss)", font=("Segoe UI", 12, "bold"), fg="#06D6A0", bg="#1C2541")
        self.lbl_an_deg.pack(anchor="w", padx=15)

        tk.Label(box_soh, text="• Remaining Energy (Cumulative Wh):", font=("Segoe UI", 10), fg="#CBD5E0", bg="#1C2541").pack(anchor="w", pady=(10, 2))
        self.lbl_an_wh = tk.Label(box_soh, text="0.00 Wh", font=("Segoe UI", 16, "bold"), fg="#FFD166", bg="#1C2541")
        self.lbl_an_wh.pack(anchor="w", padx=15)

    # ═════════════════════════════════════════════════════════════════
    # TAB 4: BATTERY SETUP (Slide 7 Brief)
    # ═════════════════════════════════════════════════════════════════
    def build_tab_setup(self):
        parent = self.tab_setup

        box = tk.LabelFrame(parent, text=" Battery Pack Configuration & Calibration ", font=("Segoe UI", 11, "bold"),
                            fg="#6FFFE9", bg="#1C2541", padx=25, pady=20)
        box.pack(fill=tk.BOTH, expand=True, padx=20, pady=15)

        # 1. Nominal Voltage Setup
        tk.Label(box, text="Nominal Pack Voltage (V):", font=("Segoe UI", 10, "bold"), fg="#CBD5E0", bg="#1C2541").grid(row=0, column=0, sticky="w", pady=10)
        v_frame = tk.Frame(box, bg="#1C2541")
        v_frame.grid(row=0, column=1, sticky="w", padx=20, pady=10)
        for v in ["12.8 V (4S LFP)", "24 V", "36 V", "48 V"]:
            tk.Radiobutton(v_frame, text=v, value=v, variable=self.setup_nominal_v, command=self.apply_battery_profile,
                           font=("Segoe UI", 10), fg="white", bg="#1C2541", selectcolor="#3A506B", activebackground="#1C2541").pack(side=tk.LEFT, padx=10)

        # 2. Capacity Setup
        tk.Label(box, text="Nominal Capacity (Ah):", font=("Segoe UI", 10, "bold"), fg="#CBD5E0", bg="#1C2541").grid(row=1, column=0, sticky="w", pady=10)
        c_frame = tk.Frame(box, bg="#1C2541")
        c_frame.grid(row=1, column=1, sticky="w", padx=20, pady=10)
        for c in ["12 Ah", "20 Ah", "50 Ah", "100 Ah"]:
            tk.Radiobutton(c_frame, text=c, value=c, variable=self.setup_capacity, command=self.apply_battery_profile,
                           font=("Segoe UI", 10), fg="white", bg="#1C2541", selectcolor="#3A506B", activebackground="#1C2541").pack(side=tk.LEFT, padx=8)

        # 3. Chemistry Setup
        tk.Label(box, text="Cell Chemistry:", font=("Segoe UI", 10, "bold"), fg="#CBD5E0", bg="#1C2541").grid(row=2, column=0, sticky="w", pady=10)
        chem_frame = tk.Frame(box, bg="#1C2541")
        chem_frame.grid(row=2, column=1, sticky="w", padx=20, pady=10)
        for ch in ["LiFePO4 (LFP)"]:
            tk.Radiobutton(chem_frame, text=ch, value=ch, variable=self.setup_chemistry, command=self.apply_battery_profile,
                           font=("Segoe UI", 10), fg="white", bg="#1C2541", selectcolor="#3A506B", activebackground="#1C2541").pack(side=tk.LEFT, padx=10)

        # Summary box
        summary_frame = tk.Frame(box, bg="#111B38", padx=15, pady=15)
        summary_frame.grid(row=3, column=0, columnspan=2, sticky="nsew", pady=(20, 0))

        tk.Label(summary_frame, text="Active Firmware Configuration Profile:", font=("Segoe UI", 10, "bold"), fg="#6FFFE9", bg="#111B38").pack(anchor="w")
        self.lbl_setup_summary = tk.Label(summary_frame,
                                          text="• Chemistry: LiFePO4 (LFP) | 4S Configuration | Nominal: 12.8 V | 100 Ah (360000 Coulombs)",
                                          font=("Segoe UI", 9), fg="#CBD5E0", bg="#111B38")
        self.lbl_setup_summary.pack(anchor="w", pady=(5, 0))

    # ═════════════════════════════════════════════════════════════════
    # TAB 5: DATA LOGGER (Slide 8 Brief)
    # ═════════════════════════════════════════════════════════════════
    def build_tab_logger(self):
        parent = self.tab_logger

        ctrl_frame = tk.Frame(parent, bg="#1C2541", padx=15, pady=12)
        ctrl_frame.pack(side=tk.TOP, fill=tk.X, padx=5, pady=5)

        self.btn_pc_log = tk.Button(ctrl_frame, text="▶ Start PC Logging (CSV)", font=("Segoe UI", 10, "bold"),
                                    command=self.toggle_pc_logging, bg="#06D6A0", fg="#0B132B", relief=tk.FLAT, padx=14, pady=4)
        self.btn_pc_log.pack(side=tk.LEFT, padx=5)

        self.lbl_logger_status = tk.Label(ctrl_frame, text="Logging Status: IDLE / STOPPED", font=("Segoe UI", 10, "bold"),
                                          fg="#A0AEC0", bg="#1C2541")
        self.lbl_logger_status.pack(side=tk.LEFT, padx=20)

        self.lbl_logger_count = tk.Label(ctrl_frame, text="Total Recorded: 0 records", font=("Segoe UI", 10),
                                         fg="#E0E0E0", bg="#1C2541")
        self.lbl_logger_count.pack(side=tk.RIGHT, padx=10)

        # Logged Parameters Info Box (Slide 8)
        param_frame = tk.Frame(parent, bg="#111B38", padx=12, pady=8)
        param_frame.pack(side=tk.TOP, fill=tk.X, padx=5, pady=(0, 5))

        tk.Label(param_frame, text="PC CSV records live measurements independently of SD: V, I, P, EKF/OCV SoC, SoH, net Wh, internal battery, SD status and sample count.",
                 font=("Segoe UI", 9, "bold"), fg="#FFD166", bg="#111B38").pack(anchor="w")

        # Table Treeview Live View
        tree_frame = tk.Frame(parent, bg="#1C2541")
        tree_frame.pack(side=tk.TOP, fill=tk.BOTH, expand=True, padx=5, pady=5)

        columns = ("Elapsed Time", "Voltage (V)", "Current (A)", "Power (W)", "SoC (%)", "SoH (%)", "Energy (Wh)")
        self.tree_logger = ttk.Treeview(tree_frame, columns=columns, show="headings", height=12)

        widths = [140, 120, 120, 120, 110, 110, 130]
        for c, w in zip(columns, widths):
            self.tree_logger.heading(c, text=c)
            self.tree_logger.column(c, width=w, anchor="center")

        sb = ttk.Scrollbar(tree_frame, orient=tk.VERTICAL, command=self.tree_logger.yview)
        self.tree_logger.configure(yscrollcommand=sb.set)
        self.tree_logger.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        sb.pack(side=tk.RIGHT, fill=tk.Y)

    # ═════════════════════════════════════════════════════════════════
    # TAB 6: HISTORY (Slide 9 Brief)
    # ═════════════════════════════════════════════════════════════════
    def build_tab_history(self):
        parent = self.tab_history

        info_box = tk.LabelFrame(parent, text=" Battery Profile History & Trend Analysis ", font=("Segoe UI", 11, "bold"),
                                 fg="#6FFFE9", bg="#1C2541", padx=15, pady=15)
        info_box.pack(side=tk.TOP, fill=tk.X, padx=5, pady=5)

        h_grid = tk.Frame(info_box, bg="#1C2541")
        h_grid.pack(fill=tk.BOTH, expand=True)

        tk.Label(h_grid, text="Latest Measurement Session:", font=("Segoe UI", 9, "bold"), fg="#6FFFE9", bg="#1C2541").grid(row=0, column=0, sticky="w", pady=4)
        self.lbl_hist_time = tk.Label(h_grid, text="--:--:--", font=("Segoe UI", 9), fg="white", bg="#1C2541")
        self.lbl_hist_time.grid(row=0, column=1, sticky="w", padx=10)

        tk.Label(h_grid, text="Capacity Loss:", font=("Segoe UI", 9, "bold"), fg="#6FFFE9", bg="#1C2541").grid(row=0, column=2, sticky="w", pady=4)
        self.lbl_hist_cap_loss = tk.Label(h_grid, text="0.00 Ah (0.0%)", font=("Segoe UI", 9), fg="#06D6A0", bg="#1C2541")
        self.lbl_hist_cap_loss.grid(row=0, column=3, sticky="w", padx=10)

        tk.Label(h_grid, text="Resistance Degradation Increase:", font=("Segoe UI", 9, "bold"), fg="#6FFFE9", bg="#1C2541").grid(row=1, column=0, sticky="w", pady=4)
        self.lbl_hist_res_inc = tk.Label(h_grid, text="+0.0 mΩ (Nominal)", font=("Segoe UI", 9), fg="#06D6A0", bg="#1C2541")
        self.lbl_hist_res_inc.grid(row=1, column=1, sticky="w", padx=10)

        tk.Label(h_grid, text="SOH Trend Status:", font=("Segoe UI", 9, "bold"), fg="#6FFFE9", bg="#1C2541").grid(row=1, column=2, sticky="w", pady=4)
        self.lbl_hist_soh_trend = tk.Label(h_grid, text="STABLE / EXCELLENT", font=("Segoe UI", 9), fg="#06D6A0", bg="#1C2541")
        self.lbl_hist_soh_trend.grid(row=1, column=3, sticky="w", padx=10)

        # Bottom: Session table
        tbl_frame = tk.Frame(parent, bg="#1C2541")
        tbl_frame.pack(side=tk.TOP, fill=tk.BOTH, expand=True, padx=5, pady=5)

        cols = ("Session Stamp", "Battery Type", "V_End (V)", "I_Avg (A)", "Final SOC (%)", "SOH (%)", "Status")
        self.tree_history = ttk.Treeview(tbl_frame, columns=cols, show="headings", height=10)
        for c in cols:
            self.tree_history.heading(c, text=c)
            self.tree_history.column(c, width=130, anchor="center")

        sb = ttk.Scrollbar(tbl_frame, orient=tk.VERTICAL, command=self.tree_history.yview)
        self.tree_history.configure(yscrollcommand=sb.set)
        self.tree_history.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        sb.pack(side=tk.RIGHT, fill=tk.Y)

    # ── UI HELPERS ──────────────────────────────────────────────
    def create_metric_card(self, parent, r, c, title, val, unit, color):
        frame = tk.Frame(parent, bg="#1C2541")
        frame.grid(row=r, column=c, padx=5, pady=5, sticky="nsew")

        tk.Label(frame, text=title, font=("Segoe UI", 9, "bold"), fg="#A0AEC0", bg="#1C2541").pack(anchor="w", padx=12, pady=(10, 2))
        
        val_box = tk.Frame(frame, bg="#1C2541")
        val_box.pack(anchor="w", padx=12, pady=(0, 10))

        lbl = tk.Label(val_box, text=val, font=("Segoe UI", 24, "bold"), fg=color, bg="#1C2541")
        lbl.pack(side=tk.LEFT)
        tk.Label(val_box, text=f" {unit}", font=("Segoe UI", 12), fg="#CBD5E0", bg="#1C2541").pack(side=tk.LEFT, pady=(8, 0))
        return lbl

    def refresh_ports(self):
        if hasattr(self, "source_var") and self.source_var.get() == "ST-Link SWD":
            if not PYOCD_AVAILABLE:
                messagebox.showerror("Error", "Library 'pyocd' belum terinstall. Jalankan: pip install pyocd")
                return
            self.port_combobox["values"] = ("Auto-detect ST-Link",)
            self.port_combobox.current(0)
            return
        if not SERIAL_AVAILABLE:
            messagebox.showerror("Error", "Library 'pyserial' belum terinstall.")
            return
        ports = [port.device for port in serial.tools.list_ports.comports()]
        self.port_combobox["values"] = ports
        if ports:
            self.port_combobox.current(0)
        else:
            self.port_combobox.set("No Port")

    def on_source_changed(self, _event=None):
        if not self.is_connected:
            self.refresh_ports()

    def toggle_connection(self):
        if not self.is_connected:
            if self.source_var.get() == "ST-Link SWD":
                if not PYOCD_AVAILABLE:
                    messagebox.showerror("Error", "Library 'pyocd' belum terinstall. Jalankan: pip install pyocd")
                    return
                self.connection_generation += 1
                generation = self.connection_generation
                self.stop_thread = False
                self.is_connected = True
                self.btn_connect.configure(text="Connecting...", state=tk.DISABLED, bg="#FFD166", fg="#0B132B")
                self.lbl_ribbon_status.configure(text="● SYSTEM: CONNECTING ST-LINK…", fg="#FFD166")
                self.serial_thread = threading.Thread(target=self.pyocd_rx_loop, args=(generation,), daemon=True)
                self.serial_thread.start()
                return
            port = self.port_combobox.get()
            if not port or port == "No Port":
                messagebox.showwarning("Warning", "Pilih Port COM STM32 terlebih dahulu!")
                return
            try:
                self.ser = serial.Serial(port, 9600, timeout=1.0)
                self.is_connected = True
                self.stop_thread = False
                self.btn_connect.configure(text="Disconnect", bg="#FF4D4D", fg="white")
                self.lbl_ribbon_status.configure(text=f"● SYSTEM: CONNECTED ({port})", fg="#06D6A0")

                self.serial_thread = threading.Thread(target=self.serial_rx_loop, daemon=True)
                self.serial_thread.start()
            except Exception as e:
                messagebox.showerror("Error", f"Gagal membuka port:\n{str(e)}")
        else:
            self.disconnect()

    def disconnect(self):
        self.connection_generation += 1
        self.stop_thread = True
        if self.ser and self.ser.is_open:
            try:
                self.ser.close()
            except:
                pass
        self.is_connected = False
        self.btn_connect.configure(text="Connect", bg="#48CAE4", fg="#0B132B")
        self.btn_connect.configure(state=tk.NORMAL)
        self.lbl_ribbon_status.configure(text="● SYSTEM: DISCONNECTED", fg="#FF4D4D")
        if self.pc_logging:
            self.toggle_pc_logging()

    def serial_rx_loop(self):
        buffer = bytearray()
        while not self.stop_thread and self.is_connected:
            try:
                if self.ser.in_waiting > 0:
                    b = self.ser.read(self.ser.in_waiting)
                    buffer.extend(b)

                    while b'\xff\xff\xff' in buffer:
                        idx = buffer.find(b'\xff\xff\xff')
                        cmd_bytes = buffer[:idx]
                        buffer = buffer[idx+3:]
                        try:
                            cmd_str = cmd_bytes.decode('ascii', errors='ignore')
                            self.parse_nextion_cmd(cmd_str)
                        except:
                            pass
                else:
                    time.sleep(0.02)
            except:
                break
        self.is_connected = False

    def pyocd_rx_loop(self, generation):
        """Poll the mapped STM32 variables off the Tk thread."""
        try:
            session = ConnectHelper.session_with_chosen_probe(
                blocking=False, return_first=True,
                options={"target_override": "stm32f103rc", "frequency": 100000,
                         "connect_mode": "attach", "resume_on_disconnect": True,
                         "warning.cortex_m_default": False})
            if session is None:
                raise RuntimeError("Probe ST-Link tidak ditemukan")
            self.pyocd_session = session
            # Use a proper attach session so pyOCD creates/selects the Cortex-M
            # core, while attach mode avoids the default halt/reset connect.
            session.open()
            target = session.target
            self.data_queue.put(("status", "CONNECTED (ST-LINK SWD)", "#06D6A0"))
            while not self.stop_thread and generation == self.connection_generation:
                memory = target.read_memory_block8(0x20000000, 0x4F4)
                values = {
                    "soh": self._unpack_float(memory, 0x20000000),
                    "sys_bat": memory[0x20000004 - 0x20000000],
                    "sd_status": bytes(memory[0x2000000C - 0x20000000:0x20000016 - 0x20000000]).split(b"\0", 1)[0].decode("ascii", errors="replace"),
                    "voltage": self._unpack_float(memory, 0x20000248),
                    "current": self._unpack_float(memory, 0x2000024C),
                    "soc": self._unpack_float(memory, 0x20000250),
                    "v_internal_bat": self._unpack_float(memory, 0x20000254),
                    "sd_ready": memory[0x20000494 - 0x20000000],
                    "samples": int.from_bytes(memory[0x200004F0 - 0x20000000:0x200004F4 - 0x20000000], "little"),
                }
                self.data_queue.put(("data", values))
                time.sleep(0.25)
        except Exception as exc:
            self.data_queue.put(("error", str(exc), generation))
        finally:
            if self.pyocd_session:
                try:
                    self.pyocd_session.close()
                except Exception:
                    pass
                self.pyocd_session = None
            self.data_queue.put(("worker_done", generation))

    @staticmethod
    def _unpack_float(memory, address):
        import struct
        offset = address - 0x20000000
        return struct.unpack("<f", bytes(memory[offset:offset + 4]))[0]

    def process_data_queue(self):
        """Apply worker results on Tk's main thread."""
        try:
            while True:
                item = self.data_queue.get_nowait()
                if item[0] == "data":
                    data = item[1]
                    self.voltage, self.current, self.soc = data["voltage"], data["current"], data["soc"]
                    self.soh, self.sys_bat = data["soh"], data["sys_bat"]
                    self.v_internal_bat = data["v_internal_bat"]
                    self.sd_status = data["sd_status"] or ("READY" if data["sd_ready"] else "NOT READY")
                    self.sd_logged_samples = data["samples"]
                    self.lbl_ribbon_bat.configure(text=f"🔋 Baterai Alat: {self.sys_bat}%",
                                                  fg="#06D6A0" if self.sys_bat >= 20 else "#FF4D4D")
                    self.lbl_ribbon_sd.configure(text=f"💾 SD Card: {self.sd_status}",
                                                 fg="#06D6A0" if data["sd_ready"] else "#FFD166")
                    self.lbl_diag_sd.configure(text=f"• SD Card Storage: {self.sd_status} (LOG.CSV)")
                    self.lbl_diag_samp.configure(text=f"• Logged Packets: {self.sd_logged_samples} samples")
                    self.lbl_home_soc.configure(text=f"{self.soc:.1f}")
                    self.home_soc_bar["value"] = self.soc
                    self.lbl_home_soc_num.configure(text=f"{self.soc:.1f}%")
                    self.soc_aekf = self.soc
                    self.lbl_an_aekf_soc.configure(text=f"{self.soc_aekf:.1f}%")
                    self.lbl_home_soh.configure(text=f"{self.soh:.1f}%")
                    self.lbl_home_soh_desc.configure(text="Condition: GOOD" if self.soh >= 80 else "Condition: DEGRADED")
                    if not self.hold_reading:
                        self.update_live_measurements()
                elif item[0] == "status":
                    self.lbl_ribbon_status.configure(text="CONNECTED (ST-LINK SWD)", fg=item[2])
                    self.btn_connect.configure(text="Disconnect", state=tk.NORMAL, bg="#FF4D4D", fg="white")
                elif item[0] == "error":
                    if item[2] == self.connection_generation and not self.stop_thread:
                        self.is_connected = False
                        self.lbl_ribbon_status.configure(text="DISCONNECTED", fg="#FF4D4D")
                        self.btn_connect.configure(text="Connect", state=tk.NORMAL, bg="#48CAE4", fg="#0B132B")
                        messagebox.showerror("ST-Link / pyOCD", item[1])
                elif item[0] == "worker_done":
                    if item[1] == self.connection_generation:
                        self.is_connected = False
                        self.lbl_ribbon_status.configure(text="DISCONNECTED", fg="#FF4D4D")
                        self.btn_connect.configure(text="Connect", state=tk.NORMAL, bg="#48CAE4", fg="#0B132B")
        except __import__("queue").Empty:
            pass
        self.after(50, self.process_data_queue)

    def parse_nextion_cmd(self, cmd):
        match = re.search(r'xstr\s+\d+,\d+,\d+,\d+,\d+,\d+,\d+,\d+,\d+,\d+,"([^"]*)"', cmd)
        if match:
            text = match.group(1).strip()
            self.process_incoming_data(cmd, text)

    def process_incoming_data(self, cmd, text):
        # 1. Header Status
        if text in ["READY", "CALIB", "INIT", "START"]:
            self.run_status = text
            self.lbl_ribbon_status.configure(text=f"● SYSTEM: {text}",
                                             fg="#06D6A0" if text == "READY" else "#FFD166")

        # 2. Battery Sistem
        elif text.startswith("BAT"):
            m = re.search(r'BAT\s+(\d+)', text)
            if m:
                self.sys_bat = int(m.group(1))
                self.lbl_ribbon_bat.configure(text=f"🔋 Baterai Alat: {self.sys_bat}%",
                                              fg="#06D6A0" if self.sys_bat >= 20 else "#FF4D4D")

        # 3. SD Card Status & Logged Samples
        elif text.startswith("SD"):
            self.sd_status = text
            self.lbl_ribbon_sd.configure(text=f"💾 SD Card: {text}",
                                         fg="#06D6A0" if "OK" in text else "#FFD166")
            self.lbl_diag_sd.configure(text=f"• SD Card Storage: {text} (LOG.CSV)")
        elif text.startswith("LOG:"):
            m = re.search(r'LOG:\s*(\d+)', text)
            if m:
                self.sd_logged_samples = int(m.group(1))
                self.lbl_diag_samp.configure(text=f"• Logged Packets: {self.sd_logged_samples} samples")

        # 4. Voltage
        elif "xstr 12," in cmd or "xstr 14," in cmd:
            try:
                v = float(text)
                if not self.hold_reading:
                    self.voltage = v
                    self.update_live_measurements()
            except:
                pass

        # 5. Current
        elif "xstr 164," in cmd or "xstr 178," in cmd:
            try:
                i = float(text)
                if not self.hold_reading:
                    self.current = i
                    self.update_live_measurements()
            except:
                pass

        # 6. SOC
        elif "xstr 76," in cmd or "xstr 85," in cmd or "xstr 190," in cmd:
            try:
                soc_val = float(text.replace("%", ""))
                self.soc = soc_val
                self.soc_aekf = soc_val
                self.lbl_home_soc.configure(text=f"{self.soc:.1f}")
                self.home_soc_bar["value"] = self.soc
                self.lbl_home_soc_num.configure(text=f"{self.soc:.1f}%")
                self.lbl_an_aekf_soc.configure(text=f"{self.soc_aekf:.1f}%")
            except:
                pass

        # 7. SOH
        elif "xstr 215," in cmd or "xstr 220," in cmd:
            try:
                soh_val = float(text.replace("%", ""))
                self.soh = soh_val
                self.lbl_home_soh.configure(text=f"{self.soh:.1f}%")
                self.lbl_home_soh_desc.configure(
                    text="Condition: GOOD" if self.soh >= 80 else "Condition: DEGRADED"
                )
            except:
                pass

    def update_live_measurements(self):
        # Update Home display
        self.lbl_home_v.configure(text=f"{self.voltage:.3f}")
        self.lbl_home_i.configure(text=f"{self.current:.3f}")
        self.power = self.voltage * self.current
        self.lbl_home_p.configure(text=f"{self.power:.2f}")

        # Update Quick Check Tab
        self.lbl_qc_vnow.configure(text=f"{self.voltage:.3f} V")
        self.lbl_qc_inow.configure(text=f"{self.current:.3f} A")

        if self.voltage > 1.0:
            if self.voltage < self.v_min:
                self.v_min = self.voltage
                self.lbl_qc_vmin.configure(text=f"{self.v_min:.3f} V")
            if self.voltage > self.v_max:
                self.v_max = self.voltage
                self.lbl_qc_vmax.configure(text=f"{self.v_max:.3f} V")

        if abs(self.current) > self.i_peak:
            self.i_peak = abs(self.current)
            self.lbl_qc_ipeak.configure(text=f"{self.i_peak:.3f} A")

        if self.current > 0.05:
            self.lbl_qc_idir.configure(text="CHARGING ⬆", fg="#06D6A0")
        elif self.current < -0.05:
            self.lbl_qc_idir.configure(text="DISCHARGING ⬇", fg="#FF4D4D")
        else:
            self.lbl_qc_idir.configure(text="STANDBY / NO LOAD", fg="#FFD166")

        # Coarse voltage-only LFP estimate; the firmware EKF is authoritative.
        if self.voltage >= 8.0:
            v_cell = self.voltage / 4.0  # pack LiFePO4 4S
            v_clamped = max(2.5, min(3.65, v_cell))
            # Coarse LFP OCV estimate. The flat plateau makes this diagnostic only.
            self.soc_ocv = max(0.0, min(100.0, (v_clamped - 2.5) / 1.15 * 100.0))
            self.lbl_qc_soc_ocv.configure(text=f"{self.soc_ocv:.1f}%")
            self.lbl_an_ocv_soc.configure(text=f"{self.soc_ocv:.1f}%")

            self.soc_error = abs(self.soc_aekf - self.soc_ocv)
            self.lbl_an_err.configure(text=f"{self.soc_error:.2f}%")
        else:
            self.soc_ocv = 0.0
            self.soc_error = 0.0
            self.lbl_qc_soc_ocv.configure(text="-- %")
            self.lbl_an_ocv_soc.configure(text="-- %")
            self.lbl_an_err.configure(text="-- %")

        # Cumulative Energy Wh
        now = time.time()
        if self.last_energy_tick is not None:
            dt_hours = (now - self.last_energy_tick) / 3600.0
            # Signed energy: positive current means charging, negative means load.
            self.accumulated_energy += (self.power * dt_hours)
            self.lbl_an_wh.configure(text=f"{self.accumulated_energy:.2f} Wh")
        self.last_energy_tick = now

        # PC Logging
        now_str = datetime.now().strftime("%H:%M:%S")
        if self.pc_logging and self.csv_writer:
            elapsed = time.time() - self.log_start_time
            self.csv_writer.writerow([
                f"{elapsed:.2f}",
                f"{self.voltage:.3f}",
                f"{self.current:.3f}",
                f"{self.power:.2f}",
                f"{self.soc:.2f}",
                f"{self.soh:.2f}",
                f"{self.accumulated_energy:.2f}",
                f"{self.soc_ocv:.2f}",
                f"{self.soc_error:.2f}",
                f"{self.v_internal_bat:.3f}",
                self.sys_bat,
                self.sd_status,
                self.sd_logged_samples,
                "ST-Link SWD" if self.source_var.get() == "ST-Link SWD" else "Serial"
            ])
            self.csv_file.flush()
            self.total_pc_logged += 1
            self.lbl_logger_count.configure(text=f"Total Recorded: {self.total_pc_logged} records")

        # Add to Treeview (Logger Tab)
        self.tree_logger.insert("", 0, values=(now_str, f"{self.voltage:.3f}", f"{self.current:.3f}",
                                               f"{self.power:.2f}", f"{self.soc:.1f}", f"{self.soh:.1f}",
                                               f"{self.accumulated_energy:.2f}"))
        if len(self.tree_logger.get_children()) > 100:
            self.tree_logger.delete(self.tree_logger.get_children()[-1])

    def toggle_hold(self):
        self.hold_reading = not self.hold_reading
        if self.hold_reading:
            self.held_v = self.voltage
            self.held_i = self.current
            self.btn_hold.configure(text="❄️ Hold Reading: ON", bg="#FFD166", fg="#0B132B")
        else:
            self.btn_hold.configure(text="❄️ Hold Reading: OFF", bg="#3A506B", fg="white")

    def toggle_pc_logging(self):
        if not self.pc_logging:
            file_path = filedialog.asksaveasfilename(
                defaultextension=".csv",
                filetypes=[("CSV Files", "*.csv"), ("All Files", "*.*")],
                initialfile=f"BIMA_LOG_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
            )
            if not file_path:
                return

            try:
                self.csv_file = open(file_path, mode="w", newline="")
                self.csv_writer = csv.writer(self.csv_file)
                self.csv_writer.writerow([
                    "Elapsed_s", "Voltage_V", "Current_A", "Power_W", "SoC_EKF_pct",
                    "SoH_pct", "Net_Energy_Wh", "SoC_OCV_approx_pct", "SoC_diff_pct",
                    "Internal_Battery_V", "Internal_Battery_pct", "SD_Status",
                    "SD_Samples", "Data_Source"
                ])
                self.log_start_time = time.time()
                self.total_pc_logged = 0
                self.pc_logging = True
                self.btn_pc_log.configure(text="⏹ Stop PC Logging", bg="#FF4D4D", fg="white")
                self.lbl_logger_status.configure(text="Logging Status: RECORDING TO CSV", fg="#06D6A0")
            except Exception as e:
                messagebox.showerror("File Error", f"Gagal membuat file CSV:\n{str(e)}")
        else:
            self.pc_logging = False
            if self.csv_file:
                try:
                    self.csv_file.close()
                except:
                    pass
                self.csv_file = None
                self.csv_writer = None
            self.btn_pc_log.configure(text="▶ Start PC Logging (CSV)", bg="#06D6A0", fg="#0B132B")
            self.lbl_logger_status.configure(text="Logging Status: IDLE / SAVED", fg="#FFD166")

            # Catat ke History Session
            stamp = datetime.now().strftime("%Y-%m-%d %H:%M")
            self.tree_history.insert("", 0, values=(stamp, self.setup_chemistry.get(), f"{self.voltage:.3f}",
                                                    f"{self.current:.3f}", f"{self.soc:.1f}", f"{self.soh:.1f}", "COMPLETED"))
            self.lbl_hist_time.configure(text=stamp)

    def set_mode(self, mode_name):
        self.analysis_mode = mode_name
        if "Accurate" in mode_name:
            self.btn_acc_mode.configure(bg="#06D6A0", fg="#0B132B", font=("Segoe UI", 8, "bold"))
            self.btn_normal_mode.configure(bg="#3A506B", fg="white", font=("Segoe UI", 8))
        else:
            self.btn_normal_mode.configure(bg="#06D6A0", fg="#0B132B", font=("Segoe UI", 8, "bold"))
            self.btn_acc_mode.configure(bg="#3A506B", fg="white", font=("Segoe UI", 8))
        self.lbl_ribbon_mode.configure(text=f"Mode: {mode_name} ({self.setup_nominal_v.get()} {self.setup_capacity.get()})")

    def apply_battery_profile(self):
        v = self.setup_nominal_v.get()
        c = self.setup_capacity.get()
        ch = self.setup_chemistry.get()
        self.lbl_ribbon_mode.configure(text=f"Mode: {self.analysis_mode} ({v} {c})")
        self.lbl_setup_summary.configure(
            text=f"• Chemistry: {ch} | 4S configuration | Nominal: {v} | Capacity: {c} | Ready for testing"
        )

    def reset_stats(self):
        self.v_min = 999.0
        self.v_max = -999.0
        self.i_peak = 0.0
        self.lbl_qc_vmin.configure(text="-- V")
        self.lbl_qc_vmax.configure(text="-- V")
        self.lbl_qc_ipeak.configure(text="0.000 A")


if __name__ == "__main__":
    app = BimaBatteryDashboard()
    app.mainloop()
