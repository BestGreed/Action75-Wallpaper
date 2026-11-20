use crate::diagnostics;
use action75_manager::graphics_adapters::{AdapterModel, list_adapter_models};
use eframe::egui;
use serde_json::Value;
use std::{
    collections::HashMap,
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
        mpsc::{self, RecvTimeoutError, TrySendError},
    },
    thread,
    time::{Duration, Instant},
};
use windows::{
    Win32::{
        Foundation::{CloseHandle, FILETIME, HANDLE},
        System::{
            Performance::{
                PDH_CSTATUS_NEW_DATA, PDH_CSTATUS_VALID_DATA, PDH_FMT_COUNTERVALUE,
                PDH_FMT_COUNTERVALUE_ITEM_W, PDH_FMT_DOUBLE, PDH_MORE_DATA, PdhAddEnglishCounterW,
                PdhCloseQuery, PdhCollectQueryData, PdhGetFormattedCounterArrayW,
                PdhGetFormattedCounterValue, PdhOpenQueryW,
            },
            SystemInformation::{GlobalMemoryStatusEx, MEMORYSTATUSEX},
            Threading::{
                ALL_PROCESSOR_GROUPS, GetActiveProcessorCount, GetProcessTimes, OpenProcess,
                PROCESS_QUERY_LIMITED_INFORMATION,
            },
        },
    },
    core::{PCWSTR, w},
};

pub const SAMPLE_INTERVAL: Duration = Duration::from_secs(1);

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PlayerProcesses {
    player_pid: u32,
    worker_pid: u32,
}

impl PlayerProcesses {
    fn new(player_pid: u32, worker_pid: u32) -> Self {
        Self {
            player_pid,
            worker_pid: if player_pid == 0 || player_pid == worker_pid {
                0
            } else {
                worker_pid
            },
        }
    }

    pub fn from_status(status: &Value) -> Self {
        if status["running"] != true {
            return Self::default();
        }
        let pid = |name| {
            status[name]
                .as_u64()
                .and_then(|pid| u32::try_from(pid).ok())
                .unwrap_or(0)
        };
        Self::new(pid("playerPid"), pid("nativeWorkerPid"))
    }

    pub fn player_pid(self) -> u32 {
        self.player_pid
    }

    fn contains(self, pid: u32) -> bool {
        pid != 0 && (pid == self.player_pid || pid == self.worker_pid)
    }

    // Publish both PIDs together so a sampler never pairs a new host with an old worker.
    fn pack(self) -> u64 {
        u64::from(self.worker_pid) << 32 | u64::from(self.player_pid)
    }

    fn unpack(pids: u64) -> Self {
        Self::new(pids as u32, (pids >> 32) as u32)
    }
}

#[derive(Clone, Copy)]
pub struct PhysicalMemory {
    pub used: u64,
    pub total: u64,
}

#[derive(Clone, Copy)]
pub struct PlayerLoad {
    pub cpu: Option<f32>,
    pub working_set: Option<u64>,
}

impl PlayerLoad {
    fn with_worker(self, worker: Option<Self>) -> Self {
        let Some(worker) = worker else {
            return self;
        };
        // A configured worker without a reading gives an incomplete total.
        Self {
            cpu: self
                .cpu
                .zip(worker.cpu)
                .map(|(player, worker)| (player + worker).min(100.)),
            working_set: self
                .working_set
                .zip(worker.working_set)
                .map(|(player, worker)| player.saturating_add(worker)),
        }
    }
}

#[derive(Clone, Copy, Default)]
pub struct GpuLoad {
    pub system: f32,
    pub player: f32,
}

#[derive(Default)]
pub struct GpuReadings {
    pub adapters: HashMap<u64, GpuLoad>,
}

pub struct Sample {
    pub at: Instant,
    pub cpu: Option<f32>,
    pub memory: Option<PhysicalMemory>,
    pub player: Option<PlayerLoad>,
    pub gpu: GpuReadings,
}

pub enum Reading {
    Hardware {
        processors: u32,
        adapters: Vec<AdapterModel>,
    },
    Sample(Sample),
}

pub struct Session {
    _stop_on_drop: mpsc::Sender<()>,
    player_processes: Arc<AtomicU64>,
    pub readings: mpsc::Receiver<Reading>,
}

impl Session {
    pub fn start(ctx: &egui::Context, processes: PlayerProcesses) -> std::io::Result<Self> {
        let (stop, wait) = mpsc::channel();
        let (output, readings) = mpsc::sync_channel(4);
        let player_processes = Arc::new(AtomicU64::new(processes.pack()));
        let current_processes = Arc::clone(&player_processes);
        let ctx = ctx.clone();
        thread::Builder::new()
            .name("action75-performance".into())
            .spawn(move || {
                let processors = unsafe { GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) }.max(1);
                let adapters = list_adapter_models().unwrap_or_default();
                if output
                    .send(Reading::Hardware {
                        processors,
                        adapters,
                    })
                    .is_err()
                {
                    return;
                }
                let mut sampler = Sampler {
                    processors,
                    previous_player: None,
                    previous_worker: None,
                    counters: PdhCounters::open(),
                };
                loop {
                    let at = Instant::now();
                    let processes =
                        PlayerProcesses::unpack(current_processes.load(Ordering::Relaxed));
                    let sample = sampler.collect(at, processes);
                    match output.try_send(Reading::Sample(sample)) {
                        Ok(()) => ctx.request_repaint(),
                        Err(TrySendError::Full(_)) => {}
                        Err(TrySendError::Disconnected(_)) => return,
                    }
                    let remaining = SAMPLE_INTERVAL.saturating_sub(at.elapsed());
                    if wait.recv_timeout(remaining) != Err(RecvTimeoutError::Timeout) {
                        return;
                    }
                }
            })?;
        Ok(Self {
            _stop_on_drop: stop,
            player_processes,
            readings,
        })
    }

    pub fn set_player(&self, processes: PlayerProcesses) {
        self.player_processes
            .store(processes.pack(), Ordering::Relaxed);
    }
}

struct Sampler {
    processors: u32,
    previous_player: Option<ProcessTime>,
    previous_worker: Option<ProcessTime>,
    counters: Option<PdhCounters>,
}

impl Sampler {
    fn collect(&mut self, at: Instant, processes: PlayerProcesses) -> Sample {
        let player = sample_process(
            processes.player_pid,
            at,
            &mut self.previous_player,
            self.processors,
        );
        let worker = sample_process(
            processes.worker_pid,
            at,
            &mut self.previous_worker,
            self.processors,
        );
        let (cpu, gpu) = self
            .counters
            .as_mut()
            .map(|counters| counters.read(processes))
            .unwrap_or_default();
        Sample {
            at,
            cpu,
            memory: physical_memory(),
            player: player.map(|load| load.with_worker(worker)),
            gpu,
        }
    }
}

fn sample_process(
    pid: u32,
    at: Instant,
    previous: &mut Option<ProcessTime>,
    processors: u32,
) -> Option<PlayerLoad> {
    if pid == 0 {
        *previous = None;
        return None;
    }
    let handle = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid) }.ok();
    let time = handle.and_then(|process| ProcessTime::read(process, pid, at));
    let cpu = time
        .zip(*previous)
        .and_then(|(current, previous)| current.usage_since(previous, processors));
    let working_set = handle.and_then(diagnostics::working_set_bytes);
    if let Some(handle) = handle {
        let _ = unsafe { CloseHandle(handle) };
    }
    *previous = time;
    Some(PlayerLoad { cpu, working_set })
}

fn physical_memory() -> Option<PhysicalMemory> {
    let mut state = MEMORYSTATUSEX {
        dwLength: size_of::<MEMORYSTATUSEX>() as u32,
        ..Default::default()
    };
    unsafe { GlobalMemoryStatusEx(&mut state) }.ok()?;
    (state.ullTotalPhys != 0).then_some(PhysicalMemory {
        used: state.ullTotalPhys.saturating_sub(state.ullAvailPhys),
        total: state.ullTotalPhys,
    })
}

#[derive(Clone, Copy)]
struct ProcessTime {
    pid: u32,
    created: u64,
    ticks: u64,
    at: Instant,
}

impl ProcessTime {
    fn read(handle: HANDLE, pid: u32, at: Instant) -> Option<Self> {
        let [mut created, mut exited, mut kernel, mut user] = [FILETIME::default(); 4];
        unsafe { GetProcessTimes(handle, &mut created, &mut exited, &mut kernel, &mut user) }
            .ok()?;
        Some(Self {
            pid,
            created: ticks(created),
            ticks: ticks(kernel).saturating_add(ticks(user)),
            at,
        })
    }

    fn usage_since(self, previous: Self, processors: u32) -> Option<f32> {
        if (self.pid, self.created) != (previous.pid, previous.created) {
            return None;
        }
        let secs = self.at.checked_duration_since(previous.at)?.as_secs_f64();
        let work = self.ticks.checked_sub(previous.ticks)? as f64 / 10_000_000.;
        (secs > 0.).then(|| (100. * work / (secs * f64::from(processors))).clamp(0., 100.) as f32)
    }
}

fn ticks(time: FILETIME) -> u64 {
    u64::from(time.dwHighDateTime) << 32 | u64::from(time.dwLowDateTime)
}

struct PdhCounters {
    query: isize,
    cpu: Option<isize>,
    engines: Option<isize>,
    buffer: Vec<PDH_FMT_COUNTERVALUE_ITEM_W>,
}

impl PdhCounters {
    fn open() -> Option<Self> {
        let mut query = 0;
        if unsafe { PdhOpenQueryW(PCWSTR::null(), 0, &mut query) } != 0 {
            return None;
        }
        let add = |path: PCWSTR| {
            let mut counter = 0;
            (unsafe { PdhAddEnglishCounterW(query, path, 0, &mut counter) } == 0).then_some(counter)
        };
        Some(Self {
            query,
            cpu: add(w!("\\Processor Information(_Total)\\% Processor Time")),
            engines: add(w!("\\GPU Engine(*)\\Utilization Percentage")),
            buffer: Vec::new(),
        })
    }

    fn read(&mut self, processes: PlayerProcesses) -> (Option<f32>, GpuReadings) {
        if unsafe { PdhCollectQueryData(self.query) } != 0 {
            return Default::default();
        }
        let cpu = self.cpu.and_then(|counter| {
            let mut data = PDH_FMT_COUNTERVALUE::default();
            let status =
                unsafe { PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, None, &mut data) };
            (status == 0)
                .then(|| formatted_value(&data))
                .flatten()
                .map(|v| v.min(100.))
        });
        let gpu = self
            .engines
            .map(|counter| self.read_engines(counter, processes));
        (cpu, gpu.unwrap_or_default())
    }

    fn read_engines(&mut self, counter: isize, processes: PlayerProcesses) -> GpuReadings {
        let (mut bytes, mut count) = (0, 0);
        let status = unsafe {
            PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &mut bytes, &mut count, None)
        };
        if status != PDH_MORE_DATA || bytes == 0 {
            return GpuReadings::default();
        }
        let slots = (bytes as usize).div_ceil(size_of::<PDH_FMT_COUNTERVALUE_ITEM_W>());
        self.buffer.resize_with(slots, Default::default);
        let status = unsafe {
            PdhGetFormattedCounterArrayW(
                counter,
                PDH_FMT_DOUBLE,
                &mut bytes,
                &mut count,
                Some(self.buffer.as_mut_ptr()),
            )
        };
        if status != 0 {
            return GpuReadings::default();
        }
        let Some(items) = self.buffer.get(..count as usize) else {
            return GpuReadings::default();
        };
        gpu_readings(
            items.iter().filter_map(|item| {
                let value = formatted_value(&item.FmtValue)?;
                if item.szName.is_null() {
                    return None;
                }
                // PDH owns the null-terminated instance strings inside this buffer.
                let name = unsafe { item.szName.to_string() }.ok()?;
                let (pid, engine) = parse_engine(&name)?;
                Some((pid, engine, value))
            }),
            processes,
        )
    }
}

impl Drop for PdhCounters {
    fn drop(&mut self) {
        unsafe { PdhCloseQuery(self.query) };
    }
}

fn formatted_value(data: &PDH_FMT_COUNTERVALUE) -> Option<f32> {
    if !matches!(data.CStatus, PDH_CSTATUS_VALID_DATA | PDH_CSTATUS_NEW_DATA) {
        return None;
    }
    // The query always asks for PDH_FMT_DOUBLE.
    let value = unsafe { data.Anonymous.doubleValue };
    (value.is_finite() && value >= 0.).then_some(value as f32)
}

#[derive(Clone, Copy, PartialEq, Eq, Hash)]
struct Engine {
    luid: u64,
    physical: u32,
    index: u32,
}

fn parse_engine(name: &str) -> Option<(u32, Engine)> {
    let mut parts = name.split('_');
    let pid: u32 = (parts.next()? == "pid")
        .then_some(parts.next()?)?
        .parse()
        .ok()?;
    if parts.next()? != "luid" {
        return None;
    }
    let high = u32::from_str_radix(parts.next()?.strip_prefix("0x")?, 16).ok()?;
    let low = u32::from_str_radix(parts.next()?.strip_prefix("0x")?, 16).ok()?;
    if parts.next()? != "phys" {
        return None;
    }
    let physical = parts.next()?.parse().ok()?;
    if parts.next()? != "eng" {
        return None;
    }
    let index = parts.next()?.parse().ok()?;
    Some((
        pid,
        Engine {
            luid: u64::from(high) << 32 | u64::from(low),
            physical,
            index,
        },
    ))
}

fn gpu_readings(
    rows: impl Iterator<Item = (u32, Engine, f32)>,
    processes: PlayerProcesses,
) -> GpuReadings {
    let mut engines = HashMap::<Engine, GpuLoad>::new();
    for (pid, engine, value) in rows {
        let load = engines.entry(engine).or_default();
        load.system += value;
        if processes.contains(pid) {
            load.player += value;
        }
    }
    // Each engine is independent. Take the busiest one for each adapter and scope.
    let merge = |target: &mut GpuLoad, load: GpuLoad| {
        target.system = target.system.max(load.system.min(100.));
        target.player = target.player.max(load.player.min(100.));
    };
    let mut readings = GpuReadings::default();
    for (engine, load) in engines {
        merge(readings.adapters.entry(engine.luid).or_default(), load);
    }
    readings
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn status_tracks_only_the_connected_player_and_distinct_worker() {
        let pair = PlayerProcesses::from_status(
            &json!({"running": true, "playerPid": 51, "nativeWorkerPid": 52}),
        );
        assert_eq!(pair, PlayerProcesses::new(51, 52));
        assert_eq!(PlayerProcesses::unpack(pair.pack()), pair);
        for status in [
            json!({"running": true, "playerPid": 51}),
            json!({"running": true, "playerPid": 51, "nativeWorkerPid": 0}),
            json!({"running": true, "playerPid": 51, "nativeWorkerPid": 51}),
            json!({"running": true, "playerPid": 51, "nativeWorkerPid": 4_294_967_296u64}),
        ] {
            assert_eq!(
                PlayerProcesses::from_status(&status),
                PlayerProcesses::new(51, 0)
            );
        }
        for status in [
            json!({"running": false, "playerPid": 51, "nativeWorkerPid": 52}),
            json!({"running": true, "nativeWorkerPid": 52}),
        ] {
            assert_eq!(
                PlayerProcesses::from_status(&status),
                PlayerProcesses::default()
            );
        }
    }

    #[test]
    fn player_load_requires_both_readings_when_a_worker_is_configured() {
        let player = PlayerLoad {
            cpu: Some(12.5),
            working_set: Some(60 << 20),
        };
        let worker = PlayerLoad {
            cpu: Some(25.),
            working_set: Some(90 << 20),
        };
        let combined = player.with_worker(Some(worker));
        assert_eq!(combined.cpu, Some(37.5));
        assert_eq!(combined.working_set, Some(150 << 20));
        let worker_warming_up = player.with_worker(Some(PlayerLoad {
            cpu: None,
            ..worker
        }));
        assert_eq!(worker_warming_up.cpu, None);
        assert_eq!(worker_warming_up.working_set, Some(150 << 20));
        let unmeasured_worker = player.with_worker(Some(PlayerLoad {
            cpu: None,
            working_set: None,
        }));
        assert_eq!(unmeasured_worker.cpu, None);
        assert_eq!(unmeasured_worker.working_set, None);
        let player_only = player.with_worker(None);
        assert_eq!(player_only.cpu, player.cpu);
        assert_eq!(player_only.working_set, player.working_set);
    }

    #[test]
    fn gpu_combines_processes_on_the_same_engine_and_compares_independent_engines() {
        let rows = [
            (
                "pid_51_luid_0x00000000_0x00001002_phys_0_eng_0_engtype_3D",
                37.,
            ),
            (
                "pid_52_luid_0x00000000_0x00001002_phys_0_eng_0_engtype_3D",
                20.,
            ),
            (
                "pid_99_luid_0x00000000_0x00001002_phys_0_eng_0_engtype_3D",
                29.,
            ),
            (
                "pid_51_luid_0x00000000_0x00001002_phys_0_eng_1_engtype_Copy",
                42.,
            ),
            (
                "pid_52_luid_0x00000000_0x00001002_phys_0_eng_2_engtype_Compute",
                46.,
            ),
            (
                "pid_51_luid_0x00000000_0x00001002_phys_1_eng_0_engtype_3D",
                12.,
            ),
            (
                "pid_99_luid_0x00000000_0x00001003_phys_0_eng_0_engtype_3D",
                75.,
            ),
        ];
        let readings = |processes| {
            gpu_readings(
                rows.iter().map(|&(name, value)| {
                    let (pid, engine) = parse_engine(name).unwrap();
                    (pid, engine, value)
                }),
                processes,
            )
        };
        let result = readings(PlayerProcesses::new(51, 52));
        assert_eq!(result.adapters[&0x1002].system, 86.);
        assert_eq!(result.adapters[&0x1003].player, 0.);
        assert_eq!(result.adapters[&0x1002].player, 57.);
        for worker in [0, 51] {
            let player_only = readings(PlayerProcesses::new(51, worker));
            assert_eq!(player_only.adapters[&0x1002].player, 42.);
            assert_eq!(player_only.adapters[&0x1002].system, 86.);
        }
        assert_eq!(
            readings(PlayerProcesses::new(0, 52)).adapters[&0x1002].player,
            0.
        );
        assert!(parse_engine("_Total").is_none());
        assert!(
            gpu_readings(std::iter::empty(), PlayerProcesses::new(51, 52))
                .adapters
                .is_empty()
        );
    }

    #[test]
    fn player_cpu_compares_only_the_same_process_and_accounts_for_all_cores() {
        let before = ProcessTime {
            pid: 10,
            created: 100,
            ticks: 1_000_000,
            at: Instant::now(),
        };
        let after = ProcessTime {
            ticks: 11_000_000,
            at: before.at + SAMPLE_INTERVAL,
            ..before
        };
        assert_eq!(after.usage_since(before, 8), Some(12.5));
        assert_eq!(
            ProcessTime {
                created: 101,
                ..after
            }
            .usage_since(before, 8),
            None
        );
    }
}
