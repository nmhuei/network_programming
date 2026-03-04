# Remote System Monitor

Hệ thống giám sát từ xa viết bằng **C thuần** (chỉ sử dụng thư viện chuẩn POSIX). Agent chạy trên các máy Linux, thu thập metrics CPU/RAM/Disk và gửi đến server trung tâm qua TCP. Server hiển thị **dashboard real-time** trên terminal với progress bars và color-coding.

---

## Kiến trúc

```
┌──────────────────────────────────────────────────────────┐
│                   Monitor Server                         │
│              (monitor_server.c)                          │
│                                                          │
│  ╔══════════════════════════════════════╗                │
│  ║  Remote System Monitor  Dashboard   ║                 │
│  ╚══════════════════════════════════════╝                │
│  ┌──────────┬──────────────┬───────┬──────────┐          │
│  │ HOST     │ CPU% ████░░  │ ...   │ ● ONLINE │          │
│  └──────────┴──────────────┴───────┴──────────┘          │
│                                                          │
│  State: hosts[] history[] alerts[] clients[]             │
└──────────────────────────────────────────────────────────┘
        ▲ TCP          ▲ TCP            ▲ TCP
  ┌─────┴──────┐ ┌─────┴──────┐  ┌─────┴──────┐
  │   agent    │ │   agent    │  │  Viewer    │
  │   web-1    │ │   db-1     │  │ (netcat)   │
  └────────────┘ └────────────┘  └────────────┘
```

| File | Vai trò |
|---|---|
| `monitor_server.c` | Server trung tâm – dashboard, cảnh báo, truy vấn |
| `agent.c` | Agent thu thập metrics từ `/proc` và gửi JSON qua TCP |
| `thresholds.conf` | Cấu hình ngưỡng cảnh báo (global + per-host) |

---

## Build

```bash
gcc -Wall -o monitor_server monitor_server.c -lm
gcc -Wall -o agent agent.c -lm
```

> [!NOTE]
> Không cần thư viện bên ngoài. Chỉ sử dụng standard C library + POSIX headers.

---

## Monitor Server (`monitor_server`)

### Chức năng

| Chức năng | Mô tả |
|---|---|
| **Dashboard real-time** | Bảng metrics hiển thị toàn bộ terminal, cập nhật mỗi 5 giây |
| **Progress bars** | Thanh bar `████░░` cho CPU/RAM/DISK, đổi màu theo mức độ |
| **Status indicators** | `● ONLINE` (xanh lá) / `○ OFFLINE` (đỏ) |
| **Box-drawing borders** | Viền `┌─┬─┐│└─┴─┘` Unicode |
| **Cảnh báo ngưỡng** | Broadcast alert khi metrics vượt threshold đến tất cả viewers |
| **Giới hạn agents** | Tối đa **6 agents** (tính cả đã ngắt kết nối) |
| **Chống trùng tên** | Từ chối agent trùng tên với agent đang kết nối |
| **Terminal resize** | Tự động điều chỉnh layout khi thay đổi kích thước terminal (SIGWINCH) |
| **Cảnh báo terminal nhỏ** | Hiện warning với size hiện tại/yêu cầu khi terminal quá nhỏ |
| **Viewer commands** | Hỗ trợ `/view`, `/history`, `/log`, `/help` qua TCP |
| **Không hiện log trên server** | Server terminal chỉ hiện bảng, mọi event chỉ xem qua `/log` |

### CLI

```bash
./monitor_server [OPTIONS]
```

| Option | Mặc định | Mô tả |
|---|---|---|
| `--port PORT` | `8784` | Cổng TCP để lắng nghe |
| `--host HOST` | `0.0.0.0` | Interface bind (mặc định: tất cả) |
| `--config FILE` | `thresholds.conf` | Đường dẫn file cấu hình ngưỡng |

**Ví dụ:**
```bash
./monitor_server --port 8784 --config thresholds.conf
```

### Dashboard

Server terminal **chỉ hiện bảng metrics** (không hiện log):

```
  ╔══════════════════════════════════════╗
  ║   Remote System Monitor  Dashboard   ║
  ║   2026-03-03 14:00:00                ║
  ╚══════════════════════════════════════╝

┌────────────────────┬────────────────┬────────────────┬────────────────┬────────────┬────────────────────────────────┐
│ HOST               │ CPU%           │ RAM%           │ DISK%          │ STATUS     │ LAST ALERT                     │
├────────────────────┼────────────────┼────────────────┼────────────────┼────────────┼────────────────────────────────┤
│ db-server          │  78.5 █████░   │  82.3 █████░   │  55.0 ███░░░   │ ● ONLINE   │ CPU=78.5% (threshold=70%)      │
│ web-1              │  23.4 █░░░░░   │  45.2 ███░░░   │  32.1 ██░░░░   │ ● ONLINE   │                                │
│ web-2              │  92.1 ██████   │  95.7 ██████   │  88.4 █████░   │ ○ OFFLINE  │                                │
└────────────────────┴────────────────┴────────────────┴────────────────┴────────────┴────────────────────────────────┘
```

**Color coding:**
- Metrics: 🟢 xanh (< 70%) → 🟡 vàng (70–85%) → 🔴 đỏ (≥ 85%)
- Progress bar: `█` (filled, theo màu metric) + `░` (empty, dim)
- Status: `● ONLINE` (bold xanh lá) | `○ OFFLINE` (dim đỏ)
- Alert: bold đỏ sáng

### Yêu cầu terminal tối thiểu

| Dimension | Giá trị | Ghi chú |
|---|---|---|
| **Width** | 119 cột | Tự tính từ `COL_*` defines |
| **Height** | 12 hàng | = `DASHBOARD_MIN_ROWS` |

Nếu terminal nhỏ hơn → hiển thị cảnh báo centered với kích thước hiện tại (màu xanh/đỏ) và kích thước yêu cầu. Tự động khôi phục khi phóng to đủ.

### Giới hạn kết nối

- Tối đa **`MAX_AGENTS = 6`** agent duy nhất (tính cả agent đã ngắt kết nối)
- Agent kết nối lại với **cùng tên** → cho phép (không tính thêm)
- Agent mới khi đã đủ 6 → bị từ chối: `ERROR: Server full (6 agents max). Connection rejected.`
- Agent trùng tên với agent đang kết nối → bị từ chối: `ERROR: Agent name 'xxx' is already connected. Choose a different --name.`

### Viewer Commands

Kết nối viewer bằng `nc` hoặc `telnet`:

```bash
nc <server_ip> <port>
```

| Lệnh | Mô tả |
|---|---|
| `/view` | Hiển thị danh sách tất cả hosts và metrics/status |
| `/history <host> <minutes>` | Xem lịch sử metrics (VD: `/history web-1 10`) |
| `/log [count]` | Xem connection events gần đây (mặc định: 20) |
| `/help` | Hiển thị danh sách lệnh |

### Kiến trúc nội bộ (Server)

Server sử dụng mô hình **single-thread + `select()`** event loop:

- Tất cả client (agent + viewer) kết nối trên cùng một cổng TCP
- Phân loại client theo message đầu tiên: JSON `{...}` → agent, `/command` → viewer
- Dashboard được vẽ lại mỗi 5 giây hoặc khi có dữ liệu mới
- Dữ liệu được lưu trữ hoàn toàn trong bộ nhớ (không dùng database)

**Cấu trúc dữ liệu chính:**

| Struct | Mô tả |
|---|---|
| `HostInfo` | Thông tin host: tên, metrics hiện tại, trạng thái, alert, history buffer |
| `HistoryEntry` | Một bản ghi lịch sử: timestamp + cpu/ram/disk |
| `ClientInfo` | Thông tin kết nối TCP: fd, loại (agent/viewer), line buffer |
| `ThresholdEntry` | Ngưỡng cảnh báo: tên host (hoặc global) + cpu/ram/disk |
| `EventEntry` | Event log: timestamp + message |

---

## Agent (`agent`)

### Chức năng

| Chức năng | Mô tả |
|---|---|
| **Thu thập metrics** | Đọc CPU/RAM/Disk từ `/proc` (Linux kernel) |
| **Gửi JSON qua TCP** | Newline-delimited JSON (NDJSON) |
| **Auto-reconnect** | Tự kết nối lại khi mất kết nối (tối đa 5 lần liên tiếp) |
| **Table output** | Hiển thị bảng metrics màu trên terminal agent |
| **Error handling** | Thoát sạch khi nhận `ERROR:` từ server |

### CLI

```bash
./agent --server HOST:PORT [OPTIONS]
```

| Option | Mặc định | Bắt buộc | Mô tả |
|---|---|---|---|
| `--server HOST:PORT` | — | ✅ | Địa chỉ server (VD: `192.168.1.5:8784`) |
| `--interval SECONDS` | `5.0` | ❌ | Khoảng cách gửi metrics (giây) |
| `--name HOSTNAME` | hostname hệ thống | ❌ | Tên agent (hiển thị trên dashboard) |

**Ví dụ:**
```bash
./agent --server 192.168.1.5:8784 --interval 5 --name web-1
```

### Metric Collection

| Metric | Nguồn | Phương pháp |
|---|---|---|
| **CPU%** | `/proc/stat` | Lấy 2 mẫu cách nhau 0.5s, tính `(delta_busy / delta_total) × 100` |
| **RAM%** | `/proc/meminfo` | `(MemTotal − MemAvailable) / MemTotal × 100` |
| **Disk%** | `statvfs("/")` | `(f_blocks − f_bfree) / f_blocks × 100` |

### Giao thức (Protocol)

**Metric frame** (agent → server) — Newline-delimited JSON:
```json
{"host":"web-1","timestamp":1700000000,"cpu":87.0,"ram":62.3,"disk":44.8}
```

| Field | Type | Mô tả |
|---|---|---|
| `host` | string | Tên agent (`--name` hoặc hostname) |
| `timestamp` | int | Unix epoch seconds |
| `cpu` | float | CPU % |
| `ram` | float | RAM % |
| `disk` | float | Root disk % |

### Kết nối và tự phục hồi

- TCP socket kết nối tới `HOST:PORT`
- Nếu mất kết nối: đợi `RECONNECT_DELAY = 5` giây rồi thử lại
- Tối đa `MAX_RETRIES = 5` lần liên tiếp thất bại → thoát
- Khi kết nối thành công → reset bộ đếm retry về 0
- Nếu nhận `ERROR:` từ server → thoát ngay (VD: trùng tên, server full)

### Output trên terminal agent

```
[agent] Starting agent 'web-1'  interval=5.0s
[agent] Connected to 127.0.0.1:8784
+---------------------+--------+--------+--------+
| TIME                | CPU%   | RAM%   | DISK%  |
+---------------------+--------+--------+--------+
| 2026-03-03 14:00:05 |  10.9  |  61.7  |  40.3  |
| 2026-03-03 14:00:10 |   8.2  |  61.5  |  40.3  |
```

Metrics được color-coded: xanh (< 70%), vàng (70–85%), đỏ (≥ 85%).

---

## Thresholds Configuration (`thresholds.conf`)

```ini
[global]
cpu  = 80
ram  = 90
disk = 85

[host:web-1]
cpu  = 70        # ngưỡng CPU riêng cho web-1

[host:db-server]
ram  = 80
disk = 90
```

- Section `[global]` — áp dụng cho tất cả hosts
- Section `[host:<name>]` — ghi đè ngưỡng riêng cho host cụ thể
- Mặc định nếu không có config: CPU=80%, RAM=90%, DISK=85%

**Alert broadcast format** (server → tất cả viewers):
```
*** [14:32:01] ALERT host=web-1: CPU=87.0% (threshold=80.0%), RAM=92.1% (threshold=90.0%) ***
```

---

## Hằng số quan trọng

### Monitor Server

| Hằng số | Giá trị | Mô tả |
|---|---|---|
| `MAX_AGENTS` | `6` | Số agent tối đa (kể cả đã ngắt) |
| `OFFLINE_TIMEOUT` | `30s` | Thời gian chờ trước khi đánh OFFLINE |
| `HISTORY_MAXLEN` | `10,000` | Số bản ghi lịch sử tối đa/host |
| `EVENT_LOG_MAXLEN` | `50` | Số event log giữ trong bộ nhớ |
| `DASHBOARD_MIN_ROWS` | `12` | Chiều cao tối thiểu dashboard |
| `DASHBOARD_MAX_ROWS` | `40` | Chiều cao tối đa dashboard |
| `MIN_TERM_WIDTH` | `119` | Chiều rộng terminal tối thiểu |
| `MIN_TERM_HEIGHT` | `12` | Chiều cao terminal tối thiểu |
| `MAX_CLIENTS` | `64` | Số kết nối TCP đồng thời tối đa |
| `REFRESH_INTERVAL` | `5s` | Chu kỳ vẽ lại dashboard |

### Agent

| Hằng số | Giá trị | Mô tả |
|---|---|---|
| `RECONNECT_DELAY` | `5s` | Đợi trước khi reconnect |
| `MAX_RETRIES` | `5` | Số lần retry liên tiếp tối đa |

---

## Yêu cầu hệ thống

- **Compiler**: GCC hoặc Clang hỗ trợ C11
- **OS**: Linux (agent bắt buộc `/proc` filesystem, server cần POSIX)
- **Thư viện**: Chỉ standard C + POSIX — `stdio`, `stdlib`, `string`, `unistd`, `sys/socket`, `sys/select`, `sys/statvfs`, `netinet/in`, `arpa/inet`, `signal`, `time`, `getopt`, `fcntl`, `termios`, `math`
- **Terminal**: Hỗ trợ ANSI escape codes và Unicode box-drawing characters

---

## Cấu trúc file

```
D1/
├── monitor_server.c   # Source code server
├── agent.c            # Source code agent
├── thresholds.conf    # Cấu hình ngưỡng cảnh báo
└── README.md          # File này
```

Sau khi build:
```
D1/
├── monitor_server     # Binary server (không commit)
├── agent              # Binary agent (không commit)
├── monitor_server.c
├── agent.c
├── thresholds.conf
└── README.md
```

---

## Quick Start

```bash
# 1. Build
gcc -Wall -o monitor_server monitor_server.c -lm
gcc -Wall -o agent agent.c -lm

# 2. Terminal 1 — Khởi động server
./monitor_server --port 8784

# 3. Terminal 2 — Khởi động agent
./agent --server 127.0.0.1:8784 --name web-1 --interval 5

# 4. Terminal 3 — Kết nối viewer
nc 127.0.0.1 8784
/view
/help
/history web-1 10
/log
```
