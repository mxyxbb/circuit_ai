# CircuitAI Phase 1 — 管道打通 + 波形显示

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 实现 CMake 构建系统 → 后端仿真引擎 → 前端 Scope 波形显示的完整数据管道，使用 RC 低通滤波器端到端验证。

**Architecture:** MVVM 三层分离。Backend (静态库) 包含 MNA 求解器、器件模型、网表解析器和多线程仿真器。Frontend (静态库) 包含 ScopeModel、ViewModel 和 ImGui/ImPlot 视图。App 入口链接两者。仿真线程通过 SPSC 无锁环形缓冲区向 UI 推送数据。

**Tech Stack:** C++17, CMake 3.20+, Eigen (线性代数), ImGui + ImPlot (UI), GLFW + OpenGL (窗口/渲染), vcpkg (依赖管理)

**Reference:** 所有接口定义见 `docs/代码架构设计.md` 和 `docs/需求规格说明书.md`

---

## Task 1: CMake 构建系统

**Files:**
- Overwrite: `CMakeLists.txt`
- Overwrite: `src/CMakeLists.txt`
- Overwrite: `src/backend/CMakeLists.txt`
- Overwrite: `src/frontend/CMakeLists.txt`
- Create: `src/app/CMakeLists.txt`

**Step 1:** Write all 5 CMake files per architecture doc section 6.

**Step 2:** Configure build (dry-run, no source yet). Verify CMake can resolve dependencies.

**Step 3:** Commit. `feat: CMake build system with vcpkg integration`

---

## Task 2: Backend 公共数据类型

**Files:**
- Create: `src/backend/common/sim_types.h`
- Create: `src/backend/common/ring_buffer.h`

**Step 1:** Write `sim_types.h` — SimSample, SignalInfo, SimConfig structs.

**Step 2:** Write `ring_buffer.h` — SPSCRingBuffer template (lock-free, power-of-2 capacity).

**Step 3:** Commit. `feat(backend): simulation data types and SPSC ring buffer`

---

## Task 3: MNA 求解器

**Files:**
- Overwrite: `src/backend/engine/mna_solver.h`
- Overwrite: `src/backend/engine/mna_solver.cpp`

**Step 1:** Write `mna_solver.h` — class declaration with init/clear/stamp/solve interface.

**Step 2:** Write `mna_solver.cpp` — implement all methods. Key: node i (1-based) → matrix index i-1; extra var j → matrix index n+j. Use `Eigen::PartialPivLU` for solve.

**Step 3:** Commit. `feat(backend): MNA solver with Eigen`

---

## Task 4: 器件基类 + 无源器件

**Files:**
- Overwrite: `src/backend/components/base_component.h`
- Create: `src/backend/components/passives/resistor.h`
- Create: `src/backend/components/passives/capacitor.h`
- Create: `src/backend/components/passives/inductor.h`
- Create: `src/backend/components/passives/transformer.h`

**Step 1:** Write `base_component.h` — abstract interface with stamp/updateState/commitHistory.

**Step 2:** Write `resistor.h` — header-only, stampConductance with g=1/R.

**Step 3:** Write `capacitor.h` — backward Euler companion model, G_eq=C/dt, I_hist.

**Step 4:** Write `inductor.h` — backward Euler companion model with extra current variable.

**Step 5:** Write `transformer.h` — ideal transformer with 2 extra variables, turns ratio constraint.

**Step 6:** Commit. `feat(backend): passive components (R, C, L, Transformer)`

---

## Task 5: 激励源器件

**Files:**
- Create: `src/backend/components/sources/voltage_source.h`
- Create: `src/backend/components/sources/current_source.h`
- Create: `src/backend/components/sources/square_wave_source.h`
- Create: `src/backend/components/sources/step_source.h`

**Step 1:** Write `voltage_source.h` — base with virtual `voltageAt(t)`, extra variable for branch current.

**Step 2:** Write `current_source.h` — direct current injection to rhs vector.

**Step 3:** Write `square_wave_source.h` — inherits VoltageSource, overrides `voltageAt(t)`.

**Step 4:** Write `step_source.h` — inherits VoltageSource, overrides `voltageAt(t)`.

**Step 5:** Commit. `feat(backend): source components (Vdc, Idc, Square, Step)`

---

## Task 6: 半导体器件

**Files:**
- Create: `src/backend/components/semiconductors/ideal_diode.h`
- Create: `src/backend/components/semiconductors/ideal_switch.h`

**Step 1:** Write `ideal_diode.h` — PWL model with ON/OFF state machine, R_on/R_off.

**Step 2:** Write `ideal_switch.h` — gate-controlled PWL model.

**Step 3:** Commit. `feat(backend): semiconductor devices (Diode, Switch)`

---

## Task 7: 电路拓扑容器

**Files:**
- Create: `src/backend/circuit/circuit.h`

**Step 1:** Write `circuit.h` — component storage, name index, node count tracking.

**Step 2:** Commit. `feat(backend): circuit topology container`

---

## Task 8: 网表解析器

**Files:**
- Create: `src/backend/circuit/netlist_parser.h`
- Create: `src/backend/circuit/netlist_parser.cpp`

**Step 1:** Write `netlist_parser.h` — ParseResult struct, NetlistParser class declaration.

**Step 2:** Write `netlist_parser.cpp` — line-by-line parser supporting R/C/L/V/I/D/S/TX directives, .TRAN, .PROBE, .END. Include value suffix parsing (k/m/u/n/p/Meg).

**Step 3:** Commit. `feat(backend): SPICE-like netlist parser`

---

## Task 9: 仿真调度器

**Files:**
- Overwrite: `src/backend/engine/simulator.h`
- Overwrite: `src/backend/engine/simulator.cpp`

**Step 1:** Write `simulator.h` — multi-threaded simulator with start/pause/reset/stop, SPSC buffer.

**Step 2:** Write `simulator.cpp` — setup() builds solver from Circuit, runLoop() in separate thread, step() executes MNA solve loop with convergence check.

**Step 3:** Commit. `feat(backend): multi-threaded simulation scheduler`

---

## Task 10: 前端数据缓冲

**Files:**
- Create: `src/frontend/view_model/scrolling_buffer.h`

**Step 1:** Write `scrolling_buffer.h` — fixed-capacity ring buffer with ImPlot-compatible getXData/getYData/getCount.

**Step 2:** Commit. `feat(frontend): ImPlot scrolling buffer`

---

## Task 11: Scope 数据模型

**Files:**
- Create: `src/frontend/view_model/scope_model.h`
- Create: `src/frontend/view_model/scope_model.cpp`

**Step 1:** Write `scope_model.h` — MuxEntry, PlotArea, ScopeModel classes.

**Step 2:** Write `scope_model.cpp` — insert/remove plot, add/remove signal, color rotation.

**Step 3:** Commit. `feat(frontend): Scope model with multi-plot support`

---

## Task 12: MainViewModel

**Files:**
- Create: `src/frontend/view_model/main_view_model.h`
- Create: `src/frontend/view_model/main_view_model.cpp`

**Step 1:** Write `main_view_model.h` — ViewModel with Simulator ownership, ScopeModel, command/data interfaces.

**Step 2:** Write `main_view_model.cpp` — loadNetlist, play/pause/reset, update() dispatches samples to ScopeModel.

**Step 3:** Commit. `feat(frontend): MainViewModel bridging engine and scope`

---

## Task 13: 视图层

**Files:**
- Create: `src/frontend/views/base_view.h`
- Overwrite: `src/frontend/views/main_view.h`
- Overwrite: `src/frontend/views/main_view.cpp`
- Create: `src/frontend/views/scope_view.h`
- Create: `src/frontend/views/scope_view.cpp`
- Create: `src/frontend/views/settings_view.h`
- Create: `src/frontend/views/settings_view.cpp`
- Create: `src/frontend/views/palette_view.h`
- Create: `src/frontend/views/palette_view.cpp`
- Create: `src/frontend/views/schematic_view.h`
- Create: `src/frontend/views/schematic_view.cpp`

**Step 1:** Write `base_view.h` — abstract BaseView with render(vm) and title.

**Step 2:** Write `scope_view.h/.cpp` — multi-plot ImPlot rendering, right-click context menu for Insert/Delete Plot and Add Signal.

**Step 3:** Write `settings_view.h/.cpp` — netlist file input, Run/Pause/Reset buttons, dt/t_end controls.

**Step 4:** Write `palette_view.h/.cpp` and `schematic_view.h/.cpp` — stub views.

**Step 5:** Write `main_view.h/.cpp` — DockSpace layout, owns all sub-views.

**Step 6:** Commit. `feat(frontend): all views with Scope multi-plot rendering`

---

## Task 14: 程序入口

**Files:**
- Overwrite: `src/app/main.cpp`

**Step 1:** Write `main.cpp` — GLFW/ImGui/ImPlot init, MainViewModel + MainView creation, main render loop.

**Step 2:** Commit. `feat: application entry point with render loop`

---

## Task 15: 测试网表 + 端到端验证

**Files:**
- Create: `netlists/rc_filter.cir`
- Create: `netlists/rl_circuit.cir`

**Step 1:** Write test netlist files.

**Step 2:** Build the entire project. Fix compilation errors.

**Step 3:** Run circuit_ai, load RC filter netlist, verify waveform display shows correct charging curve.

**Step 4:** Commit. `test: RC and RL test netlists for end-to-end verification`
