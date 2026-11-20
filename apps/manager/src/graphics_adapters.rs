use crate::storage::GpuPreference;
use windows::{
    Win32::Graphics::Dxgi::{
        CreateDXGIFactory1, DXGI_ADAPTER_DESC1, DXGI_ADAPTER_FLAG_SOFTWARE,
        DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, DXGI_GPU_PREFERENCE_MINIMUM_POWER, IDXGIAdapter1,
        IDXGIFactory1, IDXGIFactory6,
    },
    core::{Interface, Result},
};

pub struct GraphicsAdapters {
    pub names: Vec<String>,
    high_performance: Option<usize>,
    minimum_power: Option<usize>,
}

pub struct AdapterModel {
    pub luids: Vec<u64>,
    pub name: String,
}

pub fn list_adapter_models() -> Result<Vec<AdapterModel>> {
    let factory: IDXGIFactory1 = unsafe { CreateDXGIFactory1()? };
    let mut models: Vec<AdapterModel> = Vec::new();
    for desc in hardware_descriptions(|index| unsafe { factory.EnumAdapters1(index) }) {
        let luid =
            u64::from(desc.AdapterLuid.HighPart as u32) << 32 | u64::from(desc.AdapterLuid.LowPart);
        let name = adapter_name(&desc);
        // Some drivers expose the same model through several logical adapters.
        if let Some(model) = models.iter_mut().find(|model| model.name == name) {
            if !model.luids.contains(&luid) {
                model.luids.push(luid);
            }
        } else {
            models.push(AdapterModel {
                luids: vec![luid],
                name,
            });
        }
    }
    Ok(models)
}

impl GraphicsAdapters {
    pub fn detect() -> Result<Self> {
        let factory: IDXGIFactory1 = unsafe { CreateDXGIFactory1()? };
        let mut names = Vec::new();
        for description in hardware_descriptions(|index| unsafe { factory.EnumAdapters1(index) }) {
            let name = adapter_name(&description);
            // Drivers can expose several DXGI adapters with the same hardware model.
            if !names.contains(&name) {
                names.push(name);
            }
        }
        let ranked = factory.cast::<IDXGIFactory6>().ok();
        let preferred_index = |preference| {
            let factory = ranked.as_ref()?;
            let first = hardware_descriptions(|index| unsafe {
                factory.EnumAdapterByGpuPreference(index, preference)
            })
            .next()?;
            let preferred = adapter_name(&first);
            names.iter().position(|name| *name == preferred)
        };
        Ok(Self {
            high_performance: preferred_index(DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE),
            minimum_power: preferred_index(DXGI_GPU_PREFERENCE_MINIMUM_POWER),
            names,
        })
    }

    pub fn preferred(&self, preference: GpuPreference) -> Option<&str> {
        let index = match preference {
            GpuPreference::Discrete => self.high_performance,
            GpuPreference::Integrated => self.minimum_power,
        }?;
        self.names.get(index).map(String::as_str)
    }

    pub fn other_names(&self) -> impl Iterator<Item = &str> {
        self.names.iter().enumerate().filter_map(|(index, name)| {
            (Some(index) != self.high_performance && Some(index) != self.minimum_power)
                .then_some(name.as_str())
        })
    }
}

fn hardware_descriptions(
    mut adapter_at: impl FnMut(u32) -> Result<IDXGIAdapter1>,
) -> impl Iterator<Item = DXGI_ADAPTER_DESC1> {
    (0..)
        .map_while(move |index| adapter_at(index).ok())
        .filter_map(|adapter| unsafe { adapter.GetDesc1().ok() })
        .filter(|desc| desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE.0 as u32 == 0)
}

fn adapter_name(desc: &DXGI_ADAPTER_DESC1) -> String {
    let end = desc
        .Description
        .iter()
        .position(|&unit| unit == 0)
        .unwrap_or(desc.Description.len());
    String::from_utf16_lossy(&desc.Description[..end])
}
