# System Architecture Diagrams

## Control Logic Flow Diagram

```mermaid
graph TB
    subgraph "Temperature Sensors"
        S1[Inside Temp Sensor]
        S2[Tank Temp Sensor]
        S3[Boiler Temp Sensors]
        S4[Outside Temp Sensor]
    end

    subgraph "Control Tasks"
        HCT[HeatingControlTask]
        WCT[WheaterControlTask]
        BCT[BurnerControlTask]
        PID[SpaceHeatingPID]
    end

    subgraph "Event Groups"
        BRE[BURNER_REQUEST<br/>Event Group]
        SSE[SYSTEM_STATE<br/>Event Group]
    end

    subgraph "Hardware Control"
        RCT[RelayControlTask]
        RYN4[RYN4 Relays]
    end

    %% Sensor inputs
    S1 -->|Room Temp| HCT
    S4 -->|Outside Temp| HCT
    S2 -->|Tank Temp| WCT
    S3 -->|Boiler Temp| BCT
    S3 -->|Boiler Temp| PID

    %% Control flow
    HCT -->|HEATING_REQUEST_BIT<br/>+ Target Temp| BRE
    WCT -->|WATER_REQUEST_BIT<br/>+ Target Temp| BRE
    
    BRE -->|Check Requests| BCT
    
    BCT -->|Power Level<br/>Decision| PID
    PID -->|POWER_HALF/FULL| BRE
    
    BCT -->|Relay Commands| RCT
    RCT -->|Control| RYN4

    %% State updates
    BCT -->|State Updates| SSE
    RCT -->|Status Updates| SSE

    style HCT fill:#f9f,stroke:#333,stroke-width:2px
    style WCT fill:#f9f,stroke:#333,stroke-width:2px
    style BCT fill:#9ff,stroke:#333,stroke-width:2px
    style BRE fill:#ff9,stroke:#333,stroke-width:2px
```

## Burner Request Communication

```mermaid
sequenceDiagram
    participant HCT as HeatingControlTask
    participant WCT as WheaterControlTask
    participant BRE as BURNER_REQUEST EventGroup
    participant BCT as BurnerControlTask
    participant PID as PID Control
    participant RCT as RelayControlTask

    Note over HCT: Room temp < target - hysteresis
    HCT->>BRE: Set HEATING_REQUEST_BIT<br/>Encode target temp (70°C)
    
    Note over WCT: Tank temp < target - hysteresis
    WCT->>BRE: Set WATER_REQUEST_BIT<br/>Encode target temp (65°C)
    
    BCT->>BRE: Check request bits
    Note over BCT: Water has priority
    BCT->>BRE: Clear WATER_REQUEST_BIT
    BCT->>RCT: Enable burner, water mode
    BCT->>PID: Set target 65°C
    
    PID->>BRE: Set POWER_HALF_BIT
    BCT->>BRE: Check power bits
    BCT->>RCT: Set half power
```

## State Hierarchy

```mermaid
stateDiagram-v2
    [*] --> SystemOff: Power On
    
    SystemOff --> BoilerEnabled: Enable Boiler
    
    state BoilerEnabled {
        [*] --> Idle
        Idle --> HeatingActive: Heating Enabled &<br/>Heat Requested
        Idle --> WaterActive: Water Enabled &<br/>Water Requested
        
        state HeatingActive {
            [*] --> HeatingHalf
            HeatingHalf --> HeatingFull: Temp too low
            HeatingFull --> HeatingHalf: Temp approaching target
        }
        
        state WaterActive {
            [*] --> WaterHalf
            WaterHalf --> WaterFull: Temp too low
            WaterFull --> WaterHalf: Temp approaching target
        }
        
        HeatingActive --> Idle: Target reached +<br/>hysteresis
        WaterActive --> Idle: Target reached +<br/>hysteresis
        
        HeatingActive --> WaterActive: Water priority &<br/>water requested
        WaterActive --> HeatingActive: Water done &<br/>heating requested
    }
    
    BoilerEnabled --> SystemOff: Disable Boiler
```

## Hysteresis Implementation

```mermaid
graph LR
    subgraph "Heating Hysteresis (2°C)"
        H1[OFF<br/>T < 19°C] -->|Room < 19°C| H2[ON<br/>19°C < T < 21°C]
        H2 -->|Room > 21°C| H1
        H2 -.->|Maintains ON| H2
    end
    
    subgraph "Water Hysteresis (4°C)"
        W1[OFF<br/>T < 46°C] -->|Tank < 46°C| W2[ON<br/>46°C < T < 50°C]
        W2 -->|Tank > 50°C| W1
        W2 -.->|Maintains ON| W2
    end
```

## Task Timing and Priorities

```mermaid
gantt
    title Task Execution Timeline
    dateFormat X
    axisFormat %s
    
    section Control Tasks
    HeatingControlTask (1s)  :active, hct, 0, 1
    WheaterControlTask (1s)  :active, wct, 0, 1
    BurnerControlTask (200ms):crit, bct, 0, 0.2
    
    section PID Control
    SpaceHeatingPID (5s)     :active, pid, 0, 5
    
    section Monitoring
    MonitoringTask (30s)     :mon, 0, 30
    MB8ARTStatusTask (5s)    :mb8, 0, 5
    
    section Relay Control
    RelayControlTask (cont)  :crit, rct, 0, 30
```

## Memory Architecture

```mermaid
graph TB
    subgraph "Shared Resources (via SRP)"
        SR[SensorReadings<br/>520 bytes]
        RR[RelayReadings<br/>24 bytes]
        SS[SystemSettings<br/>~200 bytes]
    end
    
    subgraph "Event Groups (24 bits each)"
        EG1[SYSTEM_STATE]
        EG2[BURNER_REQUEST]
        EG3[SENSOR]
        EG4[RELAY_REQUEST]
    end
    
    subgraph "Task Stacks"
        T1[HeatingControl<br/>3584 bytes]
        T2[WheaterControl<br/>3584 bytes]
        T3[BurnerControl<br/>4096 bytes]
        T4[RelayControl<br/>3584 bytes]
        T5[Monitoring<br/>4096 bytes]
    end
    
    SR -->|Mutex Protected| T1
    SR -->|Mutex Protected| T2
    SR -->|Mutex Protected| T3
    
    RR -->|Mutex Protected| T4
    RR -->|Read Only| T5
```

## BurnerRequestBits Encoding

```mermaid
graph TB
    subgraph "32-bit Burner Request Format"
        B0[Bit 0: HEATING_REQUEST]
        B1[Bit 1: WATER_REQUEST]
        B2[Bit 2: POWER_OFF]
        B3[Bit 3: POWER_HALF]
        B4[Bit 4: POWER_FULL]
        B5[Bit 5: EMERGENCY_STOP]
        B6[Bit 6: Reserved]
        B7[Bit 7: Reserved]
        B8[Bits 8-15: Target Temp<br/>0-255°C]
        B16[Bits 16-23: Reserved]
        B24[Bits 24-31: Reserved]
    end
    
    B0 --> EX1[Example: Heating 70°C]
    EX1 --> HEX1[0x00004601<br/>Bit 0 + Temp 70]
    
    B1 --> EX2[Example: Water 65°C]
    EX2 --> HEX2[0x00004102<br/>Bit 1 + Temp 65]
```

## Error Handling Flow

```mermaid
graph TD
    E1[Sensor Error] -->|Set Error Bit| ERR[ERROR_NOTIFICATION<br/>Event Group]
    E2[Relay Timeout] -->|Set Error Bit| ERR
    E3[Overtemperature] -->|Set Error Bit| ERR
    
    ERR --> MT[MonitoringTask]
    ERR --> BCT[BurnerControlTask]
    
    BCT -->|Emergency Stop| RCT[RelayControlTask]
    MT -->|Log & Report| MQTT[MQTT Task]
    
    style E1 fill:#faa,stroke:#333,stroke-width:2px
    style E2 fill:#faa,stroke:#333,stroke-width:2px
    style E3 fill:#faa,stroke:#333,stroke-width:2px
```