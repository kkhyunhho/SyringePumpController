# SyringePumpController

**Runze SY-01B 정밀 시린지 펌프**를 Python으로 제어하기 위한 드라이버입니다. USB-RS232 변환기(CH340 칩셋 기반 EUSB-30 동글)를 통해 PC와 펌프를 연결하고, DT ASCII 프로토콜로 통신합니다.

> **현재 상태 (`0.2.0.dev0`, pre-alpha):** 펌프를 **움직이지 않고** 상태/식별 정보만 읽는 **읽기 전용 진단 API**까지 구현돼 있습니다. 실제 장비(`/dev/ttyUSB1`)에서 software `8.33`, serial `32656` 응답 수신을 확인했습니다.
> 초기화·흡입(aspirate)·토출(dispense)·중단(abort) 같이 펌프가 **실제로 움직이는 명령은 아직 구현되어 있지 않습니다.** 테스트 스위트가 이 점을 강제로 검증합니다(`TestNoMotionCommandsExposed`).

---

## 시린지 펌프를 처음 다뤄보신다면

이 펌프는 "주사기 모양의 정밀 디스펜서"라고 생각하시면 됩니다. 내부에 모터가 달린 플런저(piston)가 시린지를 위아래로 움직여서 액체를 nL ~ mL 단위로 정확히 빨아들이고(aspirate) 뱉어내고(dispense), 옆에 붙은 **밸브**가 어느 포트에서 액체를 받고 어디로 보낼지 정합니다. 호스트(여러분의 PC)가 시리얼 명령을 보내면 펌프가 그 명령대로 움직입니다.

처음 만지실 때 반드시 알아두실 것:

- **24 V DC 전원**이 필요합니다(≥1.5 A). 전원이 들어간 상태로 케이블을 꽂거나 뽑으면 **절대 안 됩니다.**
- **시리얼 통신**: 기본 9600 bps, 8N1. EUSB-30 동글을 PC USB에 꽂으면 `/dev/ttyUSB0` 또는 `/dev/ttyUSB1` 같은 장치 노드로 보입니다.
- **주소 스위치**: 펌프 본체의 회전 스위치(0–F)로 펌프의 ID를 설정합니다. 위치 0 → 주소 `1`, 위치 1 → 주소 `2`, … (전원 켜기 **전에** 맞춰야 적용됨)
- **시린지 크기 (`syringe_uL`)**: 장착한 시린지 용량(µL)을 코드에 알려줘야 합니다(예: 125 µL, 1000 µL, 5000 µL). 크기에 따라 모터 토크 한계도 달라집니다.
- **무엇이든 움직이기 전에 진단부터.** 아래의 `diagnose()` 또는 `sy01b-diagnose` CLI는 펌프를 움직이지 않고 상태만 읽어옵니다. 배선/주소/전압을 먼저 이걸로 확인하세요.

자세한 하드웨어/프로토콜 레퍼런스는 [CLAUDE.md](CLAUDE.md)에, 구조 결정 배경은 [DESIGN.md](DESIGN.md)에 정리돼 있습니다.

---

## 통신 명령어 구조 (DT ASCII)

SY-01B는 켜진 직후 처음 받은 ASCII 명령의 형식을 보고 DT / OEM 중 하나로 **잠깁니다**(전원 재투입 전엔 안 바뀝니다). 이 드라이버는 사람이 읽기 쉬운 **DT (Data Terminal)** 형식만 씁니다. 체크섬이 없고, 줄바꿈/ETX로 프레임을 끝냅니다.

### 호스트 → 펌프 (요청)

```
   /     1     <commands>      \r
  '/'  주소바이트   명령 본문    캐리지리턴
  0x2F  '1'..'?'   ASCII 문자열  0x0D
```

- **시작 문자 `/`** — 모든 요청은 슬래시로 시작.
- **주소 바이트** — 펌프의 회전 스위치 값에 따라 결정. 정수 주소 1‥15를 ASCII로 매핑:
  - 주소 `1` → `'1'` (0x31), 주소 `2` → `'2'`, …, 주소 `9` → `'9'`,
  - 주소 `10` → `':'` (0x3A), …, 주소 `15` → `'?'` (0x3F).
  - 이 매핑은 `SyringePumpController.format_address()` 가 처리합니다.
- **명령 본문** — ASCII 명령어들의 나열. 여러 개를 이어붙일 수 있습니다(예: `IA6000OA0R` = 입력 포트로 → 6000 스텝까지 채움 → 출력 포트로 → 0 스텝까지 비움).
- **실행 트리거 `R`** — 펌프를 **실제로 움직이는** 명령(`Z`, `Y`, `A`, `P`, `D`, `I`, `O`, … )은 본문 끝에 `R` 이 붙어야 비로소 실행됩니다. `R` 없이 보내면 펌프의 명령 버퍼(최대 255바이트)에만 들어가고 다음 `R` 을 기다립니다. 보고/질의 명령(`Q`, `?23`, `?202`, `*` 등)에는 `R` 이 필요 없습니다.
- **종료 `\r`** — 캐리지 리턴 한 바이트.

> **이 코드베이스의 안전장치**: `sy01b-diagnose` CLI 와 진단 경로는 본문에 절대로 `R`/`Z`/`Y`/`W` 를 붙이지 않습니다. 테스트 `test_diagnose_never_sends_R_or_init_command` 가 모든 송신 프레임을 검사해서 이를 강제합니다.

빌더는 `SyringePumpController.build_command(address, cmds, *, execute=False)` 한 줄입니다. `execute=True` 일 때만 `R` 을 자동으로 붙입니다. (드라이버의 read-only 경로는 항상 `execute=False`.)

### 펌프 → 호스트 (응답)

```
   /     0   <status>   <data...>    ETX    \r   \n
  '/'  마스터  상태바이트   데이터      0x03  0x0D 0x0A
        주소
```

- **`/0`** — 응답은 항상 슬래시 + ASCII `'0'`(호스트=마스터 주소). 다른 값이면 프로토콜 에러로 처리합니다.
- **상태 바이트(1바이트)** — 비트 단위로 의미를 가집니다. `SyringePumpController.StatusByte.decode()` 가 검증·디코드합니다:

  | 비트 | 의미 |
  |---|---|
  | bit 7 | 항상 `0` |
  | bit 6 | 항상 `1` (프레임 식별 비트) |
  | bit 5 | `1` = busy(동작 중), `0` = ready(받을 준비됨) |
  | bit 4 | 예약 |
  | bits 3..0 | **에러 코드 (0 = OK)** |

  → `0x40` = ready + OK, `0x60` = busy + OK, `0x47` = ready + "초기화 안 됨(error 7)", 식.

- **데이터 영역** — 명령 종류에 따라 다릅니다. `?23` 는 펌웨어 버전 문자열, `?202` 는 시리얼 번호, `*` 는 전압×10(정수), `Q` 는 데이터 없음 등.
- **ETX (0x03)** — 데이터 끝 표시. 드라이버는 이 바이트를 만날 때까지 읽습니다(타임아웃: `reply_timeout_s`).
- **`\r\n`** — 줄바꿈. ETX 이후 트레일링.

### 에러 코드 (상태 바이트 하위 4비트)

| 코드 | 의미 | 회복 |
|---|---|---|
| 0 | OK | — |
| 1 | 초기화 실패 | 막힘 제거 후 **반드시 재초기화**. 그 전엔 모든 명령 거부. |
| 2 | 잘못된 명령 | 명령 고치고 다시 보내기. |
| 3 | 잘못된 피연산자 | 파라미터 수정. |
| 7 | 초기화 안 됨 | `Z`/`Y`/`W` 전송. |
| 9 | 플런저 과부하 | **반드시 재초기화**. |
| 10 | 밸브 과부하 | 다음 밸브 명령이 자동 홈잉. 반복되면 밸브 교체. |
| 11 | 밸브가 bypass — 플런저 불가 | 밸브를 bypass에서 빼낸 뒤 재시도. |
| 15 | 명령 버퍼 오버플로 | 이전 동작이 끝날 때까지 `Q` 폴링. |

각 코드는 `SyringePumpController.Error` 의 하위 예외로 자동 매핑됩니다(`InitFailedError`, `PlungerOverloadError`, `CommandOverflowError`, …). `device_error_for(code)` 가 코드→예외 클래스 매핑을 노출합니다.

> **bus 규칙**: 시리얼 모드에선 **비지/레디 판정의 유일한 신뢰 경로가 `Q`** 입니다. 다른 응답에 묻어 오는 비트 5는 신뢰하지 마세요(매뉴얼·CLAUDE.md 참고).

### 가장 자주 쓰는 명령 — 한눈에

| 보내는 것 | 의미 | 응답(예) | 메모 |
|---|---|---|---|
| `/1Q\r` | 펌프 1의 상태 조회 | `/0` + 상태 + ETX | 비지/레디·에러 확인용. **`R` 불필요.** |
| `/1?23\r` | 소프트웨어 버전 | `/0` + 상태 + `8.33` + ETX | 식별/통신 검증용. |
| `/1?202\r` | 시리얼 번호 | `/0` + 상태 + `32656` + ETX | 좋은 첫 통신 테스트 명령. |
| `/1*\r` | 공급 전압 (×10) | `/0` + 상태 + `240` + ETX | `240` → 24.0 V. 22 V 미만이면 진단 실패. |
| `/1?6\r` | 밸브 위치 | `/0` + 상태 + `I`/`O`/… + ETX | `B` 면 bypass — 플런저 동작 시 error 11 위험. |
| `/1?\r` | 플런저 위치(스텝) | `/0` + 상태 + 정수 + ETX | N0 모드에서 0‥12000. |
| `/1ZR\r` | **초기화** (CW 폴리시) | — | 전원 인가 후 **첫 모션 명령**. *아직 이 드라이버에는 미구현.* |
| `/1IA6000OA0R\r` | 입력→6000까지 흡입→출력→0까지 토출 | — | 멀티-스텝 한 줄 예. *아직 이 드라이버에는 미구현.* |

---

## 설치

Python ≥ 3.12 가 필요합니다.

```bash
python3 -m venv .venv
.venv/bin/pip install -e ".[dev]"
```

## 시리얼 포트 확인

펌프와 동글이 연결되어 있다면 Linux에서는 보통 `/dev/ttyUSB0` 또는 `/dev/ttyUSB1` 로 보입니다.

```bash
ls -l /dev/ttyUSB*
# 보이지 않으면 dmesg 마지막 몇 줄에서 ch341/ch340 메시지를 확인하세요:
dmesg | tail
```

권한 오류가 나면 사용자를 `dialout` 그룹에 추가하거나 `sudo` 가 필요할 수 있습니다.

## 가장 먼저 해볼 것 — 읽기 전용 진단

가장 안전한 첫걸음. 펌프를 한 번도 움직이지 않으면서 통신이 되는지, 전원이 정상인지, 시리얼 번호가 무엇인지를 확인합니다.

### CLI로

```bash
.venv/bin/sy01b-diagnose --port /dev/ttyUSB1 --address 1 --syringe-uL 125
```

성공하면 아래와 같은 한 화면짜리 리포트가 나옵니다:

```
SY-01B diagnostic report
  software version : 8.33
  serial number    : 32656
  config           : ...
  supply voltage   : 24.x V
  valve position   : I
  plunger position : 0 steps
  pre-init status  : busy=False error=NOT_INITIALIZED
  ok to initialize : True
```

### Python에서

```python
from sy01b import SyringePumpController

cfg = SyringePumpController.Config(
    port="/dev/ttyUSB1",
    address=1,
    syringe_uL=125,
)

with SyringePumpController.open(cfg) as pump:
    report = pump.diagnose()
    print(report.render())
```

저장소에 들어있는 [main.py](main.py) 가 실제로 동작하는 가장 짧은 예제입니다(소프트웨어 버전과 시리얼 번호만 읽고 끝납니다).

## 진단이 실패할 때 점검 순서

| 증상 | 의심해볼 것 |
|---|---|
| `DiagnosticTimeoutError` — 응답이 안 옴 | 포트 번호, 펌프 전원, 동글 LED, 케이블, **주소 스위치**가 코드의 `address` 와 일치하는지 |
| `LowSupplyVoltageError` — 22 V 미만 | 어댑터 정격, DB-15 단자 |
| `valve is in bypass` 경고 | 밸브가 bypass 상태 → 플런저를 움직이려고 하면 error 11. 다음 단계 코드에서 처리 예정. |
| `pre-init status` 가 `NOT_INITIALIZED` | **정상**입니다. 전원 켠 직후엔 초기화가 안 된 상태가 맞습니다(초기화 모션은 다음 마일스톤). |
| `DiagnosticGarbledReplyError` | 다른 시리얼 클라이언트가 같은 포트를 잡고 있거나, DT 가 아닌 OEM/RUNZE 형식으로 잠긴 상태일 수 있음 — 전원 재투입 후 첫 명령을 DT 로 보내세요. |

## 지금 코드로 할 수 있는 것

`from sy01b import SyringePumpController` 한 줄로 들어오는 단일 클래스에 모두 들어있습니다:

- DT ASCII 프레임 빌더/파서, 상태 바이트 디코드 (`build_command`, `parse_reply`, `StatusByte`)
- 펌프 에러 코드 → 예외 매핑 (`SyringePumpController.Error` 하위: `DeviceError`, `InitFailedError`, `PlungerOverloadError`, `CommandOverflowError`, …)
- 통신 계층 예외 (`TransportError`, `ProtocolError`, `DiagnosticError`)
- 설정 (`SyringePumpController.Config` — TOML 로더, 시린지→stall current 표 포함)
- 읽기 전용 쿼리: `query_status` (`Q`), `query_software_version` (`?23`), `query_serial_number` (`?202`), `query_config` (`?76`), `query_supply_voltage_v` (`*`), `query_valve_position` (`?6`), `query_plunger_position` (`?`)
- 종합 진단 `diagnose()` → `DiagnosticsReport`
- `sy01b-diagnose` 콘솔 스크립트

## 아직 안 들어있는 것

다음 커밋에서 들어올 예정입니다. 자세한 체크리스트는 [ToDo.md](ToDo.md):

- `initialize(...)` — `ZR`/`YR` 로 펌프 초기화
- `aspirate_uL` / `dispense_uL` / `move_to_steps` — 부피↔스텝 변환 포함
- `valve_to` / `valve_in` / `valve_out` / `valve_bypass`
- `abort()` 와 `requires_reinit` 래치
- `_wait_until_ready` (`Q` 폴링)

## 개발

```bash
.venv/bin/ruff check src tests          # 린트
.venv/bin/ruff format --check src tests # 포매팅 체크
.venv/bin/mypy                          # src/sy01b 에 strict 타입 체크
.venv/bin/pytest                        # 전체 테스트
.venv/bin/pytest --cov=sy01b --cov-report=term-missing
```

벤치에서 얻은 교훈과 hard-won 패턴들은 [LearnedPatterns.md](LearnedPatterns.md) 에 모으고 있습니다.
